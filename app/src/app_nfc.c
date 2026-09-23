/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_nfc.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_led.h" /* NFC interaction LED signalling */
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
/* When set (by a bench shell command), the mailbox session emits a human-
 * readable trace to this shell: the request read off the mailbox, how the
 * firmware reacted, and the reply written back. NULL for the poll thread (it
 * only logs over RTT). Set/cleared around the traced call. */
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

#define ST25DV_TW_MS_PER_PAGE 5 /* EEPROM write time (static system config: GPO, MB_MODE) */

/* Dynamic register EH_CTRL_Dyn (device E0, addr 0x2002): bit2 FIELD_ON reports
 * whether an RF field is currently present. Dynamic registers live in the
 * dual-port area and are safe to read while RF is active (unlike the 512 B
 * user-memory EEPROM, whose accesses collide with RF on the shared i2c1 bus and
 * can wedge it — which is why the firmware no longer touches the user EEPROM at
 * all, #313). The mailbox session and the field-present hold poll this bit. */
#define ST25DV_EH_CTRL_DYN 0x2002
#define ST25DV_FIELD_ON    0x04

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

/* Shared scratch buffer for the mailbox: the request read from the FTM RAM, then
 * the reply staged behind its 2-byte register address (mb_write_msg). Always
 * used while holding m_lock, which serialises the poll thread (app_nfc_poll) and
 * the `nfc` shell commands against each other on the I2C bus and LPD pin. */
static uint8_t m_buf[2 + ST25DV_MB_RAM_SIZE];
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

/* --- NFC interaction LED signalling (#414) ----------------------------------
 * Guides whoever holds the phone against the sticker through a mailbox tap:
 *   phone detected (RF field)                -> green solid, at most 5 s
 *   mailbox session running                  -> green blink
 *   session ended, last exchange OK          -> green + yellow, 2 s
 *   session ended, last exchange failed      -> red, 2 s
 *   otherwise / afterwards                   -> off
 * "Failed" = the last request was rejected (wrong key / nonce / unknown channel,
 * no reply is sent), its reply could not be written or was never read by the
 * phone, or the session aborted on I2C errors. An authenticated Response.error
 * is a valid reply, i.e. OK. The last exchange decides, so an app that resyncs
 * after a rejection and then succeeds ends green + yellow. A deferred action
 * (save / reset / reboot) waits for the result to be shown before it reboots
 * (app_nfc_led_result_wait(), main.c); the boot carousel follows the reboot.
 * Timers only, so nothing blocks the NFC path. The helpers are called from the
 * poll thread, the GPO ISR (detected) and the timer handlers, so each one runs
 * under irq_lock; app_led_set is a plain gpio write and k_timer calls are
 * ISR-safe. Every state sets all three channels, so no two states can blend. */
#define NFC_LED_BLINK_MS  90   /* session blink half-period */
#define NFC_LED_DETECT_MS 5000 /* "phone detected" cap when no session starts */
#define NFC_LED_RESULT_MS 2000 /* session result (OK / error) */

static atomic_t m_led_state = ATOMIC_INIT(APP_NFC_LED_OFF);
static bool m_led_blink_on;

static void nfc_led_set3(bool r, bool g, bool y)
{
	app_led_set(APP_LED_CHANNEL_R, r ? APP_LED_ON : APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_G, g ? APP_LED_ON : APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_Y, y ? APP_LED_ON : APP_LED_OFF);
}

static void nfc_led_blink_timer(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	unsigned int key = irq_lock();
	if (atomic_get(&m_led_state) == APP_NFC_LED_SESSION) {
		m_led_blink_on = !m_led_blink_on;
		app_led_set(APP_LED_CHANNEL_G, m_led_blink_on ? APP_LED_ON : APP_LED_OFF);
	}
	irq_unlock(key);
}
static K_TIMER_DEFINE(m_led_blink_timer, nfc_led_blink_timer, NULL);

static void nfc_led_off(void);
static void nfc_led_hold_timeout(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	nfc_led_off();
}
/* Ends the time-limited states (detected, result). */
static K_TIMER_DEFINE(m_led_hold_timer, nfc_led_hold_timeout, NULL);

static void nfc_led_off(void)
{
	unsigned int key = irq_lock();
	k_timer_stop(&m_led_blink_timer);
	k_timer_stop(&m_led_hold_timer);
	nfc_led_set3(false, false, false);
	atomic_set(&m_led_state, APP_NFC_LED_OFF);
	irq_unlock(key);
}

static void nfc_led_detected(void)
{
	unsigned int key = irq_lock();
	k_timer_stop(&m_led_blink_timer);
	nfc_led_set3(false, true, false);
	atomic_set(&m_led_state, APP_NFC_LED_DETECTED);
	k_timer_start(&m_led_hold_timer, K_MSEC(NFC_LED_DETECT_MS), K_NO_WAIT);
	irq_unlock(key);
}

static void nfc_led_session(void)
{
	unsigned int key = irq_lock();
	k_timer_stop(&m_led_hold_timer);
	m_led_blink_on = true;
	nfc_led_set3(false, true, false);
	atomic_set(&m_led_state, APP_NFC_LED_SESSION);
	k_timer_start(&m_led_blink_timer, K_MSEC(NFC_LED_BLINK_MS), K_MSEC(NFC_LED_BLINK_MS));
	irq_unlock(key);
}

static void nfc_led_result(bool ok)
{
	unsigned int key = irq_lock();
	k_timer_stop(&m_led_blink_timer);
	nfc_led_set3(!ok, ok, ok);
	atomic_set(&m_led_state, ok ? APP_NFC_LED_RESULT_OK : APP_NFC_LED_RESULT_ERR);
	k_timer_start(&m_led_hold_timer, K_MSEC(NFC_LED_RESULT_MS), K_NO_WAIT);
	irq_unlock(key);
}

enum app_nfc_led_state app_nfc_led_state_get(void)
{
	return (enum app_nfc_led_state)atomic_get(&m_led_state);
}

void app_nfc_led_result_wait(void)
{
	/* Bounded: the hold timer ends a result after NFC_LED_RESULT_MS. */
	for (int waited = 0; waited <= NFC_LED_RESULT_MS + 100; waited += 20) {
		enum app_nfc_led_state st = app_nfc_led_state_get();
		if (st != APP_NFC_LED_RESULT_OK && st != APP_NFC_LED_RESULT_ERR) {
			return;
		}
		k_msleep(20);
	}
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
 * session (phone just arrived) -> light the "detected" LED (capped at 5 s). */
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

/* Reply staging for the mailbox session (mb_serve_locked): [chan][encrypted
 * Response], at most one 256 B frame. */
static uint8_t m_resp_buf[512];

/* #247/#415 claim window, persisted in its own "clm" settings subtree (key
 * "clm/state"), not the config blob - so a device_reset/factory_reset that
 * preserves identity leaves it intact; only a full NVS erase, an explicit
 * claim_active, or vendor_reset re-opens it. #415 C3: two explicit states, no
 * automatic behaviour.
 *   ACTIVE  factory default - the device may still be claimed: get_claim_info
 *           discloses the claim_token.
 *   DONE    claiming finished (claim_done command / `ats claim done`):
 *           get_claim_info returns NOT_READY.
 * Mutators are explicit only (claim_done / claim_active / vendor_reset). There
 * is no auto-arm on a provisioned token and no implicit close on a decrypted
 * command - both removed in #415 (the #247 tri-state's PENDING/CONSUMED and the
 * #308/#340-M3 arm-commit/consume machinery are gone).
 *
 * Deliberately NOT guarded by m_lock: the claim state no longer touches the tag,
 * and m_lock is held by the poll thread for a whole mailbox session (up to
 * NFC_MB_SESSION_MAX_MS) or field-present hold. app_cmd_get_info() reads the
 * state on m_work_q (GetInfo-on-join, the clock-sync Info, a LoRaWAN get_info
 * downlink), which must never wait that long - its 30 s liveness heartbeat would
 * go stale and the IWDG reset the device. Reads are a lock-free atomic_get();
 * writers serialise on the short-held m_claim_lock so a set + persist pair is
 * never interleaved with another writer's. */
enum claim_state {
	CLAIM_ACTIVE = APP_NFC_CLAIM_ACTIVE,
	CLAIM_DONE = APP_NFC_CLAIM_DONE,
};
/* Factory default when the "clm/state" key is absent (fresh NVS): ACTIVE. */
static atomic_t m_claim_state = ATOMIC_INIT(CLAIM_ACTIVE);
static K_MUTEX_DEFINE(m_claim_lock);

/* Load handler for the "clm" settings subtree (key "clm/state"). Migrates the
 * legacy #247 tri-state in place: unset(0)/pending(1) -> ACTIVE, consumed(2) ->
 * DONE, any other byte -> ACTIVE (safe default). */
static int clm_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "state", NULL)) {
		uint8_t stored;
		if (len != sizeof(stored)) {
			return -EINVAL;
		}
		ssize_t r = read_cb(cb_arg, &stored, sizeof(stored));
		if (r < 0) {
			return (int)r;
		}
		atomic_set(&m_claim_state, (stored == CLAIM_DONE) ? CLAIM_DONE : CLAIM_ACTIVE);
		return 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(app_clm, "clm", NULL, clm_settings_set, NULL, NULL);

static void clm_state_save(uint8_t state)
{
	int ret = settings_save_one("clm/state", &state, sizeof(state));
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save_one(clm/state)", ret);
	}
}

/* Set the claim window state and persist it. The mutators are reached from the
 * poll thread (claim_done inside a mailbox session, the deferred claim_active /
 * vendor_reset actions) and from the shell (app_ats.c); m_claim_lock serialises
 * them (#340 M24). It is never held across anything slow but the one-key
 * settings_save_one(), and never taken together with m_lock in the other order,
 * so a writer inside a mailbox session (m_lock held) cannot deadlock. */
static void claim_state_set(uint8_t state, const char *reason)
{
	k_mutex_lock(&m_claim_lock, K_FOREVER);
	if ((uint8_t)atomic_get(&m_claim_state) != state) {
		atomic_set(&m_claim_state, state);
		clm_state_save(state);
		LOG_INF("NFC claim window -> %s (%s) (#415)",
			state == CLAIM_DONE ? "done" : "active", reason);
	}
	k_mutex_unlock(&m_claim_lock);
}

/* #415: claiming finished - get_claim_info refuses from now on. Reached from the
 * claim_done command (secret_key-encrypted owner channel / shell; the vendor
 * channel is not allow-listed) and `ats claim done`. */
void app_nfc_claim_done(void)
{
	claim_state_set(CLAIM_DONE, "claim_done command");
}

/* #415: (re)open the claim window (factory default). Reached from the
 * claim_active command, `ats claim active`, and vendor_reset. */
void app_nfc_claim_active(void)
{
	claim_state_set(CLAIM_ACTIVE, "claim_active command");
}

/* Lock-free: safe from any thread, never waits on a mailbox session (see above). */
uint8_t app_nfc_claim_state_get(void)
{
	return (uint8_t)atomic_get(&m_claim_state);
}

static enum app_cmd_action m_cmd_action; /* deferred action from app_cmd_handle */

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

/* The ST25DV is dual-port: an I2C access concurrent with an RF transaction can
 * be NACKed (-EIO) by the arbiter — common while a phone holds its field open
 * waiting for our reply. The transfers are short-lived, so a brief retry rides
 * out the contention. Chunking a long read also means a collision only retries
 * a small block, not the whole 256 B mailbox message. */
#define ST25DV_I2C_RETRIES  20
#define ST25DV_I2C_RETRY_MS 2
#define ST25DV_READ_CHUNK   64

/* Chunked I2C read from the user-memory device (E0), used for the FTM mailbox
 * RAM (0x2008..0x2107). Dual-port by design: served to I2C while the phone holds
 * its field, which is the whole point of the mailbox — so no field-off wait here
 * (the old app_nfc_serve_mailbox read the mailbox through a field-gated EEPROM
 * path and could therefore never see a message under a held field). The debug
 * `nfc read` shell also reads the user EEPROM through it, after checking the
 * field is off. Each chunk rides out arbitration NACKs with a short retry. */
static int read_chunks(uint16_t reg, void *buf, size_t len)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	uint8_t *p = buf;
	size_t off = 0;
	while (off < len) {
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
			LOG_ERR("read_chunks @0x%04x +%u: i2c -EIO after %d retries (RF "
				"contention?)",
				(unsigned)(reg + off), (unsigned)chunk, ST25DV_I2C_RETRIES);
			return ret;
		}
		NFC_DBG("rd @0x%04x +%u ok (tries=%d)", (unsigned)(reg + off), (unsigned)chunk,
			attempt + 1);
		off += chunk;
	}

	return 0;
}

/* ST25DV register device select: dynamic registers (>=0x2000, e.g. EH_CTRL_Dyn
 * 0x2002, GPO_Dyn 0x2000) live on the user-memory device (E0 0x53); the static
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
 * under a held RF field. */

/* Whether FTM could be authorised on this chip at boot (MB_MODE set/verified).
 * false = the mailbox command channel does not work on this unit; reported as
 * APP_DEVICE_STATUS_MAILBOX_DOWN so the production tester rejects it (#313 D7). */
static bool m_mb_available;

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
 * state only the plaintext get_basic_info / get_claim_info bootstrap answers.
 * Same guard applies to vendor_token (#299, #316): an unprovisioned device also
 * refuses the vendor channel. */
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

	/* #415 C3/D10: the claim window no longer closes implicitly on a decrypted
	 * command (the #308 behaviour). It closes only on an explicit claim_done, so
	 * the app must send one after storing the keys - a crash in between leaves
	 * the token readable on a powered unit (accepted: ATELOS refuses a second
	 * claim of the same serial; only the token leaks, not control). */

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

	/* #247/#415: restore the claim window latch from its own settings subtree
	 * (settings subsystem already brought up by app_config_init; idempotent here).
	 * A missing key leaves the factory default ACTIVE. */
	(void)settings_subsys_init();
	ret = settings_load_subtree("clm");
	if (ret) {
		LOG_WRN("NFC: clm state load failed: %d (defaulting ACTIVE)", ret);
	}
	LOG_INF("NFC: claim state = %u", (unsigned)atomic_get(&m_claim_state));

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

	/* #313: the tag holds no NDEF record any more — the phone reads identity via
	 * the mailbox get_basic_info command and runs every command through the
	 * mailbox, so there is nothing to lay down or reconcile on the EEPROM here. */
	m_ready = true;
	return 0;
}

/* ---- FTM mailbox command session (#313) --------------------------------------
 * The phone enables the mailbox itself (RF Write Dynamic Configuration, MB_EN=1)
 * and then ping-pongs 256 B frames through the dual-port RAM while it keeps its
 * field on — no field-off window is ever needed, which is what makes a one-tap
 * exchange possible on iOS. Frame = [chan][payload]: chan 0x01 = owner command
 * (secret_key, response cache), 0x02 = vendor command (vendor_token, no cache),
 * 0x03 = plaintext (the unauthenticated plain_text transport, #415 — a raw
 * Command -> 0x01||Response, allow-list gated: get_basic_info / get_claim_info).
 * The 0x01/0x02 payload is byte-identical to the old encrypted hio.stck:cmd /
 * hio.stck:rsp content, so the phone codec does not change. */
#define NFC_MB_SESSION_MAX_MS                                                                      \
	120000                           /* hard cap on one session; a long history readout        \
					  * is ~100 pages x 0.3 s, iOS itself cuts at 20 s */
#define NFC_FIELD_FAST_TICK_MS    30000  /* field-present poll: 50 ms this long, then 500 ms */
#define NFC_FIELD_PRESENT_MAX_MS  120000 /* field held w/o mailbox reply -> release the tag */
#define NFC_MB_IDLE_MS            3000   /* no RF message for this long -> session over */
#define NFC_MB_POLL_MS            20     /* MB_CTRL_Dyn poll while waiting for RF_PUT */
#define NFC_MB_HOST_PUT_WAIT_MS   1000   /* wait for the phone to read our reply */
#define NFC_MB_ERR_BUDGET         8      /* consecutive MB_CTRL_Dyn read failures -> abort */
#define NFC_MB_FIELD_OFF_DEBOUNCE 3      /* FIELD_ON=0 reads in a row -> phone gone */
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
	return read_chunks(ST25DV_MB_RAM, buf, len);
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
	/* LED result (see the LED block): the outcome of the latest request decides. */
	bool any = false;
	bool last_ok = false;
	bool io_fail = false;

	nfc_keep_awake();
	nfc_led_session();
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
				io_fail = true;
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
			any = true;
			last_ok = false;
			k_msleep(NFC_MB_POLL_MS);
			continue;
		}
		t_last = k_uptime_get();
		any = true;
		last_ok = false; /* until its reply is written */

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
		} else if (len >= 2 && chan == NFC_MB_CHAN_PLAIN) {
			/* Unencrypted, unauthenticated (#415): a raw Command whose reply is
			 * 0x01||Response. The generated dispatch allow-list decides which
			 * commands answer (get_basic_info, get_claim_info) and rejects the
			 * rest — no key, no nonce, no cache, no claim side effects. */
			tp = APP_CMD_TRANSPORT_PLAIN_TEXT;
			cache = false;
		} else {
			NFC_DBG("mb: rejected frame chan=0x%02x len=%u", chan, (unsigned)len);
			continue;
		}

		NFC_REPORT("mailbox: %u B request on chan 0x%02x", (unsigned)len, chan);

		size_t resp_len = 0;
		enum app_cmd_action action = APP_CMD_ACTION_NONE;
		bool replayed = false;
		if (tp == APP_CMD_TRANSPORT_PLAIN_TEXT) {
			ret = app_cmd_handle(tp, &m_buf[1], len - 1, &m_resp_buf[1],
					     ST25DV_MB_RAM_SIZE - 1, &resp_len, &action);
		} else {
#ifdef CONFIG_APP_NFC_ENCRYPTION
			ret = handle_encrypted_cmd(key, tp, cache, &m_buf[1], len - 1,
						   &m_resp_buf[1], ST25DV_MB_RAM_SIZE - 1,
						   &resp_len, &action, &replayed);
#else
			/* Plaintext validation build: no vendor channel (no key to bind it). */
			ARG_UNUSED(key);
			ARG_UNUSED(cache);
			ret = (tp == APP_CMD_TRANSPORT_NFC)
				      ? app_cmd_handle(tp, &m_buf[1], len - 1, &m_resp_buf[1],
						       ST25DV_MB_RAM_SIZE - 1, &resp_len, &action)
				      : -EACCES;
#endif
		}
		if (ret) {
			/* A frame we cannot authenticate gets no reply; the phone times out.
			 * The session keeps going (the app may resync) — the LED shows red
			 * at the end only if nothing succeeds after this. */
			NFC_DBG("mb: request rejected: %d", ret);
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
		}
		last_ok = true; /* reply written (a still-unread one is re-checked at the end) */

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

	/* Our last reply never read by the phone (lifted too early / app gone)?
	 * Must be checked before MB_EN is cleared, which drops the message. */
	uint8_t end_ctrl = 0;
	if (last_ok && mb_read_ctrl(&end_ctrl) == 0 && (end_ctrl & ST25DV_MB_CTRL_HOST_PUT)) {
		last_ok = false;
	}
	if (mb_set_en(false)) {
		LOG_WRN("NFC mb: could not disable the mailbox at session end");
	}
	if (io_fail || (any && !last_ok)) {
		nfc_led_result(false);
	} else if (any) {
		nfc_led_result(true);
	} else {
		nfc_led_off(); /* mailbox enabled but no request: nothing to report */
	}
	NFC_DBG("mb: session end (%s), %u reply(ies), result %s", reason, served,
		io_fail || (any && !last_ok) ? "error" : (any ? "ok" : "none"));
	return (int)served;
}

/* NFC service pass: serve the ST25DV FTM mailbox while the phone holds its field
 * (#313, the one-tap command channel). The poll thread calls this after
 * app_nfc_wait_event() wakes it on the GPO interrupt (low-power; no busy
 * polling). The tag holds no NDEF record — software gating on IT_STS_Dyn is
 * useless here anyway (the register reads 0x00 every pass, cleared by the LPD
 * power-cycle in nfc_access_begin). Returns once the field is gone, a mailbox
 * command staged a deferred action, or the field has been held for
 * NFC_FIELD_PRESENT_MAX_MS without mailbox traffic. */
int app_nfc_poll(void)
{
	/* Without FTM authorised (#313 D7) the phone cannot enable the mailbox and the
	 * tag holds no NDEF, so a field can never turn into a session: do not power
	 * the chip or hold the CPU out of Stop2 waiting for one. */
	if (!m_mb_available) {
		return 0;
	}

	int ret = nfc_access_begin();
	if (ret) {
		return ret;
	}

	/* Start of this pass, then the end of each session that served a reply: the
	 * field-present hold below gives up NFC_FIELD_PRESENT_MAX_MS after it. */
	int64_t t_activity = k_uptime_get();
	bool first_pass = true;

	/* Field-present mode (#313): as long as the phone holds its field we stay
	 * powered (LPD low) and keep watching MB_CTRL_Dyn, so a mailbox enabled at
	 * any point of the tap is served at once. Measured on the bench: with LPD
	 * high the chip runs on field power alone and a phone's Write Dynamic
	 * Configuration MB_EN=1 simply does not stick (VCC_ON=0), so releasing the
	 * chip while a field is present would strand the phone. The EEPROM is not
	 * touched while the field is on (that is the single-port collision the whole
	 * design avoids). A reader parked on the tag costs a 500 ms tick after the
	 * first 30 s, and is let go after NFC_FIELD_PRESENT_MAX_MS without traffic. */
	for (;;) {
		uint8_t eh = 0, ctrl = 0;
		bool field_on = read_reg(ST25DV_EH_CTRL_DYN, &eh, 1) == 0 && (eh & ST25DV_FIELD_ON);
		bool mb_en = mb_read_ctrl(&ctrl) == 0 && (ctrl & ST25DV_MB_CTRL_MB_EN);
		int64_t idle = k_uptime_get() - t_activity;
		bool held_too_long = idle >= NFC_FIELD_PRESENT_MAX_MS;

		/* A tap the GPO edge did not light (the keep-awake lock was still held
		 * from a moment ago): show "detected" once, if nothing else is shown. */
		if (first_pass && field_on && !mb_en &&
		    app_nfc_led_state_get() == APP_NFC_LED_OFF) {
			nfc_led_detected();
		}
		first_pass = false;

		if (mb_en && field_on && !held_too_long) {
			if (mb_serve_locked() > 0) {
				t_activity = k_uptime_get(); /* a live exchange: restart the hold */
			}
			if (m_cmd_action != APP_CMD_ACTION_NONE) {
				/* A command staged a deferred action (reboot/save/reset/...):
				 * return now so the poll thread runs it even if the phone still
				 * holds its field. Staying here would let the phone re-enable the
				 * mailbox and run further commands against the not-yet-applied
				 * state, and a second action would replace this one (e.g. a reboot
				 * dropping a staged secret_key save the phone was already acked
				 * for). Re-arm the poll so a non-rebooting action (lrw_join,
				 * counters save, ...) is followed straight by a new pass: a phone
				 * still holding the field can re-enable the mailbox and go on. */
				k_sem_give(&m_gpo_sem);
				break;
			}
			continue; /* re-read the field: the phone may be gone or may re-enable */
		}
		if (mb_en) {
			/* Mailbox left enabled with no field (an aborted session, survives an
			 * MCU reset), or still enabled by a phone when the hold below gives
			 * up: clear it so a later session starts clean and the GPO reflects
			 * reality. */
			if (!field_on) {
				LOG_WRN("NFC mb: stuck MB_EN with no field -> disabling");
			}
			(void)mb_set_en(false);
		}

		if (!field_on) {
			break; /* mailbox-only: no NDEF/EEPROM reconciliation to run */
		}

		if (held_too_long) {
			/* A reader parked on the tag with no mailbox traffic (a phone left
			 * lying on the STICKER, a fixed reader nearby): stop holding the chip
			 * powered and the CPU out of Stop2. Drop the GPO events this hold
			 * already collected so the poll thread sleeps until the field actually
			 * changes (phone lifted / a new tap) instead of re-entering at once. */
			LOG_WRN("NFC: field held %u s without mailbox traffic -> releasing the tag",
				(unsigned)(idle / 1000));
			k_sem_reset(&m_gpo_sem);
			break;
		}

		/* Field on, mailbox off: hold the chip powered and wait for MB_EN (the
		 * phone enables it after finding no NDEF on the tag) or for the field to
		 * drop. GPO pulses cut the wait. */
		nfc_keep_awake();
		k_sem_take(&m_gpo_sem, K_MSEC(idle < NFC_FIELD_FAST_TICK_MS ? 50 : 500));
	}

	nfc_access_end();
	return 0;
}

#if defined(CONFIG_SHELL)

/* ---- `nfc read|write|clear` — raw user-EEPROM bench access (debug only) ----
 * The firmware itself never touches the 512 B user EEPROM (#313); these exist
 * only to inspect or edit a tag by hand, e.g. wipe the stale v1.4.x NDEF records
 * (possibly a plaintext clm claim token) off a unit reflashed to v1.5.0. The
 * EEPROM is single-port: an access under an RF field collides on the shared i2c1
 * bus and can wedge it, and with MB_EN=1 the chip refuses every write — so each
 * command refuses while a field is present and clears MB_EN first. */
#define NFC_EEPROM_SIZE        512
#define NFC_EEPROM_READ_CHUNK  64 /* read + print granularity (stack buffer) */
#define NFC_EEPROM_WRITE_CHUNK 16 /* page-aligned write; never crosses a 256 B row */
#define NFC_EEPROM_WRITE_MAX   64 /* one `nfc write` (the shell caps an arg at ~128 hex) */

/* Power the tag and make it safe for an EEPROM access: no RF field, mailbox off.
 * On success the caller holds the access lock and must nfc_access_end(). */
static int eeprom_access_begin(const struct shell *sh)
{
	int ret = nfc_access_begin();
	if (ret) {
		shell_error(sh, "nfc access failed: %d", ret);
		return ret;
	}

	uint8_t eh = 0;
	ret = read_reg(ST25DV_EH_CTRL_DYN, &eh, 1);
	if (ret == 0 && (eh & ST25DV_FIELD_ON)) {
		ret = -EBUSY;
	}
	if (ret == 0) {
		ret = mb_set_en(false);
	}
	if (ret) {
		nfc_access_end();
		shell_error(sh, "%s (%d)",
			    ret == -EBUSY ? "RF field present - remove the phone and retry"
					  : "tag not ready",
			    ret);
	}
	return ret;
}

/* Write `len` bytes at EEPROM offset `off` in page-aligned chunks, waiting out
 * the EEPROM programming time (5 ms per 4 B page) after each one — the tag must
 * not lose power (LPD high) mid-program. `data` NULL writes zeros. */
static int eeprom_write(uint16_t off, const uint8_t *data, size_t len)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
	uint8_t frame[2 + NFC_EEPROM_WRITE_CHUNK];

	while (len) {
		size_t chunk = MIN(len, NFC_EEPROM_WRITE_CHUNK - (off % NFC_EEPROM_WRITE_CHUNK));

		sys_put_be16(off, frame);
		if (data) {
			memcpy(&frame[2], data, chunk);
			data += chunk;
		} else {
			memset(&frame[2], 0, chunk);
		}

		int ret = -EIO;
		for (int attempt = 0; attempt < ST25DV_I2C_RETRIES && ret; attempt++) {
			ret = i2c_write(dev, frame, 2 + chunk, ST25DV_I2C_ADDR_E0);
			if (ret) {
				k_msleep(ST25DV_I2C_RETRY_MS);
			}
		}
		if (ret) {
			return ret;
		}
		k_msleep(DIV_ROUND_UP((off % 4) + chunk, 4) * ST25DV_TW_MS_PER_PAGE);

		off += chunk;
		len -= chunk;
	}
	return 0;
}

static int cmd_nfc_read(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	unsigned long off = strtoul(argv[1], NULL, 0);
	unsigned long len = strtoul(argv[2], NULL, 0);

	/* Bound each operand before the sum: `off + len` wraps for a huge `len`. */
	if (len == 0 || len > NFC_EEPROM_SIZE || off > NFC_EEPROM_SIZE - len) {
		shell_error(sh, "range out of 0..%d", NFC_EEPROM_SIZE);
		return -EINVAL;
	}

	int ret = eeprom_access_begin(sh);
	if (ret) {
		return ret;
	}

	uint8_t buf[NFC_EEPROM_READ_CHUNK];

	for (size_t done = 0; done < len && ret == 0;) {
		size_t chunk = MIN(len - done, sizeof(buf));

		ret = read_chunks((uint16_t)(off + done), buf, chunk);
		for (size_t i = 0; ret == 0 && i < chunk; i += SHELL_HEXDUMP_BYTES_IN_LINE) {
			shell_hexdump_line(sh, off + done + i, &buf[i],
					   MIN(chunk - i, SHELL_HEXDUMP_BYTES_IN_LINE));
		}
		done += chunk;
	}
	nfc_access_end();

	if (ret) {
		shell_error(sh, "read failed: %d", ret);
	}
	return ret;
}

static int cmd_nfc_write(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	unsigned long off = strtoul(argv[1], NULL, 0);
	uint8_t data[NFC_EEPROM_WRITE_MAX];
	size_t n = hex2bin(argv[2], strlen(argv[2]), data, sizeof(data));

	if (n == 0) {
		shell_error(sh, "bad hex (empty, invalid or over %d B)", NFC_EEPROM_WRITE_MAX);
		return -EINVAL;
	}
	if (off > NFC_EEPROM_SIZE - n) {
		shell_error(sh, "range out of 0..%d", NFC_EEPROM_SIZE);
		return -EINVAL;
	}

	int ret = eeprom_access_begin(sh);
	if (ret) {
		return ret;
	}
	ret = eeprom_write((uint16_t)off, data, n);
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

	int ret = eeprom_access_begin(sh);
	if (ret) {
		return ret;
	}
	ret = eeprom_write(0, NULL, NFC_EEPROM_SIZE);
	nfc_access_end();

	if (ret) {
		shell_error(sh, "clear failed: %d", ret);
		return ret;
	}
	shell_print(sh, "cleared %d bytes", NFC_EEPROM_SIZE);
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
	sub_nfc,
	SHELL_CMD_ARG(read, NULL, "Read user EEPROM. Usage: read <offset> <len>", cmd_nfc_read, 3,
		      0),
	SHELL_CMD_ARG(write, NULL, "Write user EEPROM (<= 64 B). Usage: write <offset> <hex>",
		      cmd_nfc_write, 3, 0),
	SHELL_CMD_ARG(clear, NULL, "Zero all 512 B of user EEPROM (wipes any NDEF).", cmd_nfc_clear,
		      1, 0),
	SHELL_CMD_ARG(reg, NULL, "Read system/dynamic register (E1). Usage: reg <addr> [count]",
		      cmd_nfc_reg, 2, 1),
	SHELL_CMD_ARG(regw, NULL, "Write system/dynamic register (E1). Usage: regw <addr> <hex>",
		      cmd_nfc_regw, 3, 0),
	SHELL_CMD(mb, &sub_nfc_mb, "FTM mailbox: status|on|off|serve (#313).", NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(nfc, &sub_nfc, "ST25DV NFC EEPROM, registers + FTM mailbox (debug).", NULL);

#endif /* CONFIG_SHELL */
