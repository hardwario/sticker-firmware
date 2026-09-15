/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_nfc.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_led.h" /* NFC interaction LED signalling */
#include "app_nfc_parser.h"
#include "app_settings.h"
#include "app_version.h"
#include "app_log.h"

/* Nanopb includes */
#include <pb_decode.h>
#include <pb_encode.h>
#include "src/app_config.pb.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

/* NFC channel AES-CCM (RFC 3610), HW AES / soft-SE backed (#261) */
#include "app_ccm.h"

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(app_nfc, LOG_LEVEL_DBG);

/* M-11: the NFC command/config channel may only run in plaintext on a debug /
 * validation build. In a release build the boot warning is a LOG_WRN that
 * preprocesses away (no CONFIG_LOG), so a release with encryption off would ship
 * a fully unencrypted NFC channel with no runtime signal. Fail the build instead
 * — plaintext NFC requires CONFIG_FW_DEBUG. */
BUILD_ASSERT(IS_ENABLED(CONFIG_APP_NFC_ENCRYPTION) || IS_ENABLED(CONFIG_FW_DEBUG),
	     "plaintext NFC (CONFIG_APP_NFC_ENCRYPTION=n) is only allowed with CONFIG_FW_DEBUG");

#if defined(CONFIG_SHELL)
/* When set (by `nfc check`), nfc_check_locked()/parser_callback emit a human-
 * readable trace to this shell: what was read & decoded off the tag, how the
 * firmware reacted, and what it wrote back. NULL for boot/poll-thread checks
 * (those only log over RTT). Set/cleared around the app_nfc_check() call. */
static const struct shell *m_report_sh;
#define NFC_REPORT(...)                                                                            \
	do {                                                                                       \
		if (m_report_sh) {                                                                 \
			shell_print(m_report_sh, __VA_ARGS__);                                     \
		}                                                                                  \
	} while (0)
#define NFC_REPORT_HEX(label, data, len)                                                           \
	do {                                                                                       \
		if (m_report_sh) {                                                                 \
			shell_print(m_report_sh, label);                                           \
			shell_hexdump(m_report_sh, (data), (len));                                 \
		}                                                                                  \
	} while (0)

static const char *cmd_action_str(enum app_cmd_action a)
{
	switch (a) {
	case APP_CMD_ACTION_NONE:
		return "none";
	case APP_CMD_ACTION_SETTINGS_SAVE:
		return "save+reboot";
	case APP_CMD_ACTION_REBOOT:
		return "reboot";
	case APP_CMD_ACTION_DEVICE_RESET:
		return "device-reset";
	case APP_CMD_ACTION_FACTORY_RESET:
		return "factory-reset";
	case APP_CMD_ACTION_VENDOR_RESET:
		return "vendor-reset";
	case APP_CMD_ACTION_SECRET_KEY_SAVE:
		return "secret-key-save+reboot";
	case APP_CMD_ACTION_ENTER_CALIBRATION:
		return "enter-calibration";
	case APP_CMD_ACTION_LRW_RESET:
		return "lrw-reset+reboot";
	case APP_CMD_ACTION_LRW_JOIN:
		return "lrw-join";
	default:
		return "?";
	}
}
#else
#define NFC_REPORT(...)                  ((void)0)
#define NFC_REPORT_HEX(label, data, len) ((void)0)
#endif

/* Poll-thread diagnostics for app_nfc. The debug build pins
 * CONFIG_LOG_MAX_LEVEL=2, so LOG_INF/LOG_DBG are compiled out and only
 * LOG_WRN/LOG_ERR reach RTT — route these through LOG_WRN so they stay
 * visible. */
#define NFC_DBG(...) LOG_WRN(__VA_ARGS__)

#define ST25DV_I2C_ADDR_E0 0x53
/* System/dynamic register device address. With user memory at 0x53 (E2=1),
 * the system area is at 0x57 (the prior 0x55 was the E2=0 value and NACKed). */
#define ST25DV_I2C_ADDR_E1 0x57

#define ST25DV_MAX_SEQ_WRITE_BYTES 256
#define ST25DV_INT_PAGE_BYTES      4
#define ST25DV_TW_MS_PER_PAGE      5

#define ST25DV_USER_MEM_SIZE 512

/* Dynamic register IT_STS_Dyn (device E0, addr 0x2005): interrupt status,
 * read-clears. Non-zero => RF activity since last read (field change / RF
 * write / etc.). Used to skip the full 512 B read when nothing happened. */
#define ST25DV_IT_STS_DYN  0x2005
#define ST25DV_IT_RF_WRITE 0x80 /* IT_STS_Dyn bit7: RF wrote to the EEPROM */

/* Dynamic register EH_CTRL_Dyn (device E0, addr 0x2002): bit2 FIELD_ON reports
 * whether an RF field is currently present. Dynamic registers live in the
 * dual-port area and are safe to read while RF is active (unlike the 512 B
 * user-memory EEPROM, whose reads/writes collide with RF on the shared i2c1 bus
 * and can wedge it). The poll gates EEPROM access on this bit so the firmware
 * only touches the tag while the field is off — see nfc_wait_field_off(). */
#define ST25DV_EH_CTRL_DYN 0x2002
#define ST25DV_FIELD_ON    0x04

/* Bound the wait for the RF field to clear before an EEPROM access. The phone's
 * protocol drops the field for ~1500 ms after writing a command so the firmware
 * can read/answer cleanly; wait a little longer than that, polling the FIELD_ON
 * bit. Only short dynamic-register reads happen during the wait, so the shared
 * i2c1 bus stays free for the sensors. */
#define NFC_FIELD_OFF_WAIT_MS 1800
#define NFC_FIELD_POLL_MS     20

/* ST25DV (non-C, IC_REF 0x24) GPO configuration. Bit positions per the ST driver
 * st25dv_reg.h. The STATIC GPO register (0x0000, E1 0x57, EEPROM) needs an open
 * I2C security session (present password) to write; the DYNAMIC GPO_CTRL_Dyn
 * (0x2000, E0 0x53, volatile) mirrors the same bit layout and takes effect at
 * runtime. GPO_EN (bit7) is the MASTER output enable — WITHOUT it the GPO pin
 * never pulses regardless of the event bits. The event bits: RF_WRITE (bit6)
 * reports RF EEPROM writes (also in IT_STS_Dyn), FIELD_CHANGE (bit3) pulses on RF
 * field on/off. */
#define ST25DV_GPO_REG           0x0000
#define ST25DV_GPO_CTRL_DYN_REG  0x2000
#define ST25DV_GPO_EN            0x80
#define ST25DV_GPO_RF_WRITE_EN   0x40
#define ST25DV_GPO_RF_PUT_MSG_EN 0x10 /* pulse when RF wrote a mailbox message (FTM) */
#define ST25DV_GPO_FIELD_EN      0x08
/* Master enable + all event sources. GPO_EN MUST be included or the pin is mute.
 * RF_PUT_MSG_EN wakes the poll thread the moment the phone drops a mailbox
 * request (#313); the field/EEPROM events keep the pre-mailbox behaviour. */
#define ST25DV_GPO_WANT                                                                            \
	(ST25DV_GPO_EN | ST25DV_GPO_RF_WRITE_EN | ST25DV_GPO_RF_PUT_MSG_EN | ST25DV_GPO_FIELD_EN)

/* Fast Transfer Mode (FTM) mailbox — a 256 B dual-port RAM the RF reader and
 * this I2C host exchange messages through WHILE THE RF FIELD IS ON (datasheet
 * DS10925 §5.1), which is what the single-port user EEPROM can never do. The
 * static MB_MODE bit (E1 EEPROM, needs the I2C password) only *authorises* FTM
 * and is set once per device at boot; the dynamic MB_EN bit (E0, no password,
 * writable from RF too) turns it on for a session — the phone sets it, we clear
 * it. While MB_EN=1 every EEPROM write (user or system) is refused by the chip
 * (I2C NACK / RF error 0Fh), so MB_EN must be 0 whenever we touch the EEPROM. */
#define ST25DV_MB_MODE_REG       0x000D /* static, E1: bit0 = FTM authorised */
#define ST25DV_MB_MODE_EN        0x01
#define ST25DV_MB_CTRL_DYN       0x2006 /* dynamic, E0 */
#define ST25DV_MB_CTRL_MB_EN     0x01   /* bit0: mailbox enabled (RW from RF and I2C) */
#define ST25DV_MB_CTRL_HOST_PUT  0x02   /* bit1: I2C (we) put a message, RF has not read it */
#define ST25DV_MB_CTRL_RF_PUT    0x04   /* bit2: RF put a message, we have not read it */
#define ST25DV_MB_CTRL_HOST_MISS 0x10   /* bit4: we missed an RF message (MB_WDG) */
#define ST25DV_MB_CTRL_RF_MISS   0x20   /* bit5: RF missed our message (MB_WDG) */
#define ST25DV_MB_LEN_DYN        0x2007 /* dynamic, E0: message length - 1 */
#define ST25DV_MB_RAM            0x2008 /* dynamic, E0: 256 B mailbox RAM */
#define ST25DV_MB_RAM_SIZE       256
#define ST25DV_VCC_ON            0x08 /* EH_CTRL_Dyn bit3: VCC present (LPD low) */
#define ST25DV_I2C_PWD_REG       0x0900

/* NFC Forum external type (TNF=0x04, urn:nfc:ext:) records carry the functional
 * protocol (cmd/rsp/ack/clm). Short type names ("hio.stck:<kind>") instead of full
 * MIME media-types save ST25DV user memory (512 B total) — leaving more room for
 * the encrypted config payload — while staying typed/filterable: Web NFC exposes
 * them as record.recordType. The resting identity record (inf) is the one
 * exception: it is a MIME media-type record (TNF 0x02, see below). */
#define NDEF_TNF_EXT 0x04

/* Resting identity record `hio.stck:inf` (NFC Forum external type, TNF 0x04):
 * ASCII "<serial>:<config_ver>:<nonce_hi>" ("%010u:%02X:%08X"). Readable by any
 * NFC reader; the phone takes serial + nonce high-water from it before its first
 * encrypted mailbox command. The "<serial>:<config_ver>" prefix is format-stable
 * forever, so a phone of any generation can read those two fields and then decide
 * how to parse the rest. FW version / build type / flags are intentionally NOT
 * here — obtain them via GetInfo. (Was a MIME record for Android tap-to-launch,
 * #298; dropped with the mailbox channel, #313 D6 — the user opens the app.)
 * Interactive commands no longer travel over NDEF at all: they use the ST25DV FTM
 * mailbox (see mb_serve_locked). */
#define NDEF_INFO_TYPE "hio.stck:inf"

#define NDEF_CLAIM_TYPE "hio.stck:clm"

/* NFC Forum Type 5 Capability Container (4-byte form) for ST25DV04K:
 *   [0] 0xE1 magic, [1] 0x40 mapping v1.0 + read/write,
 *   [2] MLEN = user_memory / 8 = 512/8 = 0x40, [3] 0x01 (MBREAD feature).
 * A phone's Web NFC (Type 5) reader requires a valid CC at offset 0 before the
 * NDEF TLV; without it the tag reads as unformatted/empty over RF. */
#define ST25DV_CC0 0xE1
#define ST25DV_CC1 0x40
#define ST25DV_CC2 (ST25DV_USER_MEM_SIZE / 8)
#define ST25DV_CC3 0x01

/* Single shared 512-byte scratch buffer for all ST25DV memory access. Always
 * used while holding m_lock, which serialises app_nfc_check() (main loop) and
 * the `nfc` shell commands against each other on the I2C bus and LPD pin. */
static uint8_t m_buf[ST25DV_USER_MEM_SIZE];
static K_MUTEX_DEFINE(m_lock);

static const struct gpio_dt_spec m_lpd = GPIO_DT_SPEC_GET(DT_NODELABEL(lpd), gpios);

/* ST25DV GPO interrupt line (PB12). Asserts on the RF events enabled in the GPO
 * config (RF write, field change, ...), letting us wake on demand instead of
 * polling. Handled directly here. */
static const struct gpio_dt_spec m_gpo = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), nfc_gpo_gpios);
static struct gpio_callback m_gpo_cb;

/* Given by the GPO ISR on each RF event; the NFC poll thread sleeps on it so the
 * CPU only wakes to service the tag when the phone is actually doing something
 * (max count 1 — bursts coalesce into one wake, which is fine). */
static K_SEM_DEFINE(m_gpo_sem, 0, 1);

/* Keep-awake window for an NFC exchange. Servicing one encrypted command is many
 * back-to-back i2c1 transfers with short field-off polls (k_sleep) between them;
 * on the release build the CPU would otherwise drop into Stop2 in those gaps, and
 * a Stop2 landing mid-operation corrupts the i2c1 (ST25DV) tag I/O so the command
 * never gets a reply — release-only (debug runs PM=n and never sleeps). The GPO
 * ISR fires on RF field-on (before the phone's command write even lands) and on
 * every RF write; each event (re)arms this window, during which a
 * SUSPEND_TO_IDLE policy lock keeps the CPU out of Stop2. After NFC_AWAKE_WINDOW_MS
 * of RF quiet the lock is dropped and the sticker returns to deep sleep. Kept
 * short: RF events during an exchange are <2 s apart so they re-arm it, so the
 * window only needs to outlast that gap plus a small margin; a longer tail just
 * burns ~mA (CPU held awake) after the phone has already left. Idle current is
 * unchanged (the lock is never taken without an RF event). */
#define NFC_AWAKE_WINDOW_MS 5000
static atomic_t m_awake_held; /* 1 while the SUSPEND_TO_IDLE lock is held */
static void nfc_awake_timeout(struct k_timer *timer);
static K_TIMER_DEFINE(m_awake_timer, nfc_awake_timeout, NULL);

/* --- NFC interaction LED signalling --------------------------------------
 * Guides an operator through an NFC exchange:
 *   phone detected (RF field / GPO)      -> green solid
 *   command being serviced               -> fast green blink
 *   command rejected (auth/nonce, #315)  -> fast red blink, then off
 *   response written, waiting for phone   -> green + yellow solid
 *   response consumed / session quiet     -> LED off
 * The "processing"/"rejected" blink runs on a k_timer so it never blocks the NFC
 * critical path. app_led_set is a plain gpio write (ISR-safe); k_timer start/stop
 * are ISR-safe too, so these may be called from the GPO ISR / timer handlers.
 * One timer drives whichever channel the current state blinks (m_led_blink_ch),
 * and every state helper leaves the two channels it does not use turned off, so
 * a state change can never blend into a colour of its own (red + green = orange). */
#define NFC_LED_BLINK_MS 90

/* How long the rejection blink is held (#315). Long enough to be unmistakable to
 * whoever is holding the phone against the sticker, and self-limiting: unlike the
 * green processing blink it is not left for the RF-quiet backstop to clear, because
 * the boot-staged path (app_nfc_check() from main(), no RF field and therefore no
 * awake window running) has no backstop and would otherwise blink forever. */
#define NFC_LED_REJECT_MS 2000

static bool m_led_blink_on;
static enum app_led_channel m_led_blink_ch = APP_LED_CHANNEL_G;
static void nfc_led_blink_timer(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	m_led_blink_on = !m_led_blink_on;
	app_led_set(m_led_blink_ch, m_led_blink_on ? APP_LED_ON : APP_LED_OFF);
}
static K_TIMER_DEFINE(m_led_blink_timer, nfc_led_blink_timer, NULL);

static void nfc_led_off(void);
static void nfc_led_reject_timeout(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	nfc_led_off();
}
static K_TIMER_DEFINE(m_led_reject_timer, nfc_led_reject_timeout, NULL);

static void nfc_led_detected(void)
{
	k_timer_stop(&m_led_reject_timer);
	k_timer_stop(&m_led_blink_timer);
	app_led_set(APP_LED_CHANNEL_R, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_Y, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_G, APP_LED_ON);
}

static void nfc_led_processing(void)
{
	k_timer_stop(&m_led_reject_timer);
	m_led_blink_on = true;
	m_led_blink_ch = APP_LED_CHANNEL_G;
	app_led_set(APP_LED_CHANNEL_R, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_Y, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_G, APP_LED_ON);
	k_timer_start(&m_led_blink_timer, K_MSEC(NFC_LED_BLINK_MS), K_MSEC(NFC_LED_BLINK_MS));
}

/* #315: the command was rejected before it ever ran (wrong secret_key /
 * vendor_token, stale or out-of-window nonce_counter, malformed frame) and no
 * reply is written back to the tag. Without this the green "servicing" blink from
 * nfc_led_processing() would simply keep running until the RF-quiet backstop —
 * visually identical to a successful command for whoever is holding the phone.
 * Same blink cadence in red (same rhythm, different colour = rejected), held for
 * NFC_LED_REJECT_MS and then cleared. */
static void nfc_led_rejected(void)
{
	m_led_blink_on = true;
	m_led_blink_ch = APP_LED_CHANNEL_R;
	app_led_set(APP_LED_CHANNEL_G, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_Y, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_R, APP_LED_ON);
	k_timer_start(&m_led_blink_timer, K_MSEC(NFC_LED_BLINK_MS), K_MSEC(NFC_LED_BLINK_MS));
	k_timer_start(&m_led_reject_timer, K_MSEC(NFC_LED_REJECT_MS), K_NO_WAIT);
}

static void nfc_led_off(void)
{
	k_timer_stop(&m_led_reject_timer);
	k_timer_stop(&m_led_blink_timer);
	app_led_set(APP_LED_CHANNEL_R, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_G, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_Y, APP_LED_OFF);
}

/* True while an NFC exchange is in progress (the keep-awake lock is held). The
 * main-loop status/heartbeat blinks defer to this so they do not fight the NFC
 * interaction LED. */
bool app_nfc_session_active(void)
{
	return atomic_get(&m_awake_held) != 0;
}

/* Take the deep-sleep lock (once) and (re)arm the inactivity window. Safe from
 * ISR context: pm_policy_state_lock_get and k_timer_start are irq-safe, and the
 * atomic_cas guards against a double get. The 0->1 edge is the start of an NFC
 * session (phone just arrived) -> light the "detected" LED. */
static void nfc_keep_awake(void)
{
	if (atomic_cas(&m_awake_held, 0, 1)) {
		pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		nfc_led_detected();
	}
	k_timer_start(&m_awake_timer, K_MSEC(NFC_AWAKE_WINDOW_MS), K_NO_WAIT);
}

/* Inactivity window elapsed with no further RF activity: drop the lock (once) so
 * the sticker can go back to deep sleep, and clear the interaction LED. */
static void nfc_awake_timeout(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	if (atomic_cas(&m_awake_held, 1, 0)) {
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		nfc_led_off();
	}
}

/* Periodic NFC check enable (toggled via `nfc autocheck`). Lets a config blob
 * be written over several `nfc write` calls without the periodic check racing
 * it and rewriting the tag to the info record mid-write. */
static bool m_periodic = true;

/* Reply staging for the mailbox session (mb_serve_locked): [chan][encrypted
 * Response], at most one 256 B frame. */
static uint8_t m_resp_buf[512];
static bool m_seen_inf; /* #247: tag holds our info record (settled resting state) */

/* #247 claim-record lifecycle, persisted in its own "clm" settings subtree (not
 * the config blob, so a factory reset that preserves identity leaves it intact —
 * only a full NVS erase re-opens provisioning):
 *   UNSET    no token provisioned yet, or clm never laid down
 *   PENDING  clm laid down on the tag, awaiting an authenticated claim confirm
 *   CONSUMED clm_ack command or any successfully-decrypted hio.stck:cmd (#360)
 *            — never rewrite it again */
enum clm_state {
	CLM_UNSET = 0,
	CLM_PENDING = 1,
	CLM_CONSUMED = 2,
};
static uint8_t m_clm_state;

/* Load handler for the "clm" settings subtree (key "clm/state"). */
static int clm_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "state", NULL)) {
		if (len != sizeof(m_clm_state)) {
			return -EINVAL;
		}
		ssize_t r = read_cb(cb_arg, &m_clm_state, sizeof(m_clm_state));
		return (r < 0) ? (int)r : 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(app_clm, "clm", NULL, clm_settings_set, NULL, NULL);

static void clm_state_save(void)
{
	int ret = settings_save_one("clm/state", &m_clm_state, sizeof(m_clm_state));
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save_one(clm/state)", ret);
	}
}

/* #340 M24: m_clm_state is also mutated by nfc_check_locked()'s poll-thread
 * arm sequence (clm_arm_commit/revert below) - guarded by m_lock across that
 * whole sequence (nfc_access_begin/end). app_nfc_clm_reset() and clm_consume()
 * are reachable directly from other threads (shell app_ats.c commands, the
 * main thread's deferred-action dispatch, app_settings_vendor_reset()) with
 * no synchronization of their own, so a reset/consume from one of those could
 * race a poll-thread commit/revert and be silently clobbered by whichever ran
 * last. Take m_lock here too - Zephyr's k_mutex is recursive for the owning
 * thread, so this is a safe no-op when already called from within the poll
 * thread's own locked sequence (clm_consume() from handle_encrypted_cmd() or
 * the delete-detected path), and correctly serializes against a genuinely
 * different thread otherwise. */
void app_nfc_clm_reset(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	m_clm_state = CLM_UNSET;
	clm_state_save();
	k_mutex_unlock(&m_lock);
}

/* Shared PENDING->CONSUMED transition (#308): two independent triggers funnel
 * through here so the latch/log/persist logic lives in one place. (#360: the
 * original third trigger - unauthenticated delete-detection in
 * nfc_check_locked, the #247 signal - was removed; it let any NFC write that
 * removed the clm record, not just the claiming phone, latch CONSUMED with no
 * secret_key involved.)
 *   1. clm_ack command (app_nfc_clm_ack) - explicit, authenticated (secret_key).
 *   2. any successfully-decrypted hio.stck:cmd (handle_encrypted_cmd) - implicit:
 *      decrypting at all already proves the caller holds secret_key, which is
 *      already the "provisioning operator" bar the rest of this file uses, so a
 *      claim window left open after a phone has already run a real command is
 *      just noise on the tag.
 * A no-op outside PENDING (already CONSUMED, or never armed). See #340 M24
 * above for why this takes m_lock. */
static void clm_consume(const char *reason)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	if (m_clm_state != CLM_PENDING) {
		k_mutex_unlock(&m_lock);
		return;
	}
	m_clm_state = CLM_CONSUMED;
	clm_state_save();
	k_mutex_unlock(&m_lock);
	LOG_INF("NFC clm record consumed (%s) (#308)", reason);
}

/* #340 M3: arming (UNSET->PENDING) is split into an in-RAM step (so
 * build_resting_ndef sees PENDING immediately) and a deferred persist that
 * only happens once nfc_check_locked has confirmed the clm-bearing resting
 * NDEF actually landed on the tag. These two helpers are that persist/undo,
 * called only on the poll cycle that just armed (just_armed == true). */
static void clm_arm_commit(void)
{
	m_clm_state = CLM_PENDING;
	clm_state_save();
}

static void clm_arm_revert(void)
{
	m_clm_state = CLM_UNSET;
}

void app_nfc_clm_ack(void)
{
	clm_consume("clm_ack command");
}

uint8_t app_nfc_clm_state_get(void)
{
	uint8_t state;

	k_mutex_lock(&m_lock, K_FOREVER);
	state = m_clm_state;
	k_mutex_unlock(&m_lock);

	return state;
}

/* #340 L1 test support: whether the "processing"/"rejected" blink timer is
 * currently armed. Same testability idiom as app_nfc_clm_state_get() above. */
bool app_nfc_led_blink_active(void)
{
	return k_timer_remaining_get(&m_led_blink_timer) != 0;
}

/* A claim token is provisioned once any byte is non-zero (all-zero = unset, the
 * same sentinel the write-once shell guard uses, #170). */
static bool claim_token_is_set(void)
{
	for (size_t i = 0; i < sizeof(g_app_config.claim_token); i++) {
		if (g_app_config.claim_token[i] != 0) {
			return true;
		}
	}
	return false;
}
static enum app_cmd_action m_cmd_action; /* deferred action from app_cmd_handle */

/* Debounce for the "unrecognized data -> restore info" path. A poll can catch
 * the tag mid-write by a foreign NFC writer (any phone app can write the EEPROM),
 * which parses as garbage; restoring the info record then would clobber what is
 * being written. So only restore info after the data stays unrecognized for
 * this many consecutive polls — a real partial write resolves within one poll. */
#define NFC_UNKNOWN_DEBOUNCE 3
static uint8_t m_unknown_count;

/* Take (and clear) the deferred action staged by the last mailbox command. The
 * mailbox session only stages it after the phone has read (or had a second to
 * read) the reply and then closes, so by the time the poll thread gets here a
 * reboot/save never cuts off an unread response. */
enum app_cmd_action app_nfc_take_cmd_action(void)
{
	enum app_cmd_action a = m_cmd_action;
	m_cmd_action = APP_CMD_ACTION_NONE;
	return a;
}

bool app_nfc_periodic_enabled(void)
{
	return m_periodic;
}

/* The ST25DV is dual-port: an I2C access concurrent with an RF transaction can
 * be NACKed (-EIO) by the arbiter — common while a phone holds its field open
 * waiting for our reply. The transfers are short-lived, so a brief retry rides
 * out the contention. Chunking a long read also means a collision only retries
 * a small block, not the whole 512 B (which would otherwise fail repeatedly
 * under continuous RF). */
#define ST25DV_I2C_RETRIES  20
#define ST25DV_I2C_RETRY_MS 2
#define ST25DV_READ_CHUNK   64

/* Bounded wait for the RF field to be off (defined further below). read_mem /
 * write_mem gate every chunk on it: if the field reappears mid-transfer they
 * pause before the next chunk and resume once it clears, so no EEPROM chunk
 * ever runs on the bus while RF is active. Returns false if the field stays on
 * past the wait, or if the status register itself is persistently unreadable
 * (fail-closed, #329) — either way the chunk loop aborts with -EBUSY and the
 * caller skips this cycle. */
static bool nfc_wait_field_off(void);

/* Chunked I2C read from the user-memory device (E0). `field_gated` selects the
 * two very different regions that live behind that device select:
 *  - true:  user EEPROM (0x0000..0x01FF). Single-port — an access concurrent with
 *           RF collides on the shared i2c1 bus and can wedge it, so every chunk
 *           waits for the RF field to be off (nfc_wait_field_off) and aborts
 *           with -EBUSY if it stays on.
 *  - false: dynamic registers / the FTM mailbox RAM (0x2000..0x2107). Dual-port
 *           by design: served to I2C while the phone holds its field, which is
 *           the whole point of the mailbox — never wait for field-off here (the
 *           old app_nfc_serve_mailbox read the mailbox through the gated path and
 *           could therefore never see a message under a held field).
 * Either way each chunk rides out arbitration NACKs with a short retry. */
static int read_chunks(uint16_t reg, void *buf, size_t len, bool field_gated)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	uint8_t *p = buf;
	size_t off = 0;
	while (off < len) {
		/* Pause before this chunk if the RF field is back; resume once it clears.
		 * Keeps every EEPROM read off the dual-port bus during RF. */
		if (field_gated && !nfc_wait_field_off()) {
			return -EBUSY;
		}

		size_t chunk = MIN(len - off, (size_t)ST25DV_READ_CHUNK);
		uint8_t reg_[2];
		sys_put_be16((uint16_t)(reg + off), reg_);

		int ret = -EIO;
		int attempt = 0;
		for (; attempt < ST25DV_I2C_RETRIES; attempt++) {
			ret = i2c_write_read(dev, ST25DV_I2C_ADDR_E0, reg_, sizeof(reg_), p + off,
					     chunk);
			if (ret == 0) {
				break;
			}
			k_msleep(ST25DV_I2C_RETRY_MS); /* let RF yield the dual port */
		}
		if (ret) {
			LOG_ERR("read_mem @0x%04x +%u: i2c -EIO after %d retries (RF contention?)",
				(unsigned)(reg + off), (unsigned)chunk, ST25DV_I2C_RETRIES);
			return ret;
		}
		NFC_DBG("rd @0x%04x +%u ok (tries=%d)", (unsigned)(reg + off), (unsigned)chunk,
			attempt + 1);
		off += chunk;
	}

	return 0;
}

/* User EEPROM read (field-gated, see read_chunks). */
static int read_mem(uint16_t reg, void *buf, size_t len)
{
	return read_chunks(reg, buf, len, true);
}

static inline uint32_t calc_prog_time_ms(uint16_t reg, size_t len)
{
	size_t off_in_page = reg & (ST25DV_INT_PAGE_BYTES - 1);
	size_t total = off_in_page + len;
	size_t pages = DIV_ROUND_UP(total, ST25DV_INT_PAGE_BYTES);
	return pages * ST25DV_TW_MS_PER_PAGE;
}

static int write_mem(uint16_t reg, const void *buf, size_t len)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	const uint8_t *p = buf;
	size_t remaining = len;

	while (remaining) {
		/* Pause before this chunk if the RF field is back; resume once it clears.
		 * Keeps every EEPROM write off the dual-port bus during RF. */
		if (!nfc_wait_field_off()) {
			return -EBUSY;
		}

		size_t within_256 =
			ST25DV_MAX_SEQ_WRITE_BYTES - (reg & (ST25DV_MAX_SEQ_WRITE_BYTES - 1));

		size_t chunk = MIN(remaining, within_256);

		if (chunk > ST25DV_MAX_SEQ_WRITE_BYTES) {
			chunk = ST25DV_MAX_SEQ_WRITE_BYTES;
		}

		uint8_t frame[2 + ST25DV_MAX_SEQ_WRITE_BYTES];
		sys_put_be16(reg, frame);
		memcpy(&frame[2], p, chunk);

		ret = -EIO;
		int attempt = 0;
		for (; attempt < ST25DV_I2C_RETRIES; attempt++) {
			ret = i2c_write(dev, frame, 2 + chunk, ST25DV_I2C_ADDR_E0);
			if (ret == 0) {
				break;
			}
			k_msleep(ST25DV_I2C_RETRY_MS); /* RF contention on the dual port */
		}
		if (ret) {
			LOG_ERR("write_mem @0x%04x +%u: i2c -EIO after %d retries", (unsigned)reg,
				(unsigned)chunk, ST25DV_I2C_RETRIES);
			return ret;
		}
		NFC_DBG("wr @0x%04x +%u ok (tries=%d)", (unsigned)reg, (unsigned)chunk,
			attempt + 1);

		uint32_t wait_ms = calc_prog_time_ms(reg, chunk);
		if (wait_ms) {
			k_msleep(wait_ms);
		}

		reg += chunk;
		p += chunk;
		remaining -= chunk;
	}

	return 0;
}

/* ST25DV register device select: dynamic registers (>=0x2000, e.g. IT_STS_Dyn
 * 0x2005, GPO_Dyn 0x2000) live on the user-memory device (E0 0x53); the static
 * system configuration area (<0x2000, e.g. GPO 0x0000) is on the system device
 * (E1 0x57). */
static inline uint8_t reg_dev_addr(uint16_t reg)
{
	return (reg >= 0x2000) ? ST25DV_I2C_ADDR_E0 : ST25DV_I2C_ADDR_E1;
}

/* Read ST25DV system/dynamic register(s). No password needed for reads. */
static int read_reg(uint16_t reg, void *buf, size_t len)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	uint8_t reg_[2];
	sys_put_be16(reg, reg_);

	int ret = -EIO;
	for (int attempt = 0; attempt < ST25DV_I2C_RETRIES; attempt++) {
		ret = i2c_write_read(dev, reg_dev_addr(reg), reg_, sizeof(reg_), buf, len);
		if (ret == 0) {
			break;
		}
		k_msleep(ST25DV_I2C_RETRY_MS); /* RF contention on the dual port */
	}
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("i2c_write_read", ret);
		return ret;
	}

	return 0;
}

/* Write an ST25DV register. Dynamic registers need no password; the static
 * system config area requires an open I2C security session (not handled here). */
static int write_reg(uint16_t reg, const void *buf, size_t len)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}
	if (len > 16) {
		return -EINVAL;
	}

	uint8_t frame[2 + 16];
	sys_put_be16(reg, frame);
	memcpy(&frame[2], buf, len);

	int ret = -EIO;
	for (int attempt = 0; attempt < ST25DV_I2C_RETRIES; attempt++) {
		ret = i2c_write(dev, frame, 2 + len, reg_dev_addr(reg));
		if (ret == 0) {
			break;
		}
		k_msleep(ST25DV_I2C_RETRY_MS); /* RF contention on the dual port */
	}
	if (ret) {
		/* Name the register + I2C device select so a NACK is identifiable on RTT
		 * (E0=0x53 data/dynamic, E1=0x57 system/password-protected). */
		LOG_ERR("i2c_write reg 0x%04x (E%c addr 0x%02x, %u B) failed after %d retries: %d",
			reg, reg_dev_addr(reg) == ST25DV_I2C_ADDR_E0 ? '0' : '1', reg_dev_addr(reg),
			(unsigned)len, ST25DV_I2C_RETRIES, ret);
		return ret;
	}

	return 0;
}

/* Open the I2C security session by presenting an 8-byte password (required to
 * write the static system config such as GPO). Frame: addr(2) + pwd(8) + 0x09
 * (present code) + pwd(8). */
static int nfc_present_password(const uint8_t pwd[8])
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	uint8_t frame[2 + 8 + 1 + 8];
	sys_put_be16(ST25DV_I2C_PWD_REG, frame);
	memcpy(&frame[2], pwd, 8);
	frame[10] = 0x09;
	memcpy(&frame[11], pwd, 8);

	int ret = -EIO;
	for (int attempt = 0; attempt < ST25DV_I2C_RETRIES; attempt++) {
		ret = i2c_write(dev, frame, sizeof(frame), ST25DV_I2C_ADDR_E1);
		if (ret == 0) {
			break;
		}
		k_msleep(ST25DV_I2C_RETRY_MS); /* RF contention / chip-busy on the dual port */
	}
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("i2c_write(password)", ret);
		return ret;
	}

	return 0;
}

/* ---- ST25DV FTM mailbox register layer (#313) ------------------------------
 * All of these run with the access lock held and the tag powered (LPD low).
 * They touch only dynamic registers / mailbox RAM (dual-port), so they are safe
 * under a held RF field and never go through nfc_wait_field_off(). */

/* Whether FTM could be authorised on this chip at boot (MB_MODE set/verified).
 * false = the mailbox command channel does not work on this unit; reported as
 * APP_DEVICE_STATUS_MAILBOX_DOWN so the production tester rejects it (#313 D7). */
static bool m_mb_available;

/* Set by nfc_wait_field_off() when it sees MB_EN appear under a held field: the
 * phone switched to the mailbox mid-wait, so the EEPROM cycle is abandoned and
 * app_nfc_poll() serves the mailbox instead. Consumed by app_nfc_poll(). */
static bool m_mb_requested;

bool app_nfc_mailbox_available(void)
{
	return m_mb_available;
}

static int mb_read_ctrl(uint8_t *ctrl)
{
	return read_reg(ST25DV_MB_CTRL_DYN, ctrl, 1);
}

/* Set or clear MB_EN and verify the read-back. Clearing also drops any message
 * flags (the chip resets HOST_PUT/RF_PUT with the mailbox). */
static int mb_set_en(bool enable)
{
	uint8_t v = enable ? ST25DV_MB_CTRL_MB_EN : 0;
	int ret = write_reg(ST25DV_MB_CTRL_DYN, &v, 1);
	if (ret) {
		return ret;
	}
	uint8_t rb = 0;
	ret = read_reg(ST25DV_MB_CTRL_DYN, &rb, 1);
	if (ret) {
		return ret;
	}
	if (!!(rb & ST25DV_MB_CTRL_MB_EN) != enable) {
		LOG_WRN("NFC mb: MB_EN=%d did not stick (MB_CTRL_Dyn=0x%02x)", enable, rb);
		return -EIO;
	}
	return 0;
}

/* Length of the message currently in the mailbox (MB_LEN_Dyn holds len - 1). */
static int mb_read_len(size_t *len)
{
	uint8_t lenm1 = 0;
	int ret = read_reg(ST25DV_MB_LEN_DYN, &lenm1, 1);
	if (ret) {
		return ret;
	}
	*len = (size_t)lenm1 + 1;
	return 0;
}

/* Configure the GPO so the pin actually pulses on RF write / field change — the
 * wake source for the event-driven NFC poll. Sets GPO_EN (master output enable) +
 * RF_WRITE + FIELD_CHANGE in BOTH the static EEPROM register (persists across
 * power-up) and the dynamic GPO_CTRL_Dyn register (takes effect immediately: the
 * dynamic GPO_EN only loads from the static one at power-up, so a freshly-written
 * static GPO_EN would otherwise not enable the GPO until the next reboot). Needs
 * the default (all-zero) I2C password for the static (EEPROM) write. Returns 0
 * once GPO_EN + events are active. Caller must hold the access lock. */
static int nfc_enable_rf_write_it(void)
{
	static const uint8_t default_pwd[8] = {0};
	int ret;

	/* Assume unavailable until MB_MODE is authorised + verified below; any early
	 * return (password / GPO failure) then correctly leaves the mailbox marked
	 * down rather than keeping a previous boot's value. */
	m_mb_available = false;

	/* 0) A mailbox left enabled by an aborted session survives an MCU reset (the
	 *    dynamic registers persist while the phone's field or VCC keeps the chip
	 *    up) and would make every EEPROM write below fail (NACK, DS §5.1.2) —
	 *    this was the June "boot mb_disable NACKs" symptom. Clear MB_EN first;
	 *    a failed read here just means the register is unknown (emulator). */
	uint8_t ctrl = 0;
	if (mb_read_ctrl(&ctrl) == 0 && (ctrl & ST25DV_MB_CTRL_MB_EN)) {
		LOG_WRN("NFC mb: left enabled at boot (MB_CTRL_Dyn=0x%02x) -> disabling", ctrl);
		(void)mb_set_en(false);
	}

	ret = nfc_present_password(default_pwd);
	if (ret) {
		return ret;
	}

	/* The chip needs a moment after a present-password write before the next
	 * I2C access is ACKed. 5 ms was too short (the next read NACKed with -EIO);
	 * a system-area probe confirmed 10 ms is enough, so allow a safe margin. */
	k_msleep(15);

	/* 1) Static (EEPROM) GPO register: persist the config so it is active from
	 *    every power-up. */
	uint8_t gpo = 0;
	ret = read_reg(ST25DV_GPO_REG, &gpo, 1);
	if (ret) {
		return ret;
	}

	if ((gpo & ST25DV_GPO_WANT) != ST25DV_GPO_WANT) {
		gpo |= ST25DV_GPO_WANT;
		ret = write_reg(ST25DV_GPO_REG, &gpo, 1);
		if (ret) {
			return ret;
		}

		/* GPO is held in EEPROM: wait the write time before reading it back,
		 * or the verify read sees the stale value. */
		k_msleep(ST25DV_TW_MS_PER_PAGE + 5);

		ret = read_reg(ST25DV_GPO_REG, &gpo, 1);
		if (ret) {
			return ret;
		}
		if ((gpo & ST25DV_GPO_WANT) != ST25DV_GPO_WANT) {
			return -EIO;
		}
	}

	/* 2) MB_MODE (static EEPROM, same password session): authorise FTM once per
	 *    device so the phone can enable the mailbox itself (MB_EN is RF-writable
	 *    only while MB_MODE=1). EEPROM write only when the bit is not set yet.
	 *    Failure is not fatal for the GPO path but marks the mailbox unavailable —
	 *    a production defect the tester catches via device_status bit 13 (D7). */
	uint8_t mode = 0;
	int mret = read_reg(ST25DV_MB_MODE_REG, &mode, 1);
	if (mret == 0 && !(mode & ST25DV_MB_MODE_EN)) {
		mode |= ST25DV_MB_MODE_EN;
		mret = write_reg(ST25DV_MB_MODE_REG, &mode, 1);
		if (mret == 0) {
			k_msleep(ST25DV_TW_MS_PER_PAGE + 5);
			mret = read_reg(ST25DV_MB_MODE_REG, &mode, 1);
			if (mret == 0 && !(mode & ST25DV_MB_MODE_EN)) {
				mret = -EIO;
			}
		}
	}
	m_mb_available = (mret == 0);
	if (!m_mb_available) {
		LOG_WRN("NFC mb: unavailable - MB_MODE cfg failed: %d (tester must reject)", mret);
	}

	/* 3) Dynamic GPO_CTRL_Dyn (volatile, E0, no password): enable GPO_EN now so
	 *    the output is live for this power cycle without waiting for a reboot. */
	uint8_t dyn = 0;
	ret = read_reg(ST25DV_GPO_CTRL_DYN_REG, &dyn, 1);
	if (ret) {
		return ret;
	}
	if (!(dyn & ST25DV_GPO_EN)) {
		dyn |= ST25DV_GPO_EN;
		ret = write_reg(ST25DV_GPO_CTRL_DYN_REG, &dyn, 1);
		if (ret) {
			return ret;
		}
	}

	LOG_INF("NFC: GPO cfg static=0x%02x dyn=0x%02x (GPO_EN=%d) MB_MODE=0x%02x mailbox=%s", gpo,
		dyn, !!(dyn & ST25DV_GPO_EN), mode, m_mb_available ? "ok" : "UNAVAILABLE");

	return 0;
}

/* Power the ST25DV up for I2C access (LPD low) and take the access lock. On
 * success the caller must pair this with nfc_access_end(). */
static int nfc_access_begin(void)
{
	int ret;

	k_mutex_lock(&m_lock, K_FOREVER);

	if (!gpio_is_ready_dt(&m_lpd)) {
		LOG_ERR("GPIO device not ready (LPD)");
		k_mutex_unlock(&m_lock);
		return -ENODEV;
	}

	ret = gpio_pin_set_dt(&m_lpd, 0);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_set_dt", ret);
		k_mutex_unlock(&m_lock);
		return ret;
	}

	k_sleep(K_MSEC(150));

	return 0;
}

/* Power the ST25DV back down (LPD high) and release the access lock. */
static void nfc_access_end(void)
{
	int ret = gpio_pin_set_dt(&m_lpd, 1);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_set_dt", ret);
	}

	k_mutex_unlock(&m_lock);
}

/* Wait (bounded) for the RF field to be absent before the caller touches the
 * user-memory EEPROM. The 512 B EEPROM reads/writes collide with a present RF
 * field on the shared i2c1 bus (arbitration NACK / bus wedge), and a wedged
 * transaction can starve the watchdog feeder in the main loop (all the sensors
 * share i2c1) -> a 10 s SoC reset with no panic dump. EH_CTRL_Dyn.FIELD_ON is a
 * dual-port dynamic register, safe to poll during RF.
 *
 * Returns true once the field is absent (safe to access). Returns false if the
 * field is still present after NFC_FIELD_OFF_WAIT_MS, or if the status read
 * itself never succeeds (caller should skip this cycle either way).
 *
 * FAIL-CLOSED (#329/#330): read_reg() already retries the transfer
 * ST25DV_I2C_RETRIES times internally before giving up, so a `ret != 0` here
 * is not routine dual-port RF contention (that would have been absorbed by
 * those retries) — it is a persistently unreadable register, the same
 * symptom a wedged i2c1 produces (Stop2 wiping TIMINGR with no resume edge
 * to reapply it, #329). Proceeding into the EEPROM chunk access on that
 * evidence used to be fail-open (assume field-off, go ahead); that just
 * traded one silent failure for another doomed transfer a few chunks later.
 * Failing closed costs nothing a genuinely-live bus would have needed anyway
 * (the caller's -EBUSY path already exists for "field still on" and simply
 * retries next poll cycle), and it stops masking a real bus fault as if it
 * were expected RF contention. The caller must already hold the access lock
 * (tag powered via nfc_access_begin). */
static bool nfc_wait_field_off(void)
{
	bool waited_for_field = false;
	for (int waited = 0; waited <= NFC_FIELD_OFF_WAIT_MS; waited += NFC_FIELD_POLL_MS) {
		uint8_t eh;
		int ret = read_reg(ST25DV_EH_CTRL_DYN, &eh, 1);
		if (ret != 0) {
			NFC_DBG("field: EH_CTRL_Dyn read=%d after retries -> abort (fail-closed)",
				ret);
			return false; /* persistently unreadable -> don't touch a possibly-wedged
					 bus */
		}
		if (!(eh & ST25DV_FIELD_ON)) {
			if (waited_for_field) {
				NFC_DBG("field: cleared after %d ms (EH=0x%02x)", waited, eh);
			}
			return true; /* field absent -> safe to access the EEPROM */
		}
		if (!waited_for_field) {
			NFC_DBG("field: FIELD_ON set (EH=0x%02x) -> waiting for RF off", eh);
			waited_for_field = true;
		}
		/* #313: a phone that keeps its field on and enables the mailbox wants
		 * the FTM channel, not an EEPROM exchange — stop waiting for a field-off
		 * window that will never come and let app_nfc_poll() serve the mailbox. */
		uint8_t ctrl = 0;
		if (m_mb_available && mb_read_ctrl(&ctrl) == 0 && (ctrl & ST25DV_MB_CTRL_MB_EN)) {
			NFC_DBG("field: MB_EN set by RF after %d ms -> mailbox session", waited);
			m_mb_requested = true;
			return false;
		}
		k_msleep(NFC_FIELD_POLL_MS);
	}
	NFC_DBG("field: still on after %d ms -> abort EEPROM chunk", NFC_FIELD_OFF_WAIT_MS);
	return false;
}

/* One NFC Forum external-type record in an NDEF message. */
struct ndef_rec {
	uint8_t tnf;      /* NDEF_TNF_EXT (external type); MIME no longer used (#313 D6) */
	const char *type; /* external type name, or media-type string when tnf == MIME */
	const uint8_t *payload;
	size_t payload_len;
};

/* Frame `n_recs` records into a single NDEF message (CC + Message TLV + records +
 * Terminator TLV) at `out`. Message-Begin is set on the first record, Message-End
 * on the last, so a single record (n_recs==1) is byte-identical to the previous
 * single-record framer (0xD4 short / 0xC4 normal). Returns bytes written, 0 on
 * overflow / empty. */
static size_t build_ndef_message(uint8_t *out, size_t out_size, const struct ndef_rec *recs,
				 size_t n_recs)
{
	if (n_recs == 0) {
		return 0;
	}

	/* Sum every record's encoded length to size the NDEF Message TLV. Each
	 * record: flags + type_len + payload_len_field (1 short / 4 normal) + type +
	 * payload. */
	size_t msg_len = 0;
	for (size_t r = 0; r < n_recs; r++) {
		size_t type_len = strlen(recs[r].type);
		size_t len_field = (recs[r].payload_len <= 0xFF) ? 1 : 4;
		msg_len += 1 + 1 + len_field + type_len + recs[r].payload_len;
	}

	/* NDEF Message TLV length: single byte below 0xFF, else the 3-byte form
	 * (0xFF + 2-byte big-endian length, max 0xFFFE). */
	bool tlv_long = msg_len >= 0xFF;
	size_t tlv_len_field = tlv_long ? 3 : 1;

	/* Tag content: CC (4) + TLV type (0x03) + TLV length + message + Terminator. */
	size_t total = 4 + 1 + tlv_len_field + msg_len + 1;
	if (msg_len > 0xFFFE || total > out_size) {
		return 0;
	}

	size_t i = 0;
	out[i++] = ST25DV_CC0; /* Type 5 Capability Container */
	out[i++] = ST25DV_CC1;
	out[i++] = ST25DV_CC2;
	out[i++] = ST25DV_CC3;
	out[i++] = 0x03; /* NDEF Message TLV type */
	if (tlv_long) {
		out[i++] = 0xFF;
		out[i++] = (uint8_t)(msg_len >> 8);
		out[i++] = (uint8_t)(msg_len & 0xFF);
	} else {
		out[i++] = (uint8_t)msg_len;
	}

	for (size_t r = 0; r < n_recs; r++) {
		size_t type_len = strlen(recs[r].type);
		bool short_record = recs[r].payload_len <= 0xFF;
		/* TNF per record (external 0x04 or MIME 0x02); MB on the first record, ME on
		 * the last, SR when the payload length fits one byte. */
		uint8_t flags = recs[r].tnf;
		if (r == 0) {
			flags |= 0x80; /* Message Begin */
		}
		if (r == n_recs - 1) {
			flags |= 0x40; /* Message End */
		}
		if (short_record) {
			flags |= 0x10; /* Short Record */
		}
		out[i++] = flags;
		out[i++] = (uint8_t)type_len;
		if (short_record) {
			out[i++] = (uint8_t)recs[r].payload_len;
		} else {
			out[i++] = (uint8_t)(recs[r].payload_len >> 24);
			out[i++] = (uint8_t)(recs[r].payload_len >> 16);
			out[i++] = (uint8_t)(recs[r].payload_len >> 8);
			out[i++] = (uint8_t)(recs[r].payload_len & 0xFF);
		}
		memcpy(&out[i], recs[r].type, type_len);
		i += type_len;
		memcpy(&out[i], recs[r].payload, recs[r].payload_len);
		i += recs[r].payload_len;
	}
	out[i++] = 0xFE; /* Terminator TLV */

	return i;
}

/* Build the info payload (see NDEF_INFO_TYPE comment) into `out`. Stable between
 * accepted NFC commands (no uptime/clock; the nonce counter advances only on an
 * accepted command), so app_nfc_check() can compare it to the tag content and skip
 * rewriting when already present, and rewrite it when the counter has moved.
 * Buffer for "<10 serial>:<hex config_ver>:<8 hex nonce>" + NUL (config_ver is a
 * uint32 so allow its full 8 hex digits, though it is normally 1–2). */
#define NDEF_INFO_PAYLOAD_MAX 32

/* Format the ASCII info payload; returns its length (excluding the NUL), 0 on
 * overflow. */
static size_t build_info_payload(char *payload, size_t size)
{
	struct app_cmd_info info;
	app_cmd_get_info(&info);

	/* Anti-replay counter high-water read live (app_config(), == what decrypt()
	 * checks against) so the phone can resync after a reboot/cache-miss. */
	int n = snprintf(payload, size, "%010u:%02X:%08X", (unsigned int)info.serial_number,
			 (unsigned int)g_app_config.config_version,
			 (unsigned int)app_config()->nonce_counter);
	if (n <= 0 || (size_t)n >= size) {
		return 0;
	}

	return (size_t)n;
}

/* #247: encode the ClaimInfo protobuf {serial_number, claim_token} into `out`.
 * Plaintext in every build (like the info record) — the claim window relies on
 * physical proximity + the backend first-claim check, not on the NFC key. */
static size_t build_claim_payload(uint8_t *out, size_t out_size)
{
	ClaimInfo msg = ClaimInfo_init_zero;
	msg.serial_number = g_app_config.serial_number;
	BUILD_ASSERT(sizeof(msg.claim_token) == sizeof(g_app_config.claim_token),
		     "ClaimInfo.claim_token size mismatch");
	memcpy(msg.claim_token, g_app_config.claim_token, sizeof(g_app_config.claim_token));

	pb_ostream_t stream = pb_ostream_from_buffer(out, out_size);
	if (!pb_encode(&stream, ClaimInfo_fields, &msg)) {
		LOG_ERR("pb_encode(ClaimInfo) failed: %s", PB_GET_ERROR(&stream));
		return 0;
	}
	return stream.bytes_written;
}

/* Build the "resting" NDEF the tag holds between phone exchanges: the info
 * record, plus the clm provisioning record while m_clm_state == CLM_PENDING
 * (#247). Stable input (advances only with the nonce counter / claim state) so
 * nfc_check_locked can compare it to the tag and skip rewriting when present. */
static size_t build_resting_ndef(uint8_t *out, size_t out_size)
{
	char inf[NDEF_INFO_PAYLOAD_MAX];
	size_t inf_len = build_info_payload(inf, sizeof(inf));

	/* inf first (Message Begin) so Android tap-to-launch keys off it. */
	struct ndef_rec recs[2];
	size_t n = 0;
	recs[n++] = (struct ndef_rec){.tnf = NDEF_TNF_EXT,
				      .type = NDEF_INFO_TYPE,
				      .payload = (const uint8_t *)inf,
				      .payload_len = inf_len};

	uint8_t clm[ClaimInfo_size];
	if (m_clm_state == CLM_PENDING) {
		size_t clm_len = build_claim_payload(clm, sizeof(clm));
		if (clm_len) {
			recs[n++] = (struct ndef_rec){.tnf = NDEF_TNF_EXT,
						      .type = NDEF_CLAIM_TYPE,
						      .payload = clm,
						      .payload_len = clm_len};
		}
	}

	return build_ndef_message(out, out_size, recs, n);
}

#ifdef CONFIG_APP_NFC_ENCRYPTION

/* AES-CCM nonce layout: serial(4) || nonce_counter(4) || direction(1). The
 * direction byte separates the request keystream from the response keystream so
 * a request and its reply can never share a (key, nonce) pair (which would leak
 * both plaintexts via keystream XOR). It is implicit — NOT transmitted — each
 * side fills it in from its own role. The 8-byte wire header is additionally
 * fed as AAD so serial/counter are authenticated by the tag. */
#define NFC_NONCE_LEN          9
#define NFC_NONCE_DIR_REQUEST  0x00
#define NFC_NONCE_DIR_RESPONSE 0x01

/* Largest forward jump accepted for the anti-replay nonce counter (#266, N-2).
 * decrypt() accepts a received counter only in (current, current + this]. Without
 * an upper bound a single accepted command carrying a counter near UINT32_MAX
 * would store that high-water and make every future (necessarily larger) counter
 * impossible — permanently bricking the encrypted NFC channel, recoverable only
 * by a debug `settings erase` or JTAG mass-erase (the counter is
 * preserve_on_reset). The Manager-App reads the current high-water from the
 * plaintext `inf` record before every command and always sends current + 1, so
 * legitimate jumps are 1; this window is generous headroom yet negligible vs.
 * UINT32_MAX, so it eliminates the brick without constraining real use. The
 * nfc_crypto ztest mirrors this constant — keep them in lockstep. */
#define NFC_NONCE_MAX_SKIP 1024

#define NFC_CCM_TAG_LEN 16

static bool is_buffer_zero(const void *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (((const uint8_t *)buf)[i] != 0) {
			return false;
		}
	}

	return true;
}

/* An all-zero key means it has not been provisioned yet. Mirror the
 * bootloader's auth.c (all_zero() -> unkeyed): refuse to take part in the
 * encrypted channel at all while unkeyed. Otherwise, with
 * CONFIG_APP_NFC_ENCRYPTION=y (default), the device would AES-CCM-decrypt and
 * execute any command forged under the public all-zero key (serial is public,
 * nonce_counter starts at 0) — an attacker in NFC range could rewrite LoRaWAN
 * keys, device-reset or wedge the device with valid CCM tags. In the unkeyed
 * state only the plaintext info record is served. Same guard applies to
 * vendor_token (#299, #316): an unprovisioned device also refuses hio.stck:vnd. */
static bool key_is_provisioned(const uint8_t *key, size_t key_len)
{
	return !is_buffer_zero(key, key_len);
}

/* CCM is provided by app_ccm (RFC 3610 over the STM32WL HW AES or the soft-SE AES,
 * #261), replacing mbedTLS. The wire bytes are byte-identical to the previous
 * mbedtls_ccm, so the phone-side contract and the nfc_crypto golden vectors are
 * unchanged. `key` is g_app_config.secret_key for the hio.stck:cmd channel or
 * g_app_config.vendor_token for the hio.stck:vnd channel (#299, #316) — both are
 * 128-bit, and this function is otherwise identical either way. */
static int decrypt(const uint8_t *key, const uint8_t *in, size_t in_len, uint8_t *out,
		   size_t out_size, size_t *out_len)
{
	int res = 0;

	if (!key_is_provisioned(key, 16)) {
		LOG_ERR("Key not provisioned; rejecting encrypted request");
		return -EACCES;
	}

	if (in_len < 8 + NFC_CCM_TAG_LEN) {
		LOG_ERR("Buffer too short for decryption: %zu byte(s)", in_len);
		return -EINVAL;
	}

	/* Verify serial number (part of nonce) */
	uint32_t serial_number = sys_get_be32(&in[0]);
	LOG_INF("Serial number: %u", serial_number);
	NFC_REPORT("  serial: %u (expected %u)", serial_number, g_app_config.serial_number);

	if (g_app_config.serial_number != serial_number) {
		LOG_ERR("Serial number does not match: %u != %u", serial_number,
			g_app_config.serial_number);
		return -EACCES;
	}

	/* Verify nonce counter (part of nonce) */
	uint32_t nonce_counter = sys_get_be32(&in[4]);
	LOG_INF("Nonce counter: %u", nonce_counter);
	NFC_REPORT("  nonce: %u (last used %u)", nonce_counter, app_config()->nonce_counter);

	/* Compare against the live high-water mark (app_config()/m_app_config) — the
	 * same struct decrypt() advances below. Using g_app_config here would read a
	 * stale boot-time value (g is only synced from m at commit/reset), which would
	 * let any already-used counter be replayed within a session. */
	if (app_config()->nonce_counter >= nonce_counter) {
		LOG_ERR("Nonce counter is not greater than the last used nonce: %u >= %u",
			app_config()->nonce_counter, nonce_counter);
		return -EACCES;
	}

	/* Bound the forward jump (#266, N-2). Reject a counter implausibly far ahead
	 * of the high-water so a buggy/malicious provisioning tool cannot store a
	 * near-UINT32_MAX value and permanently brick the channel. The subtraction is
	 * overflow-safe: the check above guarantees nonce_counter > current, so the
	 * unsigned difference never wraps. */
	if (nonce_counter - app_config()->nonce_counter > NFC_NONCE_MAX_SKIP) {
		LOG_ERR("Nonce counter jumps too far ahead: %u > %u + %u", nonce_counter,
			app_config()->nonce_counter, NFC_NONCE_MAX_SKIP);
		return -EACCES;
	}

	/* Wire after the 8 B header is [ciphertext || 16 B tag]. */
	size_t pt_len = in_len - 8 - NFC_CCM_TAG_LEN;
	if (out_size < pt_len) {
		LOG_ERR("Output buffer too short: need %zu, have %zu", pt_len, out_size);
		return -ENOMEM;
	}

	uint8_t nonce[NFC_NONCE_LEN];
	memcpy(nonce, in, 8);
	nonce[8] = NFC_NONCE_DIR_REQUEST;

	int ret = app_ccm_auth_decrypt(key, nonce, sizeof(nonce), /* AAD */ in, 8, &in[8], pt_len,
				       &in[8 + pt_len], NFC_CCM_TAG_LEN, out);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_ccm_auth_decrypt", ret);
		res = -EIO;
	} else {
		*out_len = pt_len;
	}

	if (!res) {
		/* Advance and persist the anti-replay counter NOW, before the caller runs
		 * the command — otherwise a command that reboots (e.g. lrw_reset) would
		 * lose the bump and stay replayable. Persist a single NVS key, not the
		 * whole settings blob. If the persist fails, roll the counter back and
		 * reject so we never accept a counter we couldn't make durable. */
		uint32_t prev = app_config()->nonce_counter;
		app_config()->nonce_counter = nonce_counter;
		int sret = app_settings_save_nonce_counter();
		if (sret) {
			LOG_ERR_CALL_FAILED_INT("app_settings_save_nonce_counter", sret);
			app_config()->nonce_counter = prev;
			res = sret;
		}
	}

	return res;
}

/* Encrypt `in_len` plaintext bytes into the response wire format:
 * [serial(4)][nonce_counter(4)][AES-CCM ciphertext + 16 B tag]. The CCM nonce is
 * serial||nonce_counter||RESPONSE — same counter as the request but a distinct
 * direction byte, so the reply never shares a (key, nonce) pair with the request.
 * The phone knows the counter (echoed in the header) and the implicit RESPONSE
 * direction, so it can decrypt. Returns the wire length in *out_len, or errno. */
static int encrypt(const uint8_t *key, const uint8_t *in, size_t in_len, uint32_t nonce_counter,
		   uint8_t *out, size_t out_size, size_t *out_len)
{
	int res = 0;

	if (!key_is_provisioned(key, 16)) {
		LOG_ERR("Key not provisioned; refusing to emit encrypted response");
		return -EACCES;
	}

	/* 8 B header + ciphertext (== plaintext) + 16 B CCM tag. */
	if (out_size < 8 + in_len + 16) {
		LOG_ERR("Buffer too short for encryption: need %zu, have %zu", 8 + in_len + 16,
			out_size);
		return -ENOMEM;
	}

	sys_put_be32(g_app_config.serial_number, &out[0]);
	sys_put_be32(nonce_counter, &out[4]);

	uint8_t nonce[NFC_NONCE_LEN];
	memcpy(nonce, out, 8);
	nonce[8] = NFC_NONCE_DIR_RESPONSE;

	/* Ciphertext (== plaintext length) into &out[8], the 16 B tag right after it. */
	int ret = app_ccm_encrypt_and_tag(key, nonce, sizeof(nonce), /* AAD */ out, 8, in, in_len,
					  &out[8], &out[8 + in_len], NFC_CCM_TAG_LEN);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_ccm_encrypt_and_tag", ret);
		res = -EIO;
	}

	if (!res) {
		*out_len = 8 + in_len + NFC_CCM_TAG_LEN;
	}

	return res;
}

/* Last encrypted response, kept so a retransmission (same nonce_counter, e.g. the
 * phone never read the reply over a lossy RF link) replays the cached reply instead
 * of re-running the command. RAM-only and serialised by the tag lock (m_lock); on a
 * reboot it is empty, so a post-reboot retransmission falls through to -EACCES and
 * the phone resyncs from the info-record counter.
 *
 * #340 M1: `m_resp_cache_req`/`m_resp_cache_req_len` hold the exact request frame
 * (ciphertext + CCM tag) that produced the cached reply. A cache hit requires a
 * byte-exact match against it, not just the plaintext (serial, counter) header —
 * both of those are public (readable from the resting hio.stck:inf record), so
 * matching on them alone would let anyone forge an 8-byte frame and get the cached
 * reply served without ever proving key possession. A genuine retransmission from
 * the phone resends the identical frame, so this doesn't affect the legitimate
 * case. */
static uint32_t m_resp_cache_counter;
static uint8_t m_resp_cache_buf[512];
static size_t m_resp_cache_len;
static uint8_t m_resp_cache_req[512];
static size_t m_resp_cache_req_len;

/* Process one encrypted command frame for a keyed NDEF channel: secret_key over
 * hio.stck:cmd, or vendor_token over hio.stck:vnd (#316). `key` selects the
 * AES-CCM key; `transport` is passed to app_cmd_handle() (NFC vs VENDOR command/
 * writable gating); `cache` enables the same-counter response cache — hio.stck:cmd
 * passes true, the vendor channel passes false so it neither serves from nor
 * writes the (single, shared) cache slot, avoiding a cross-key mix-up.
 * Three-way on the nonce counter vs the stored high-water:
 *   counter == cached AND frame byte-identical to the cached request
 *                       -> retransmission: replay the cached response, do NOT re-run
 *   counter  > stored  -> new: decrypt (advances+persists the counter), run, cache
 *   counter <= stored, or counter == cached with a mismatched frame
 *                       -> stale/forged: falls through to decrypt(), which rejects
 *                          with -EACCES (the monotonic counter check never re-admits
 *                          a counter <= stored)
 * Writes the encrypted reply into out_buf/out_len and the deferred action into
 * *action (NONE on a replay — the original already ran). Sets *replayed=true on a
 * same-counter retransmission so the caller keeps a deferred action still waiting
 * for the phone's ack instead of clearing it. Returns 0 or -errno. */
static int handle_encrypted_cmd(const uint8_t *key, enum app_cmd_transport transport, bool cache,
				const uint8_t *in, size_t in_len, uint8_t *out_buf, size_t out_cap,
				size_t *out_len, enum app_cmd_action *action, bool *replayed)
{
	*out_len = 0;
	*action = APP_CMD_ACTION_NONE;
	*replayed = false;

	if (in_len < 8) {
		return -EINVAL;
	}

	uint32_t serial = sys_get_be32(&in[0]);
	uint32_t counter = sys_get_be32(&in[4]);
	NFC_DBG("cmd: in_len=%zu serial=%u counter=%u (cache=%u stored=%u)", in_len, serial,
		counter, m_resp_cache_counter, app_config()->nonce_counter);

	/* #340 M1: serial+counter alone are not proof of key possession (both are
	 * public, readable off the resting hio.stck:inf record) - require the whole
	 * incoming frame to match the one that produced the cached reply. A genuine
	 * retransmission resends the identical ciphertext+tag; a forged header-only
	 * frame does not and falls through to decrypt(), which rejects it below. */
	if (cache && serial == g_app_config.serial_number && m_resp_cache_len > 0 &&
	    counter == m_resp_cache_counter && in_len == m_resp_cache_req_len &&
	    memcmp(in, m_resp_cache_req, in_len) == 0) {
		if (m_resp_cache_len > out_cap) {
			return -ENOMEM;
		}
		memcpy(out_buf, m_resp_cache_buf, m_resp_cache_len);
		*out_len = m_resp_cache_len;
		*replayed = true;
		NFC_REPORT("  -> retransmission (counter %u): replaying cached response", counter);
		return 0;
	}

	static uint8_t cmd_plain[512];
	static uint8_t resp_plain[512];
	size_t cmd_len = 0;
	int ret = decrypt(key, in, in_len, cmd_plain, sizeof(cmd_plain), &cmd_len);
	if (ret) {
		NFC_DBG("cmd: decrypt failed=%d", ret);
		return ret;
	}
	uint32_t req_nonce = app_config()->nonce_counter;
	NFC_DBG("cmd: decrypt ok, cmd_len=%zu", cmd_len);

	/* #308: decrypting at all already proves the caller holds secret_key,
	 * regardless of which command it turns out to be or whether it succeeds -
	 * that is already the bar the rest of the claim window relies on. Excludes
	 * the vendor_token channel (#316): a vendor_token holder is a narrower,
	 * separate principal (HARDWARIO recovery), not proof of secret_key
	 * possession, so a vendor touch must not silently close a claim window
	 * meant for the actual device owner. */
	if (transport != APP_CMD_TRANSPORT_VENDOR) {
		clm_consume("valid hio.stck:cmd received");
	}

	/* The encrypted reply must fit the caller's buffer: 8 B header + 16 B tag of
	 * overhead. NDEF callers pass 512 (no reply gets near it); the 256 B mailbox
	 * frame leaves 231 B of plaintext, and app_cmd_handle() already degrades an
	 * oversize reply (drops Info alarms, then a compact Error) instead of failing. */
	size_t plain_cap = sizeof(resp_plain);
	if (out_cap > 8 + NFC_CCM_TAG_LEN && out_cap - 8 - NFC_CCM_TAG_LEN < plain_cap) {
		plain_cap = out_cap - 8 - NFC_CCM_TAG_LEN;
	}

	size_t resp_len = 0;
	ret = app_cmd_handle(transport, cmd_plain, cmd_len, resp_plain, plain_cap, &resp_len,
			     action);
	if (ret) {
		NFC_DBG("cmd: app_cmd_handle failed=%d", ret);
		return ret;
	}
	NFC_DBG("cmd: handled, resp_len=%zu", resp_len);
	if (resp_len == 0) {
		return 0;
	}

	ret = encrypt(key, resp_plain, resp_len, req_nonce, out_buf, out_cap, out_len);
	if (ret) {
		return ret;
	}

	/* Cache for an idempotent retransmission of this counter (hio.stck:cmd only;
	 * the vendor channel passes cache=false — see the header). Store the request
	 * frame alongside the reply (#340 M1) so a later retransmission can be
	 * verified byte-exact instead of trusting the plaintext header alone. */
	if (cache) {
		if (*out_len <= sizeof(m_resp_cache_buf) && in_len <= sizeof(m_resp_cache_req)) {
			memcpy(m_resp_cache_buf, out_buf, *out_len);
			m_resp_cache_len = *out_len;
			memcpy(m_resp_cache_req, in, in_len);
			m_resp_cache_req_len = in_len;
			m_resp_cache_counter = req_nonce;
		} else {
			m_resp_cache_len = 0;
			m_resp_cache_req_len = 0;
		}
	}
	return 0;
}

#endif /* CONFIG_APP_NFC_ENCRYPTION */

static int parser_callback(const struct app_nfc_parser_record_info *record_info, void *user_data)
{
	ARG_UNUSED(user_data);

	/* Only our own resting records are meaningful on the tag (#313: commands no
	 * longer travel over NDEF — they use the FTM mailbox). Everything is an NFC
	 * Forum external-type record (TNF 0x04); anything else is foreign data. */
	if (record_info->tnf != NDEF_TNF_EXT) {
		return 0;
	}

	/* Our info record: marks a settled resting tag (vs a foreign write in
	 * progress). nfc_check_locked uses this to refresh a stale info record
	 * (nonce high-water advanced by a mailbox session) and to reconcile the clm
	 * lifecycle (#247). */
	size_t inf_type_len = strlen(NDEF_INFO_TYPE);
	if (record_info->type_len == inf_type_len &&
	    strncmp((const char *)record_info->type, NDEF_INFO_TYPE, inf_type_len) == 0) {
		m_seen_inf = true;
		return 0;
	}

	/* #247/#360: our provisioning claim record. Presence detection only — the
	 * firmware never parses it back and no longer reacts to its absence (that
	 * unauthenticated consume trigger was removed by #360). Recognized here purely
	 * so it doesn't fall through to the "unrecognized record" log line below. */
	size_t clm_type_len = strlen(NDEF_CLAIM_TYPE);
	if (record_info->type_len == clm_type_len &&
	    strncmp((const char *)record_info->type, NDEF_CLAIM_TYPE, clm_type_len) == 0) {
		NFC_REPORT("NFC read: our clm provisioning record (%u B)",
			   record_info->payload_len);
		return 0;
	}

	/* A record we do not lay down ourselves: an old app's hio.stck:cmd (never
	 * executed — the NDEF command channel is gone, #313 D2), a retired
	 * hio.stck:cfg (#250), or a foreign writer. Debounced restore below. */
	NFC_REPORT("NFC read: unrecognized record type (%u B) -> ignored",
		   record_info->payload_len);
	return 0;
}

/* ST25DV GPO interrupt handler: wake the NFC poll thread to service the tag and
 * hold the CPU out of Stop2 for the RF session (nfc_keep_awake) so the tag I/O
 * of the command isn't torn by a mid-operation deep-sleep. The GPO fires on RF
 * field-on/off and RF writes (configured in nfc_enable_rf_write_it). The EXTI on
 * PB12 wakes the CPU from Stop2, so this ISR is what turns a phone tap into a
 * serviced command with no periodic polling. */
static void gpo_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/* The GPO EXTI (PB12) fired on an RF event — wake the poll thread and keep the
	 * CPU out of Stop2 for the session. nfc_keep_awake lights the "phone detected"
	 * LED on the session's first event. */
	nfc_keep_awake();
	k_sem_give(&m_gpo_sem);
}

/* Block the NFC poll thread until the GPO line signals RF activity. A negative
 * `fallback_ms` waits forever (event-driven: the thread only wakes on the GPO
 * EXTI, so an idle sticker never polls the tag and stays in Stop2); a
 * non-negative value is a bounded safety net (used while a response/info-restore
 * is pending). Returns 0 if woken by GPO, -EAGAIN on the fallback timeout. */
int app_nfc_wait_event(int fallback_ms)
{
	k_timeout_t timeout = (fallback_ms < 0) ? K_FOREVER : K_MSEC(fallback_ms);

	return k_sem_take(&m_gpo_sem, timeout);
}

/* Configure the GPO line (PB12) as an input with an edge interrupt. */
static int nfc_gpo_irq_setup(void)
{
	if (!gpio_is_ready_dt(&m_gpo)) {
		LOG_ERR("GPO gpio not ready");
		return -ENODEV;
	}
	int ret = gpio_pin_configure_dt(&m_gpo, GPIO_INPUT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt(gpo)", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&m_gpo, GPIO_INT_EDGE_BOTH);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_interrupt_configure_dt(gpo)", ret);
		return ret;
	}
	gpio_init_callback(&m_gpo_cb, gpo_isr, BIT(m_gpo.pin));
	ret = gpio_add_callback(m_gpo.port, &m_gpo_cb);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_add_callback(gpo)", ret);
		return ret;
	}
	return 0;
}

/* Set once app_nfc_init() succeeds. The ST25DV tag is non-essential (#88): on
 * init failure we degrade instead of die()-ing. Single source of truth for the
 * NFC-ready state; main.c and the device_status bit read it via app_nfc_ready(). */
static bool m_ready;

bool app_nfc_ready(void)
{
	return m_ready;
}

int app_nfc_init(void)
{
	int ret;

#ifndef CONFIG_APP_NFC_ENCRYPTION
	LOG_WRN("============================================================");
	LOG_WRN("== NFC ENCRYPTION DISABLED - VALIDATION BUILD ONLY        ==");
	LOG_WRN("== Command & config records are accepted in PLAINTEXT.    ==");
	LOG_WRN("== Do NOT ship this build.                                ==");
	LOG_WRN("============================================================");
#endif

	/* #247: restore the claim-record lifecycle latch from its own settings subtree
	 * (settings subsystem already brought up by app_config_init; idempotent here). */
	(void)settings_subsys_init();
	ret = settings_load_subtree("clm");
	if (ret) {
		LOG_WRN("NFC: clm state load failed: %d (defaulting UNSET)", ret);
	}
	LOG_INF("NFC: clm state = %u", m_clm_state);

	if (!gpio_is_ready_dt(&m_lpd)) {
		LOG_ERR("GPIO device not ready (LPD)");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&m_lpd, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	/* Configure GPO RF_WRITE_EN so RF EEPROM writes are reported (sets the GPO
	 * pin / IT_STS_Dyn). The poll no longer gates on it (it reads 0x00 here, see
	 * app_nfc_poll), but keeping it set readies the GPO pin for a future
	 * hardware-interrupt-driven, low-power command pickup. Not fatal. */
	if (nfc_access_begin() == 0) {
		if (nfc_enable_rf_write_it() == 0) {
			LOG_INF("NFC: RF_WRITE_EN configured");
		} else {
			LOG_WRN("NFC: RF_WRITE_EN config failed (not used by the poll)");
		}
		nfc_access_end();
	}

	ret = nfc_gpo_irq_setup();
	if (ret) {
		LOG_WRN("NFC: GPO IRQ setup failed: %d", ret);
	} else {
		LOG_INF("NFC: GPO IRQ on PB12 ready");
	}

#ifdef CONFIG_PM_DEVICE
	/* Keep GPIOB (the PB12 GPO port) out of the Stop2 device-suspend sweep so its
	 * EXTI can wake the MCU from deep sleep on an RF event — the basis for the
	 * event-driven, low-power NFC pickup. Needs `wakeup-source` on &gpiob (app.overlay)
	 * to be WS-capable; the return value reports whether the flag actually took. */
	const struct device *gpiob = DEVICE_DT_GET(DT_NODELABEL(gpiob));
	if (device_is_ready(gpiob)) {
		bool ok = pm_device_wakeup_enable(gpiob, true);

		LOG_INF("NFC: gpiob wake-source enable %s", ok ? "ok" : "FAILED (not WS-capable)");
	}
#endif /* CONFIG_PM_DEVICE */

	/* NOTE: do NOT write the info record here. The boot-time app_nfc_check() (and
	 * the poll thread) already lay it down on a blank tag and restore it after a
	 * consumed config/command — and crucially they run AFTER reading the tag, so a
	 * config/command written over NFC while the device was powered off is ingested
	 * first. Writing the info record at init would overwrite that pending record
	 * before it is read, breaking power-off provisioning (SetParam-applied-at-boot,
	 * #147). */
	m_ready = true;
	return 0;
}

/* Core NFC reconciliation: read the tag and make sure it holds our resting NDEF
 * (info record, plus clm while PENDING). Caller must hold the access lock and
 * the RF field must be off (EEPROM). */
static int nfc_check_locked(void)
{
	int ret;
	int res = 0;

	m_seen_inf = false;

	/* #247: once a claim token is provisioned, start exposing the clm record
	 * (UNSET -> PENDING). Driven purely by the token being set (not tag content),
	 * so it fires on the first check after commissioning; build_resting_ndef then
	 * includes clm. CONSUMED (via clm_ack or a decrypted command, #360) is
	 * terminal. `just_armed` guards the deferred-persist step below (#340 M3):
	 * the PENDING state is set in RAM only here (build_resting_ndef right below
	 * needs it immediately to include the clm record) - it is NOT persisted yet.
	 * If the tag write that lays down that clm-bearing record never lands (RF
	 * field up -> -EBUSY, I2C error, ...), persisting PENDING now would leave
	 * flash out of sync with what's actually on the tag. So every path below
	 * reachable while just_armed is true must either confirm the write succeeded
	 * and call clm_arm_commit() (persist PENDING), or call clm_arm_revert() (undo
	 * back to UNSET in RAM so the next poll retries arming from scratch) - see
	 * #351/#357 clm_rearm, which made this arming sequence run on every
	 * re-provisioning, not just once at factory commissioning. */
	bool just_armed = false;
	if (m_clm_state == CLM_UNSET && claim_token_is_set()) {
		m_clm_state = CLM_PENDING;
		just_armed = true;
		LOG_INF("NFC clm record armed (claim token provisioned) (#247)");
	}

	/* read_mem / write_mem below gate every EEPROM chunk on the RF field being
	 * off (see nfc_wait_field_off): a 512 B access during RF collides with the
	 * phone on the shared i2c1 bus and can wedge it, starving the watchdog feeder
	 * in the main loop (all sensors share i2c1) -> a 10 s SoC reset with no panic
	 * dump. The sensors keep using i2c1 unaffected. */

	/* Build the expected resting NDEF up front (no I2C): info record, plus the
	 * clm record while PENDING (#247). Used both to detect "tag already holds our
	 * resting content" and to (re)write it. */
	uint8_t info[128];
	size_t info_len = build_resting_ndef(info, sizeof(info));

	ret = read_mem(0, m_buf, ST25DV_USER_MEM_SIZE);
	if (ret == -EBUSY) {
		/* RF field stayed on through the read -> skip this cycle (benign); the
		 * GPO event / fallback re-polls once the field is quiet again. No write
		 * happened, so an arm this cycle (#340 M3) is not yet confirmed - retry. */
		if (just_armed) {
			clm_arm_revert();
		}
		return 0;
	}
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("read_mem", ret);
		if (just_armed) {
			clm_arm_revert();
		}
		return ret;
	}
	NFC_DBG("poll: tag read ok, CC=%02x TLV=%02x len=%02x rec=%02x", m_buf[0], m_buf[4],
		m_buf[5], m_buf[6]);

	/* Empty tag: lay down the info record so a phone always finds metadata. */
	if (is_buffer_zero(m_buf, ST25DV_USER_MEM_SIZE)) {
		m_unknown_count = 0;
		NFC_REPORT("NFC tag empty -> writing info record (%zu B)", info_len);
		bool write_ok = false;
		if (info_len) {
			ret = write_mem(0, info, info_len);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("write_mem", ret);
				res = ret;
			} else {
				write_ok = true;
			}
		}
		/* #340 M3: commit the arm only once the clm-bearing record actually landed
		 * on the tag (info_len == 0 would mean nothing was ever written either).
		 * Tracked via a dedicated flag, not `res`, since `res` is not touched on
		 * the write's own success path (only on failure). */
		if (just_armed) {
			if (write_ok) {
				clm_arm_commit();
			} else {
				clm_arm_revert();
			}
		}
		return res;
	}

	/* Tag already holds exactly our info record: nothing pending, leave it
	 * (avoids rewriting the EEPROM on every check). */
	if (info_len && memcmp(m_buf, info, info_len) == 0) {
		m_unknown_count = 0;
		NFC_REPORT("NFC tag holds our info record (nothing pending) -> no action");
		/* #340 M3: tag content already matches what we'd write (clm included when
		 * PENDING) - confirmed correct, not a race. */
		if (just_armed) {
			clm_arm_commit();
		}
		return 0;
	}

	/* Something else is on the tag: walk the NDEF to see whether it is still our
	 * resting content (inf, possibly stale) or foreign data. */
	ret = app_nfc_parser_run(m_buf, ST25DV_USER_MEM_SIZE, parser_callback, NULL);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_nfc_parser_run", ret);
		res = ret;
	}

	/* #247: settled resting state — our info record is on the tag (so this is not
	 * a phone mid-write, which would show neither inf nor clm). Refresh the
	 * resting record if the tag copy is stale (e.g. an older nonce high-water).
	 * (#360: this used to also latch CLM_CONSUMED here when clm was absent —
	 * removed, since that trigger fired on unauthenticated presence/absence of
	 * the plaintext clm record with no secret_key check at all.) */
	if (m_seen_inf) {
		m_unknown_count = 0;
		info_len = build_resting_ndef(info, sizeof(info));
		bool matched = info_len && memcmp(m_buf, info, info_len) == 0;
		bool write_ok = false;
		if (!matched && info_len) {
			NFC_REPORT("NFC refreshing resting record (%zu B)", info_len);
			ret = write_mem(0, info, info_len);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("write_mem", ret);
				res = ret;
			} else {
				write_ok = true;
			}
		}
		/* #340 M3: commit once the clm-bearing record is confirmed on the tag,
		 * either because it already matched or because the refresh write above
		 * just landed it; otherwise retry next poll. Tracked via a dedicated flag,
		 * not `res` (which may carry an unrelated earlier parser error). */
		if (just_armed) {
			if (matched || write_ok) {
				clm_arm_commit();
			} else {
				clm_arm_revert();
			}
		}
		return res;
	}

	/* Unrecognized data and nothing actionable. This is usually a poll catching
	 * the tag mid-write while the phone lays down a command/config record, which
	 * resolves on the next poll. Debounce: only restore the info record (which
	 * would clobber the in-progress write) after the data stays unrecognized for
	 * NFC_UNKNOWN_DEBOUNCE consecutive polls. */
	if (++m_unknown_count < NFC_UNKNOWN_DEBOUNCE) {
		NFC_REPORT("NFC unrecognized data (%u/%u) -> waiting (likely mid-write)",
			   m_unknown_count, NFC_UNKNOWN_DEBOUNCE);
		/* #340 M3: no write happened this cycle - an arm is not confirmed. */
		if (just_armed) {
			clm_arm_revert();
		}
		return res;
	}

	m_unknown_count = 0;
	LOG_INF("Writing info record to NFC (cleared unknown data)...");
	NFC_REPORT("NFC wrote: info record (%zu B) - cleared unknown data, restored metadata",
		   info_len);
	bool write_ok = false;
	if (info_len) {
		ret = write_mem(0, info, info_len);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("write_mem", ret);
			res = ret;
		} else {
			write_ok = true;
		}
	}

	/* #340 M3: commit the arm once this final restore write is confirmed, else
	 * revert so the next poll retries from scratch. Tracked via a dedicated
	 * flag, not `res` (which may carry an unrelated earlier parser error). */
	if (just_armed) {
		if (write_ok) {
			clm_arm_commit();
		} else {
			clm_arm_revert();
		}
	}

	return res;
}

/* ---- FTM mailbox command session (#313) --------------------------------------
 * The phone enables the mailbox itself (RF Write Dynamic Configuration, MB_EN=1)
 * and then ping-pongs 256 B frames through the dual-port RAM while it keeps its
 * field on — no field-off window is ever needed, which is what makes a one-tap
 * exchange possible on iOS. Frame = [chan][payload]: chan 0x01 = owner command
 * (secret_key, response cache), 0x02 = vendor command (vendor_token, no cache),
 * 0x03 = plaintext channel (reserved for the plain_text transport, PR #415 —
 * rejected here until it lands); the payload is byte-identical to the encrypted
 * hio.stck:cmd / hio.stck:rsp content, so the phone codec does not change. */
#define NFC_MB_SESSION_MAX_MS                                                                      \
	120000                          /* hard cap on one session; a long history readout         \
					 * is ~100 pages x 0.3 s, iOS itself cuts at 20 s */
#define NFC_FIELD_FAST_TICK_MS    30000 /* field-present poll: 50 ms this long, then 500 ms */
#define NFC_MB_IDLE_MS            3000  /* no RF message for this long -> session over */
#define NFC_MB_POLL_MS            20    /* MB_CTRL_Dyn poll while waiting for RF_PUT */
#define NFC_MB_HOST_PUT_WAIT_MS   1000  /* wait for the phone to read our reply */
#define NFC_MB_ERR_BUDGET         8     /* consecutive MB_CTRL_Dyn read failures -> abort */
#define NFC_MB_FIELD_OFF_DEBOUNCE 3     /* FIELD_ON=0 reads in a row -> phone gone */
#define NFC_MB_CHAN_CMD           0x01
#define NFC_MB_CHAN_VND           0x02
#define NFC_MB_CHAN_PLAIN         0x03

/* Read the message the phone put in the mailbox (RF_PUT_MSG). Reading the last
 * byte is what tells the chip the message was consumed (clears RF_PUT_MSG and
 * frees the mailbox for the next RF write), so always read the whole message. */
static int mb_read_msg(uint8_t *buf, size_t len)
{
	if (len == 0 || len > ST25DV_MB_RAM_SIZE) {
		return -EMSGSIZE;
	}
	return read_chunks(ST25DV_MB_RAM, buf, len, false);
}

/* Put our reply in the mailbox: one I2C write starting at 0x2008 (the chip sets
 * HOST_PUT_MSG + MB_LEN_Dyn at the STOP condition; a split write is not a
 * message). Frame is staged in m_buf — the request it held has been consumed by
 * then. Caller holds the access lock. */
static int mb_write_msg(const uint8_t *data, size_t len)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	if (len == 0 || len > ST25DV_MB_RAM_SIZE) {
		return -EMSGSIZE;
	}

	sys_put_be16(ST25DV_MB_RAM, m_buf);
	memcpy(&m_buf[2], data, len);

	int ret = -EIO;
	for (int attempt = 0; attempt < ST25DV_I2C_RETRIES; attempt++) {
		ret = i2c_write(dev, m_buf, 2 + len, ST25DV_I2C_ADDR_E0);
		if (ret == 0) {
			return 0;
		}
		k_msleep(ST25DV_I2C_RETRY_MS); /* RF transaction in flight on the dual port */
	}
	LOG_ERR("NFC mb: write %u B failed after %d retries: %d", (unsigned)len, ST25DV_I2C_RETRIES,
		ret);
	return ret;
}

/* Wait until the phone has read our reply (HOST_PUT_MSG cleared by its RF read of
 * the last byte). Without this a fast phone could write its next request before
 * reading the current reply, or we could overwrite an unread reply, desyncing the
 * request/response pairing (the #194 six-frame gap). Returns 0 when read,
 * -ENOTCONN when the field dropped meanwhile, -ETIMEDOUT otherwise. */
static int mb_wait_host_put_cleared(uint32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (k_uptime_get() < deadline) {
		uint8_t ctrl = 0, eh = 0;
		if (mb_read_ctrl(&ctrl) == 0 && !(ctrl & ST25DV_MB_CTRL_HOST_PUT)) {
			return 0;
		}
		if (read_reg(ST25DV_EH_CTRL_DYN, &eh, 1) == 0 && !(eh & ST25DV_FIELD_ON)) {
			return -ENOTCONN;
		}
		k_msleep(5);
	}
	return -ETIMEDOUT;
}

/* Serve mailbox requests until the phone leaves, disables the mailbox, goes
 * quiet, or a deferred action needs the session closed. Caller holds the access
 * lock with the tag powered (LPD low) for the whole session. Returns the number
 * of replies sent. Always leaves MB_EN cleared: with the mailbox enabled the chip
 * refuses every EEPROM write, so a stuck MB_EN would break the resting-record
 * upkeep (the June "truncated NDEF" symptom). */
static int mb_serve_locked(void)
{
	int64_t t0 = k_uptime_get();
	int64_t t_last = t0;
	int64_t t_keep = t0;
	int err_budget = NFC_MB_ERR_BUDGET;
	int field_off_n = 0;
	unsigned int served = 0;
	const char *reason = "?";

	nfc_keep_awake();
	nfc_led_detected();
	NFC_DBG("mb: session start");

	for (;;) {
		int64_t now = k_uptime_get();
		if (now - t0 > NFC_MB_SESSION_MAX_MS) {
			reason = "max duration";
			break;
		}
		if (now - t_last > NFC_MB_IDLE_MS) {
			reason = "idle";
			break;
		}
		if (now - t_keep >= 1000) {
			nfc_keep_awake(); /* never let Stop2 land mid-session (#329) */
			t_keep = now;
		}

		uint8_t ctrl = 0;
		if (mb_read_ctrl(&ctrl)) {
			if (--err_budget <= 0) {
				reason = "i2c errors";
				break;
			}
			k_msleep(NFC_MB_POLL_MS);
			continue;
		}
		err_budget = NFC_MB_ERR_BUDGET;

		if (!(ctrl & ST25DV_MB_CTRL_MB_EN)) {
			reason = "phone disabled MB_EN";
			break;
		}

		uint8_t eh = 0;
		if (read_reg(ST25DV_EH_CTRL_DYN, &eh, 1) == 0 && !(eh & ST25DV_FIELD_ON)) {
			if (++field_off_n >= NFC_MB_FIELD_OFF_DEBOUNCE) {
				reason = "field off";
				break;
			}
		} else {
			field_off_n = 0;
		}

		if (!(ctrl & ST25DV_MB_CTRL_RF_PUT)) {
			/* Nothing new — nap until the GPO RF_PUT_MSG pulse or the poll tick. */
			k_sem_take(&m_gpo_sem, K_MSEC(NFC_MB_POLL_MS));
			continue;
		}

		size_t len = 0;
		int ret = mb_read_len(&len);
		if (ret == 0) {
			ret = mb_read_msg(m_buf, len);
		}
		if (ret) {
			NFC_DBG("mb: read request failed: %d", ret);
			k_msleep(NFC_MB_POLL_MS);
			continue;
		}
		t_last = k_uptime_get();

		uint8_t chan = m_buf[0];
		const uint8_t *key = NULL;
		enum app_cmd_transport tp = APP_CMD_TRANSPORT_NFC;
		bool cache = true;

		if (len >= 2 && chan == NFC_MB_CHAN_CMD) {
			key = g_app_config.secret_key;
		} else if (len >= 2 && chan == NFC_MB_CHAN_VND) {
			key = g_app_config.vendor_token;
			tp = APP_CMD_TRANSPORT_VENDOR;
			cache = false;
		} else {
			/* 0x03 (plain_text, PR #415) not wired yet; anything else is noise. */
			NFC_DBG("mb: rejected frame chan=0x%02x len=%u", chan, (unsigned)len);
			nfc_led_rejected();
			continue;
		}

		nfc_led_processing();
		NFC_REPORT("mailbox: %u B request on chan 0x%02x", (unsigned)len, chan);

		size_t resp_len = 0;
		enum app_cmd_action action = APP_CMD_ACTION_NONE;
		bool replayed = false;
#ifdef CONFIG_APP_NFC_ENCRYPTION
		ret = handle_encrypted_cmd(key, tp, cache, &m_buf[1], len - 1, &m_resp_buf[1],
					   ST25DV_MB_RAM_SIZE - 1, &resp_len, &action, &replayed);
#else
		/* Plaintext validation build: no vendor channel (no key to bind it to). */
		ARG_UNUSED(key);
		ARG_UNUSED(cache);
		if (tp != APP_CMD_TRANSPORT_NFC) {
			ret = -EACCES;
		} else {
			ret = app_cmd_handle(tp, &m_buf[1], len - 1, &m_resp_buf[1],
					     ST25DV_MB_RAM_SIZE - 1, &resp_len, &action);
		}
#endif
		if (ret) {
			/* Same as the NDEF path: a frame we cannot authenticate gets no reply
			 * (#315 red blink), the phone times out. */
			NFC_DBG("mb: request rejected: %d", ret);
			nfc_led_rejected();
			continue;
		}

		if (resp_len) {
			m_resp_buf[0] = chan;
			ret = mb_write_msg(m_resp_buf, resp_len + 1);
			if (ret) {
				continue; /* budget/idle above bound the retry */
			}
			served++;
			ret = mb_wait_host_put_cleared(NFC_MB_HOST_PUT_WAIT_MS);
			NFC_REPORT("mailbox: %u B reply, read by phone: %s", (unsigned)resp_len + 1,
				   ret == 0 ? "yes" : "no");
			nfc_led_detected();
		}

		if (!replayed && action != APP_CMD_ACTION_NONE) {
			/* Reboot/save/reset: the phone has read (or had a second to read) the
			 * reply — hand the action to the poll thread and close the session so
			 * it runs with the mailbox off and the tag released (#242 equivalent). */
			if (m_cmd_action != APP_CMD_ACTION_NONE && m_cmd_action != action) {
				LOG_WRN("NFC mb: action %d supersedes pending %d", action,
					m_cmd_action);
			}
			m_cmd_action = action;
			NFC_REPORT("mailbox: deferred action: %s", cmd_action_str(action));
			reason = "deferred action";
			break;
		}
	}

	if (mb_set_en(false)) {
		LOG_WRN("NFC mb: could not disable the mailbox at session end");
	}
	nfc_led_off();
	NFC_DBG("mb: session end (%s), %u reply(ies)", reason, served);
	return (int)served;
}

/* Full NFC check: always reads the tag. Used at boot and by `nfc check`
 * (an I2C-side `nfc write` does not set the RF IT_STS_Dyn flags). */
int app_nfc_check(void)
{
	int ret = nfc_access_begin();
	if (ret) {
		return ret;
	}

	int res = nfc_check_locked();

	nfc_access_end();
	return res;
}

/* NFC service pass: read the tag and process any pending command / config,
 * restoring the info record otherwise. The poll thread calls this after
 * app_nfc_wait_event() wakes it on the GPO interrupt (low-power; no busy
 * polling). It always does the full read — software gating on IT_STS_Dyn is
 * useless here (the register reads 0x00 every pass, cleared by the LPD
 * power-cycle in nfc_access_begin), and a command can only be read / answered
 * while the RF field is briefly off, which IT_STS wouldn't flag anyway. */
int app_nfc_poll(void)
{
	int ret = nfc_access_begin();
	if (ret) {
		return ret;
	}

	int res = 0;
	int64_t t_start = k_uptime_get();

	/* Field-present mode (#313): as long as the phone holds its field we stay
	 * powered (LPD low) and keep watching MB_CTRL_Dyn, so a mailbox enabled at
	 * any point of the tap is served at once. Measured on the bench: with LPD
	 * high the chip runs on field power alone and a phone's Write Dynamic
	 * Configuration MB_EN=1 simply does not stick (VCC_ON=0), so releasing the
	 * chip while a field is present would strand the phone. The EEPROM is not
	 * touched while the field is on (that is the single-port collision the whole
	 * design avoids); the usual reconciliation runs as soon as the field drops.
	 * A reader parked on the tag costs a 500 ms tick after the first 30 s. */
	for (;;) {
		uint8_t eh = 0, ctrl = 0;
		bool field_on = read_reg(ST25DV_EH_CTRL_DYN, &eh, 1) == 0 && (eh & ST25DV_FIELD_ON);
		bool mb_en =
			m_mb_available && mb_read_ctrl(&ctrl) == 0 && (ctrl & ST25DV_MB_CTRL_MB_EN);

		if (mb_en && field_on) {
			m_mb_requested = false;
			ret = mb_serve_locked();
			if (ret < 0) {
				res = ret;
			}
			continue; /* re-read the field: the phone may be gone or may re-enable */
		}
		if (mb_en) {
			/* Mailbox enabled but no phone: left over from an aborted session
			 * (survives an MCU reset) and it blocks every EEPROM write — clear it. */
			LOG_WRN("NFC mb: stuck MB_EN with no field -> disabling");
			(void)mb_set_en(false);
		}

		if (!field_on) {
			m_mb_requested = false;
			res = nfc_check_locked();
			if (m_mb_requested) {
				continue; /* field came back with MB_EN during the read -> serve */
			}
			break;
		}

		/* Field on, mailbox off: hold the chip powered and wait for MB_EN or for
		 * the field to drop (legacy NDEF phones drop it ~1.5 s after writing —
		 * the EEPROM read then runs on the next tick). GPO pulses cut the wait. */
		nfc_keep_awake();
		int64_t elapsed = k_uptime_get() - t_start;
		k_sem_take(&m_gpo_sem, K_MSEC(elapsed < NFC_FIELD_FAST_TICK_MS ? 50 : 500));
	}

	nfc_access_end();
	return res;
}

#if defined(CONFIG_SHELL)

static int cmd_nfc_dump(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}

	ret = read_mem(0, m_buf, ST25DV_USER_MEM_SIZE);
	if (ret == 0) {
		shell_hexdump(sh, m_buf, ST25DV_USER_MEM_SIZE);
	}

	nfc_access_end();

	if (ret) {
		shell_error(sh, "read failed: %d", ret);
		return ret;
	}

	return 0;
}

static int cmd_nfc_read(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	unsigned long off = strtoul(argv[1], NULL, 0);
	unsigned long len = strtoul(argv[2], NULL, 0);

	/* Bound each operand before the sum: `off + len` wraps modulo the word size
	 * for a huge `len`, which would slip past a combined check and overrun
	 * m_buf. */
	if (len == 0 || len > ST25DV_USER_MEM_SIZE || off >= ST25DV_USER_MEM_SIZE ||
	    off > ST25DV_USER_MEM_SIZE - len) {
		shell_error(sh, "range out of 0..%d", ST25DV_USER_MEM_SIZE);
		return -EINVAL;
	}

	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}

	ret = read_mem((uint16_t)off, m_buf, len);
	if (ret == 0) {
		shell_hexdump(sh, m_buf, len);
	}

	nfc_access_end();

	if (ret) {
		shell_error(sh, "read failed: %d", ret);
		return ret;
	}

	return 0;
}

static int cmd_nfc_write(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	unsigned long off = strtoul(argv[1], NULL, 0);

	size_t n = hex2bin(argv[2], strlen(argv[2]), m_buf, ST25DV_USER_MEM_SIZE);
	if (n == 0) {
		shell_error(sh, "bad hex (or empty)");
		return -EINVAL;
	}

	if (off >= ST25DV_USER_MEM_SIZE || off + n > ST25DV_USER_MEM_SIZE) {
		shell_error(sh, "range out of 0..%d", ST25DV_USER_MEM_SIZE);
		return -EINVAL;
	}

	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}

	ret = write_mem((uint16_t)off, m_buf, n);

	nfc_access_end();

	if (ret) {
		shell_error(sh, "write failed: %d", ret);
		return ret;
	}

	shell_print(sh, "wrote %zu byte(s) at offset %lu", n, off);
	return 0;
}

static int cmd_nfc_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	memset(m_buf, 0, ST25DV_USER_MEM_SIZE);

	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}

	ret = write_mem(0, m_buf, ST25DV_USER_MEM_SIZE);

	nfc_access_end();

	if (ret) {
		shell_error(sh, "clear failed: %d", ret);
		return ret;
	}

	shell_print(sh, "cleared %d bytes", ST25DV_USER_MEM_SIZE);
	return 0;
}

static int cmd_nfc_autocheck(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "on") == 0) {
		m_periodic = true;
	} else if (strcmp(argv[1], "off") == 0) {
		m_periodic = false;
	} else {
		shell_error(sh, "usage: nfc autocheck on|off");
		return -EINVAL;
	}

	shell_print(sh, "periodic NFC check %s", m_periodic ? "on" : "off");
	return 0;
}

static int cmd_nfc_check(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* Route the check's read/decode/react/write trace to this shell. */
	m_report_sh = sh;
	int ret = app_nfc_check();
	m_report_sh = NULL;
	if (ret) {
		shell_error(sh, "nfc check failed: %d", ret);
		return ret;
	}

	return 0;
}

static int cmd_nfc_reg(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long addr = strtoul(argv[1], NULL, 0);
	unsigned long count = (argc >= 3) ? strtoul(argv[2], NULL, 0) : 1;

	if (count == 0 || count > 64) {
		shell_error(sh, "count must be 1..64");
		return -EINVAL;
	}

	uint8_t buf[64];
	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}

	ret = read_reg((uint16_t)addr, buf, count);
	nfc_access_end();

	if (ret) {
		shell_error(sh, "reg read failed: %d", ret);
		return ret;
	}

	shell_hexdump(sh, buf, count);
	return 0;
}

static int cmd_nfc_regw(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	unsigned long addr = strtoul(argv[1], NULL, 0);

	uint8_t buf[16];
	size_t n = hex2bin(argv[2], strlen(argv[2]), buf, sizeof(buf));
	if (n == 0) {
		shell_error(sh, "bad hex (or empty / too long, max 16 B)");
		return -EINVAL;
	}

	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}

	ret = write_reg((uint16_t)addr, buf, n);
	nfc_access_end();

	if (ret) {
		shell_error(sh, "reg write failed: %d", ret);
		return ret;
	}

	shell_print(sh, "wrote %zu byte(s) to reg 0x%lx", n, addr);
	return 0;
}

/* ---- `nfc mb` — FTM mailbox bench controls (#313) ---- */

static int cmd_nfc_mb_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}

	uint8_t mode = 0, ctrl = 0, eh = 0, gpo = 0, gpo_dyn = 0;
	size_t mlen = 0;
	int r_mode = read_reg(ST25DV_MB_MODE_REG, &mode, 1);
	int r_ctrl = mb_read_ctrl(&ctrl);
	int r_len = mb_read_len(&mlen);
	int r_eh = read_reg(ST25DV_EH_CTRL_DYN, &eh, 1);
	int r_gpo = read_reg(ST25DV_GPO_REG, &gpo, 1);
	int r_gdyn = read_reg(ST25DV_GPO_CTRL_DYN_REG, &gpo_dyn, 1);
	nfc_access_end();

	shell_print(sh, "mailbox available: %s",
		    m_mb_available ? "yes" : "NO (MB_MODE cfg failed)");
	shell_print(sh, "MB_MODE   (0x000D): 0x%02x%s", mode, r_mode ? " (read failed)" : "");
	shell_print(sh,
		    "MB_CTRL   (0x2006): 0x%02x  MB_EN=%d HOST_PUT=%d RF_PUT=%d HOST_MISS=%d "
		    "RF_MISS=%d%s",
		    ctrl, !!(ctrl & ST25DV_MB_CTRL_MB_EN), !!(ctrl & ST25DV_MB_CTRL_HOST_PUT),
		    !!(ctrl & ST25DV_MB_CTRL_RF_PUT), !!(ctrl & ST25DV_MB_CTRL_HOST_MISS),
		    !!(ctrl & ST25DV_MB_CTRL_RF_MISS), r_ctrl ? " (read failed)" : "");
	shell_print(sh, "MB_LEN    (0x2007): msg %u B%s", (unsigned)mlen,
		    r_len ? " (read failed)" : "");
	shell_print(sh, "EH_CTRL   (0x2002): 0x%02x  FIELD_ON=%d VCC_ON=%d%s", eh,
		    !!(eh & ST25DV_FIELD_ON), !!(eh & ST25DV_VCC_ON), r_eh ? " (read failed)" : "");
	shell_print(sh, "GPO static (0x0000): 0x%02x  GPO_CTRL_Dyn (0x2000): 0x%02x%s", gpo,
		    gpo_dyn, (r_gpo || r_gdyn) ? " (read failed)" : "");
	return 0;
}

static int cmd_nfc_mb_en(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	bool enable = strcmp(argv[0], "on") == 0;

	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}
	ret = mb_set_en(enable);
	nfc_access_end();

	if (ret) {
		shell_error(sh, "MB_EN=%d failed: %d", enable, ret);
		return ret;
	}
	shell_print(sh, "MB_EN=%d", enable);
	return 0;
}

/* Bench helper: enable the mailbox from the I2C side and serve it, for readers
 * that cannot send Write Dynamic Configuration themselves. Runs the same session
 * loop the poll thread uses; a deferred action is only reported, not executed. */
static int cmd_nfc_mb_serve(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}
	ret = mb_set_en(true);
	if (ret) {
		nfc_access_end();
		shell_error(sh, "MB_EN=1 failed: %d", ret);
		return ret;
	}
	m_report_sh = sh;
	int served = mb_serve_locked();
	m_report_sh = NULL;
	nfc_access_end();

	shell_print(sh, "mailbox session over: %d reply(ies)%s", served,
		    m_cmd_action != APP_CMD_ACTION_NONE
			    ? " (deferred action pending for the poll thread)"
			    : "");
	return served < 0 ? served : 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_nfc_mb,
	SHELL_CMD_ARG(status, NULL, "Show FTM mailbox registers.", cmd_nfc_mb_status, 1, 0),
	SHELL_CMD_ARG(on, NULL, "Enable the mailbox (MB_EN=1).", cmd_nfc_mb_en, 1, 0),
	SHELL_CMD_ARG(off, NULL, "Disable the mailbox (MB_EN=0).", cmd_nfc_mb_en, 1, 0),
	SHELL_CMD_ARG(serve, NULL, "Enable + serve the mailbox until idle/field-off.",
		      cmd_nfc_mb_serve, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_nfc, SHELL_CMD_ARG(dump, NULL, "Hex dump all 512 B of NFC memory.", cmd_nfc_dump, 1, 0),
	SHELL_CMD_ARG(read, NULL, "Read a range. Usage: read <offset> <len>", cmd_nfc_read, 3, 0),
	SHELL_CMD_ARG(write, NULL, "Write hex bytes. Usage: write <offset> <hexbytes>",
		      cmd_nfc_write, 3, 0),
	SHELL_CMD_ARG(clear, NULL, "Zero all 512 B of NFC memory.", cmd_nfc_clear, 1, 0),
	SHELL_CMD_ARG(autocheck, NULL, "Enable/disable periodic check. Usage: autocheck on|off",
		      cmd_nfc_autocheck, 2, 0),
	SHELL_CMD_ARG(check, NULL, "Reconcile the resting NDEF record now.", cmd_nfc_check, 1, 0),
	SHELL_CMD_ARG(reg, NULL, "Read system/dynamic register (E1). Usage: reg <addr> [count]",
		      cmd_nfc_reg, 2, 1),
	SHELL_CMD_ARG(regw, NULL, "Write system/dynamic register (E1). Usage: regw <addr> <hex>",
		      cmd_nfc_regw, 3, 0),
	SHELL_CMD(mb, &sub_nfc_mb, "FTM mailbox: status|on|off|serve (#313).", NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(nfc, &sub_nfc, "ST25DV NFC memory access (debug).", NULL);

#endif /* CONFIG_SHELL */
