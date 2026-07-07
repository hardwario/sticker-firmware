/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_nfc.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_nfc_parser.h"
#include "app_settings.h"
#include "app_version.h"
#include "app_log.h"

/* Nanopb includes */
#include <pb_decode.h>
#include "src/app_config.pb.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

/* PSA Crypto includes */
#include <mbedtls/ccm.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
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
	case APP_CMD_ACTION_FACTORY_RESET:
		return "factory-reset";
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

/* Temporary poll-thread diagnostics for the GetConfig-vs-GetInfo i2c -5 hunt
 * (#204). The debug build pins CONFIG_LOG_MAX_LEVEL=2, so LOG_INF/LOG_DBG are
 * compiled out and only LOG_WRN/LOG_ERR reach RTT — route these through LOG_WRN
 * so they are visible. Remove once the read-wedge is understood. */
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

/* Static system config (device E1 0x57). Writing it needs an open I2C security
 * session (present password). GPO bit6 RF_WRITE_EN makes RF EEPROM writes show
 * up in IT_STS_Dyn, letting the poll gate on writes specifically. */
#define ST25DV_GPO_REG         0x0000
#define ST25DV_GPO_RF_WRITE_EN 0x40
#define ST25DV_I2C_PWD_REG     0x0900

/* NFC Forum external type (TNF=0x04, urn:nfc:ext:) records. Short type names
 * ("hio.stck:<kind>") instead of full MIME media-types save ST25DV user memory
 * (512 B total) — leaving more room for the encrypted config payload — while
 * staying typed/filterable: Web NFC exposes them as record.recordType. */
#define NDEF_TNF_EXT 0x04

/* Plaintext info record the firmware keeps on the tag so a phone learns the
 * sticker identity and schema version the moment it taps, without decrypting
 * anything. Payload layout (15 bytes):
 *   [0]     format version (NDEF_INFO_FORMAT)
 *   [1..4]  serial number (big-endian uint32)
 *   [5]     fw major   [6] fw minor   [7] fw patch
 *   [8]     build type (0=main, 1=dev, 2=custom)
 *   [9]     config/schema version (APP_CONFIG_VERSION)
 *   [10]    flags: bit0 = debug build
 *   [11..14] NFC anti-replay counter high-water (big-endian uint32) — the last
 *            accepted nonce_counter, so a phone can resync its counter (= this
 *            value + 1) after a device reboot/cache-miss without an encrypted
 *            exchange. Not secret (it travels in plaintext in every header).
 */
#define NDEF_INFO_TYPE   "hio.stck:inf"
#define NDEF_INFO_FORMAT 0x02

/* Command/response over NDEF. The phone writes a command record (raw protobuf
 * Command, like a LoRaWAN downlink); the firmware processes it via app_cmd and
 * replaces it with a response record (0x01 version + protobuf Response). */
#define NDEF_COMMAND_TYPE  "hio.stck:cmd"
#define NDEF_RESPONSE_TYPE "hio.stck:rsp"
/* The phone writes this over the response record once it has read and accepted
 * the reply (#164): an explicit "consumed" ack, so the firmware restores the
 * info record deterministically instead of guessing from RF-field timing (which
 * raced the phone's read and clobbered the reply mid-exchange). */
#define NDEF_ACK_TYPE      "hio.stck:ack"

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

/* Periodic NFC check enable (toggled via `nfc autocheck`). Lets a config blob
 * be written over several `nfc write` calls without the periodic check racing
 * it and rewriting the tag to the info record mid-write. */
static bool m_periodic = true;

/* Command/response state, filled by parser_callback when an NDEF command record
 * is found and consumed by nfc_check_locked (writes the response to the tag). */
static uint8_t m_resp_buf[512];
static size_t m_resp_len;
static bool m_have_resp; /* a command was processed; m_resp_buf holds the reply to write */
static bool m_seen_resp; /* tag already holds a response record; leave it for the phone */
static bool m_seen_ack;  /* tag holds the phone's ack: reply consumed, restore info now */
/* A response is staged in m_resp_buf/m_resp_len but not yet fully written to the
 * tag (the RF field came back before the write landed). While set, the poll
 * re-attempts the write in the next field-off window WITHOUT re-reading or
 * re-running the command — same principle as the gated read: don't regenerate,
 * retry the EEPROM transfer until a clean window lands it. */
static bool m_resp_write_pending;
static enum app_cmd_action m_cmd_action; /* deferred action from app_cmd_handle */

/* Gate for m_cmd_action. A deferred action (reboot/save/factory-reset/...) is
 * only handed to the poll thread once its response has actually been delivered
 * to the phone: after the phone acks the reply and we restore the info record,
 * or after the quiet backstop restores it if the phone leaves without acking.
 * This makes a reboot fire *after* the phone has read the response — extending
 * the #164 read handshake to the action — instead of the old blind fixed delay
 * that raced the phone read. */
static bool m_cmd_action_ready;

/* Debounce for the "unrecognized data -> restore info" path. A poll can catch
 * the tag mid-write (the phone is still laying down a command/config record),
 * which parses as garbage; restoring the info record then would clobber what
 * the phone is writing (and any reply we just produced). So only restore info
 * after the data stays unrecognized for this many consecutive polls — a real
 * partial write resolves within one poll. A recognized consumed config (sets
 * *action) is still cleared immediately for anti-replay. */
#define NFC_UNKNOWN_DEBOUNCE 3
static uint8_t m_unknown_count;

/* #164: after a command/response exchange the response record is left on the tag
 * for the phone to read. This flag is set while it sits unread and cleared when
 * the info record is (re)written. The info record is restored deterministically
 * when the phone writes its ack (NDEF_ACK_TYPE — it has read the reply); the
 * poll thread also restores it after a long RF-quiet window (~10 s, no GPO →
 * phone gone) as a backstop for when no ack arrives, so a later tap always finds
 * valid info instead of a stale response. The earlier "restore on the field
 * edge" heuristic was dropped: it raced the phone's read and clobbered the
 * reply mid-exchange (#144). */
static bool m_info_restore_pending;

/* Take (and clear) the deferred action from the last processed NFC command, so
 * the poll thread can run reboot/save AFTER the response was written and read.
 * Returns APP_CMD_ACTION_NONE until the response has been delivered to the phone
 * (m_cmd_action_ready), so a reboot never cuts off a reply the phone has not yet
 * read. */
enum app_cmd_action app_nfc_take_cmd_action(void)
{
	if (!m_cmd_action_ready) {
		return APP_CMD_ACTION_NONE;
	}

	enum app_cmd_action a = m_cmd_action;
	m_cmd_action = APP_CMD_ACTION_NONE;
	m_cmd_action_ready = false;
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
 * past the wait (the chunk loop then aborts with -EBUSY; the caller skips). */
static bool nfc_wait_field_off(void);

static int read_mem(uint16_t reg, void *buf, size_t len)
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
		if (!nfc_wait_field_off()) {
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

/* Set GPO RF_WRITE_EN so RF EEPROM writes are reported in IT_STS_Dyn. Needs the
 * default (all-zero) I2C password. Returns 0 if RF_WRITE reporting is active.
 * Caller must hold the access lock. */
static int nfc_enable_rf_write_it(void)
{
	static const uint8_t default_pwd[8] = {0};

	int ret = nfc_present_password(default_pwd);
	if (ret) {
		return ret;
	}

	/* The chip needs a moment after a present-password write before the next
	 * I2C access is ACKed. 5 ms was too short (the next read NACKed with -EIO);
	 * a system-area probe confirmed 10 ms is enough, so allow a safe margin. */
	k_msleep(15);

	uint8_t gpo;
	ret = read_reg(ST25DV_GPO_REG, &gpo, 1);
	if (ret) {
		return ret;
	}

	if (gpo & ST25DV_GPO_RF_WRITE_EN) {
		return 0; /* already enabled */
	}

	gpo |= ST25DV_GPO_RF_WRITE_EN;
	ret = write_reg(ST25DV_GPO_REG, &gpo, 1);
	if (ret) {
		return ret;
	}

	/* GPO is a system-config register held in EEPROM: wait the write time
	 * before reading it back, or the verify read sees the stale value. */
	k_msleep(ST25DV_TW_MS_PER_PAGE + 5);

	uint8_t check = 0;
	ret = read_reg(ST25DV_GPO_REG, &check, 1);
	if (ret) {
		return ret;
	}

	return (check & ST25DV_GPO_RF_WRITE_EN) ? 0 : -EIO;
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
 * field is still present after NFC_FIELD_OFF_WAIT_MS (caller should skip this
 * cycle). FAIL-OPEN: if the status read itself fails, returns true so the path
 * falls back to the existing I2C-retry behaviour (never worse than before). The
 * caller must already hold the access lock (tag powered via nfc_access_begin). */
static bool nfc_wait_field_off(void)
{
	bool waited_for_field = false;
	for (int waited = 0; waited <= NFC_FIELD_OFF_WAIT_MS; waited += NFC_FIELD_POLL_MS) {
		uint8_t eh;
		int ret = read_reg(ST25DV_EH_CTRL_DYN, &eh, 1);
		if (ret != 0) {
			NFC_DBG("field: EH_CTRL_Dyn read=%d -> proceed (fail-open)", ret);
			return true; /* can't tell -> proceed (fall back to I2C retry) */
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
		k_msleep(NFC_FIELD_POLL_MS);
	}
	NFC_DBG("field: still on after %d ms -> abort EEPROM chunk", NFC_FIELD_OFF_WAIT_MS);
	return false;
}

/* Build CC + NDEF Message TLV + a single NFC-Forum-external-type record +
 * terminator into `out`. Returns the byte length, or 0 if it would not fit the
 * tag (out_size). Uses an NDEF short record (1-byte payload length) when the
 * payload is <= 255 B, else a normal record (4-byte length) so a full config
 * dump (encrypted ConfigDump can exceed 255 B) still fits the 512 B EEPROM;
 * the NDEF Message TLV likewise switches to its 3-byte length form when needed.
 * Shared by the info and command-response writers. */
static size_t build_ndef_record(uint8_t *out, size_t out_size, const char *type,
				const uint8_t *payload, size_t payload_len)
{
	size_t type_len = strlen(type);

	/* Short record encodes the payload length in 1 byte; a normal record in 4. */
	bool short_record = payload_len <= 0xFF;
	size_t len_field = short_record ? 1 : 4;

	/* NDEF record: flags + type_len + payload_len_field + type + payload */
	size_t msg_len = 1 + 1 + len_field + type_len + payload_len;

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
	/* MB | ME | [SR] | TNF=NFC Forum external (0x04). */
	out[i++] = short_record ? 0xD4 : 0xC4;
	out[i++] = (uint8_t)type_len;
	if (short_record) {
		out[i++] = (uint8_t)payload_len;
	} else {
		out[i++] = (uint8_t)(payload_len >> 24);
		out[i++] = (uint8_t)(payload_len >> 16);
		out[i++] = (uint8_t)(payload_len >> 8);
		out[i++] = (uint8_t)(payload_len & 0xFF);
	}
	memcpy(&out[i], type, type_len);
	i += type_len;
	memcpy(&out[i], payload, payload_len);
	i += payload_len;
	out[i++] = 0xFE; /* Terminator TLV */

	return i;
}

/* Build the plaintext info NDEF into `out`. Stable between accepted NFC commands
 * (no uptime/clock; the nonce counter advances only on an accepted command), so
 * app_nfc_check() can compare it to the tag content and skip rewriting when
 * already present, and rewrite it when the counter has moved. */
static size_t build_info_ndef(uint8_t *out, size_t out_size)
{
	struct app_cmd_info info;
	app_cmd_get_info(&info);

	uint8_t payload[15];
	payload[0] = NDEF_INFO_FORMAT;
	sys_put_be32(info.serial_number, &payload[1]);
	payload[5] = info.fw_major;
	payload[6] = info.fw_minor;
	payload[7] = info.fw_patch;
	payload[8] = info.build_type;
	payload[9] = (uint8_t)g_app_config.config_version;
	payload[10] = info.debug ? 0x01 : 0x00;
	/* Anti-replay counter high-water (live source of truth, == what decrypt()
	 * checks against) so the phone can resync after a reboot/cache-miss. */
	sys_put_be32(app_config()->nonce_counter, &payload[11]);

	return build_ndef_record(out, out_size, NDEF_INFO_TYPE, payload, sizeof(payload));
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

/* An all-zero secret key means the device has not been provisioned yet. Mirror
 * the bootloader's auth.c (all_zero() -> unkeyed): refuse to take part in the
 * encrypted channel at all while unkeyed. Otherwise, with
 * CONFIG_APP_NFC_ENCRYPTION=y (default), the device would AES-CCM-decrypt and
 * execute any command forged under the public all-zero key (serial is public,
 * nonce_counter starts at 0) — an attacker in NFC range could rewrite LoRaWAN
 * keys, factory-reset or wedge the device with valid CCM tags. In the unkeyed
 * state only the plaintext info record is served. */
static bool secret_key_is_set(void)
{
	for (size_t i = 0; i < sizeof(g_app_config.secret_key); i++) {
		if (g_app_config.secret_key[i] != 0) {
			return true;
		}
	}

	return false;
}

/* Initialise an AES-CCM context with the device secret key. The caller frees it
 * with mbedtls_ccm_free(). Returns 0 or a negative errno. Shared by
 * decrypt()/encrypt() so the key setup lives in one place.
 *
 * We use mbedtls_ccm directly rather than the PSA crypto API: the PSA dispatch
 * layer + key-slot management costs ~2.5 KB of flash the release image can't
 * spare, and CCM produces identical wire bytes either way (PSA's CCM backend is
 * this same mbedtls_ccm), so the phone-side contract and the nfc_crypto golden
 * vectors are unchanged. */
static int ccm_setkey(mbedtls_ccm_context *ctx)
{
	mbedtls_ccm_init(ctx);

	int ret = mbedtls_ccm_setkey(ctx, MBEDTLS_CIPHER_ID_AES, g_app_config.secret_key,
				     8 * sizeof(g_app_config.secret_key));
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("mbedtls_ccm_setkey", ret);
		mbedtls_ccm_free(ctx);
		return -EIO;
	}

	return 0;
}

static int decrypt(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size, size_t *out_len)
{
	int res = 0;

	if (!secret_key_is_set()) {
		LOG_ERR("Secret key not provisioned; rejecting encrypted request");
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

	mbedtls_ccm_context ctx;
	res = ccm_setkey(&ctx);
	if (res) {
		return res;
	}

	uint8_t nonce[NFC_NONCE_LEN];
	memcpy(nonce, in, 8);
	nonce[8] = NFC_NONCE_DIR_REQUEST;

	int ret = mbedtls_ccm_auth_decrypt(&ctx, pt_len, nonce, sizeof(nonce), /* AAD */ in, 8,
					   &in[8], out, &in[8 + pt_len], NFC_CCM_TAG_LEN);
	mbedtls_ccm_free(&ctx);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("mbedtls_ccm_auth_decrypt", ret);
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
static int encrypt(const uint8_t *in, size_t in_len, uint32_t nonce_counter, uint8_t *out,
		   size_t out_size, size_t *out_len)
{
	int res = 0;

	if (!secret_key_is_set()) {
		LOG_ERR("Secret key not provisioned; refusing to emit encrypted response");
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

	mbedtls_ccm_context ctx;
	res = ccm_setkey(&ctx);
	if (res) {
		return res;
	}

	uint8_t nonce[NFC_NONCE_LEN];
	memcpy(nonce, out, 8);
	nonce[8] = NFC_NONCE_DIR_RESPONSE;

	/* Ciphertext (== plaintext length) into &out[8], the 16 B tag right after it. */
	int ret = mbedtls_ccm_encrypt_and_tag(&ctx, in_len, nonce, sizeof(nonce), /* AAD */ out, 8,
					      in, &out[8], &out[8 + in_len], NFC_CCM_TAG_LEN);
	mbedtls_ccm_free(&ctx);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("mbedtls_ccm_encrypt_and_tag", ret);
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
 * the phone resyncs from the info-record counter. */
static uint32_t m_resp_cache_counter;
static uint8_t m_resp_cache_buf[512];
static size_t m_resp_cache_len;

/* Process one encrypted command frame (the NDEF command channel).
 * Three-way on the nonce counter vs the stored high-water:
 *   counter == cached  -> retransmission: replay the cached response, do NOT re-run
 *   counter  > stored  -> new: decrypt (advances+persists the counter), run, cache
 *   counter <= stored  -> stale replay: decrypt() rejects with -EACCES
 * Writes the encrypted reply into out_buf/out_len and the deferred action into
 * *action (NONE on a replay — the original already ran). Sets *replayed=true on a
 * same-counter retransmission so the caller keeps a deferred action still waiting
 * for the phone's ack instead of clearing it. Returns 0 or -errno. */
static int handle_encrypted_cmd(const uint8_t *in, size_t in_len, uint8_t *out_buf, size_t out_cap,
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

	if (serial == g_app_config.serial_number && m_resp_cache_len > 0 &&
	    counter == m_resp_cache_counter) {
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
	int ret = decrypt(in, in_len, cmd_plain, sizeof(cmd_plain), &cmd_len);
	if (ret) {
		NFC_DBG("cmd: decrypt failed=%d", ret);
		return ret;
	}
	uint32_t req_nonce = app_config()->nonce_counter;
	NFC_DBG("cmd: decrypt ok, cmd_len=%zu", cmd_len);

	size_t resp_len = 0;
	ret = app_cmd_handle(APP_CMD_TRANSPORT_NFC, cmd_plain, cmd_len, resp_plain,
			     sizeof(resp_plain), &resp_len, action);
	if (ret) {
		NFC_DBG("cmd: app_cmd_handle failed=%d", ret);
		return ret;
	}
	NFC_DBG("cmd: handled, resp_len=%zu", resp_len);
	if (resp_len == 0) {
		return 0;
	}

	ret = encrypt(resp_plain, resp_len, req_nonce, out_buf, out_cap, out_len);
	if (ret) {
		return ret;
	}

	/* Cache for an idempotent retransmission of this counter. */
	if (*out_len <= sizeof(m_resp_cache_buf)) {
		memcpy(m_resp_cache_buf, out_buf, *out_len);
		m_resp_cache_len = *out_len;
		m_resp_cache_counter = req_nonce;
	} else {
		m_resp_cache_len = 0;
	}
	return 0;
}
#endif /* CONFIG_APP_NFC_ENCRYPTION */

static int parser_callback(const struct app_nfc_parser_record_info *record_info, void *user_data)
{
	int ret;

	ARG_UNUSED(user_data);

	/* Only our NFC Forum external-type records carry sticker payloads. */
	if (record_info->tnf != NDEF_TNF_EXT) {
		return 0;
	}

	/* Command record: run the protobuf Command through app_cmd and stage the
	 * response for nfc_check_locked to write back to the tag. With encryption on
	 * (default) the record is decrypted first and the reply encrypted back;
	 * without it (validation build) both are plaintext. */
	size_t cmd_type_len = strlen(NDEF_COMMAND_TYPE);
	if (record_info->type_len == cmd_type_len &&
	    strncmp((const char *)record_info->type, NDEF_COMMAND_TYPE, cmd_type_len) == 0) {
		LOG_INF("Found command record - length: %u byte(s)", record_info->payload_len);
		NFC_DBG("parsed: command record, payload=%u B", record_info->payload_len);
		NFC_REPORT("NFC read: command record (%u B)", record_info->payload_len);
		NFC_REPORT_HEX("  command (protobuf):", record_info->payload,
			       record_info->payload_len);

		enum app_cmd_action cmd_action = APP_CMD_ACTION_NONE;
		bool replayed = false;
#ifdef CONFIG_APP_NFC_ENCRYPTION
		/* Encrypted channel: decrypt, run, encrypt the reply (RESPONSE-direction
		 * nonce so it never reuses the request nonce), with same-counter
		 * retransmission served from the response cache. */
		ret = handle_encrypted_cmd(record_info->payload, record_info->payload_len,
					   m_resp_buf, sizeof(m_resp_buf), &m_resp_len, &cmd_action,
					   &replayed);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("handle_encrypted_cmd", ret);
			NFC_REPORT("  -> command rejected: %d", ret);
			m_resp_len = 0;
			return ret;
		}
#else
		/* Plaintext channel (validation build): hand the raw protobuf Command
		 * to app_cmd and stage the plaintext response. No response cache, so no
		 * replay handling. */
		(void)replayed;
		ret = app_cmd_handle(APP_CMD_TRANSPORT_NFC, record_info->payload,
				     record_info->payload_len, m_resp_buf, sizeof(m_resp_buf),
				     &m_resp_len, &cmd_action);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_cmd_handle", ret);
			NFC_REPORT("  -> app_cmd_handle failed: %d", ret);
			m_resp_len = 0;
			return ret;
		}
#endif /* CONFIG_APP_NFC_ENCRYPTION */

		m_have_resp = (m_resp_len > 0);
		/* A same-counter retransmission replays the cached reply without
		 * re-running the command — keep any action still awaiting the phone's ack
		 * instead of clearing it. A fresh execution sets the action: it waits for
		 * the ack (set ready in the m_seen_ack / backstop paths) when there is a
		 * reply to deliver, or fires immediately when there is nothing to read. */
		if (!replayed) {
			/* M-5: don't clobber a deferred action from an earlier command that
			 * has not been dispatched yet (the phone sent another command before
			 * acking the first reply). Overwriting it would silently drop a pending
			 * reboot/save. Keep the pending action — it still fires once the phone
			 * acks (m_seen_ack) — and drop the new command's own action instead.
			 * A well-behaved app acks between commands (#242), so this only guards
			 * the misbehaving case. */
			if (m_cmd_action == APP_CMD_ACTION_NONE) {
				m_cmd_action = cmd_action;
				m_cmd_action_ready =
					(cmd_action != APP_CMD_ACTION_NONE && !m_have_resp);
			} else if (cmd_action != APP_CMD_ACTION_NONE) {
				LOG_WRN("NFC: action %d still pending; ignoring new command action "
					"%d",
					(int)m_cmd_action, (int)cmd_action);
			}
		}
		NFC_REPORT("  -> handled, response %zu B, deferred action: %s", m_resp_len,
			   cmd_action_str(cmd_action));
		return 0;
	}

	/* Response record already on the tag (our previous reply): leave it so the
	 * phone can read it; don't overwrite with the info record. */
	size_t resp_type_len = strlen(NDEF_RESPONSE_TYPE);
	if (record_info->type_len == resp_type_len &&
	    strncmp((const char *)record_info->type, NDEF_RESPONSE_TYPE, resp_type_len) == 0) {
		m_seen_resp = true;
		NFC_REPORT("NFC read: our response record still on tag (%u B) - leaving it for "
			   "the phone",
			   record_info->payload_len);
		return 0;
	}

	/* Ack record: the phone wrote it over our response to confirm it read the
	 * reply (#164). nfc_check_locked restores the info record now — deterministic,
	 * no RF-timing guess, so the restore can never clobber an unread reply. */
	size_t ack_type_len = strlen(NDEF_ACK_TYPE);
	if (record_info->type_len == ack_type_len &&
	    strncmp((const char *)record_info->type, NDEF_ACK_TYPE, ack_type_len) == 0) {
		m_seen_ack = true;
		NFC_REPORT("NFC read: phone ack record - reply consumed, restoring info");
		return 0;
	}

	/* #250: the legacy bare-AppConfigMessage config record (hio.stck:cmd's sibling
	 * "hio.stck:cfg") has been retired — offline/boot-staged provisioning now goes
	 * through the encrypted Command/SetParam record handled above, so any other
	 * record type is just unrecognized data. */
	NFC_REPORT("NFC read: unrecognized record type (%u B) -> ignored",
		   record_info->payload_len);
	return 0;
}

static bool is_buffer_zero(const void *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (((const uint8_t *)buf)[i] != 0) {
			return false;
		}
	}

	return true;
}

/* ST25DV GPO interrupt handler: wake the NFC poll thread to service the tag. */
static void gpo_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_sem_give(&m_gpo_sem);
}

/* Block the NFC poll thread until the GPO line signals RF activity, or until
 * `fallback_ms` elapses (a safety net so a missed edge can't stall the
 * channel). Returns 0 if woken by GPO, -EAGAIN on the fallback timeout. */
int app_nfc_wait_event(int fallback_ms)
{
	return k_sem_take(&m_gpo_sem, K_MSEC(fallback_ms));
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

	/* NOTE: do NOT write the info record here. The boot-time app_nfc_check() (and
	 * the poll thread) already lay it down on a blank tag and restore it after a
	 * consumed config/command — and crucially they run AFTER reading the tag, so a
	 * config/command written over NFC while the device was powered off is ingested
	 * first. Writing the info record at init would overwrite that pending record
	 * before it is read, breaking power-off provisioning (SetParam-applied-at-boot,
	 * #147). */
	return 0;
}

/* Write the staged response (m_resp_buf/m_resp_len) to the tag as a response
 * record. write_mem gates every EEPROM chunk on the RF field being off; if the
 * field is up the write defers (-EBUSY) and m_resp_write_pending stays set so the
 * next field-off poll re-attempts it — the encrypted reply is never regenerated,
 * just rewritten, the same principle as the gated read. Returns 0 (deferred or
 * done) or -errno on a hard error. Caller holds the access lock; m_buf is free
 * scratch (a pending write skips the tag read, so its contents are unused). */
static int nfc_write_response(void)
{
	size_t out_len = build_ndef_record(m_buf, ST25DV_USER_MEM_SIZE, NDEF_RESPONSE_TYPE,
					   m_resp_buf, m_resp_len);
	if (out_len == 0) {
		LOG_ERR("NFC response record too large (%zu B payload)", m_resp_len);
		m_resp_write_pending = false;
		return -EMSGSIZE;
	}

	int ret = write_mem(0, m_buf, out_len);
	if (ret == -EBUSY) {
		/* RF field up -> the write could not land this window; keep it pending and
		 * retry on the next field-off poll (no re-read, no regeneration). */
		NFC_DBG("resp: write deferred (field on), %zu B still pending", out_len);
		return 0;
	}
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("write_mem", ret);
		m_resp_write_pending = false;
		return ret;
	}

	NFC_DBG("resp: wrote %zu B record to tag", out_len);
	NFC_REPORT("NFC wrote: response record (%zu B NDEF, %zu B payload)", out_len, m_resp_len);
	m_resp_write_pending = false;
	/* #164: a response record now sits on the tag -> arm the info-restore. It is
	 * restored when the phone acks the reply (NDEF_ACK_TYPE), or after the quiet
	 * backstop if no ack arrives. */
	m_info_restore_pending = true;
	return 0;
}

/* Core NFC check: read the tag, parse/ingest a pending config, and restore the
 * info record. Caller must hold the access lock (nfc_access_begin). */
static int nfc_check_locked(void)
{
	int ret;
	int res = 0;

	/* A response from an earlier cycle is still waiting to be written (the RF
	 * field came back before the write landed). Retry it in this field-off window
	 * WITHOUT re-reading the tag or re-running the command — the reply is cached
	 * in m_resp_buf, so just rewrite it until a clean window lands it. */
	if (m_resp_write_pending) {
		return nfc_write_response();
	}

	m_have_resp = false;
	m_seen_resp = false;
	m_seen_ack = false;

	/* read_mem / write_mem below gate every EEPROM chunk on the RF field being
	 * off (see nfc_wait_field_off): a 512 B access during RF collides with the
	 * phone on the shared i2c1 bus and can wedge it, starving the watchdog feeder
	 * in the main loop (all sensors share i2c1) -> a 10 s SoC reset with no panic
	 * dump. The sensors keep using i2c1 unaffected. */

	/* Build the expected info record up front (no I2C); used both to detect
	 * "tag already holds our info" and to (re)write it. */
	uint8_t info[80];
	size_t info_len = build_info_ndef(info, sizeof(info));

	ret = read_mem(0, m_buf, ST25DV_USER_MEM_SIZE);
	if (ret == -EBUSY) {
		/* RF field stayed on through the read -> skip this cycle (benign); the
		 * GPO event / fallback re-polls once the field is quiet again. */
		return 0;
	}
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("read_mem", ret);
		return ret;
	}
	NFC_DBG("poll: tag read ok, CC=%02x TLV=%02x len=%02x rec=%02x", m_buf[0], m_buf[4],
		m_buf[5], m_buf[6]);

	/* Empty tag: lay down the info record so a phone always finds metadata. */
	if (is_buffer_zero(m_buf, ST25DV_USER_MEM_SIZE)) {
		m_unknown_count = 0;
		m_info_restore_pending = false; /* #164: writing info record below */
		NFC_REPORT("NFC tag empty -> writing info record (%zu B)", info_len);
		if (info_len) {
			ret = write_mem(0, info, info_len);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("write_mem", ret);
				res = ret;
			}
		}
		return res;
	}

	/* Tag already holds exactly our info record: nothing pending, leave it
	 * (avoids rewriting the EEPROM on every check). */
	if (info_len && memcmp(m_buf, info, info_len) == 0) {
		m_unknown_count = 0;
		m_info_restore_pending = false; /* #164: info record is present */
		NFC_REPORT("NFC tag holds our info record (nothing pending) -> no action");
		return 0;
	}

	/* Pending data written by a phone: parse it. parser_callback may ingest a
	 * config (sets *action), or process a command (stages m_resp_buf), or flag
	 * that the tag already holds our response (m_seen_resp). */
	ret = app_nfc_parser_run(m_buf, ST25DV_USER_MEM_SIZE, parser_callback, NULL);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_nfc_parser_run", ret);
		res = ret;
	}

	/* A command was processed: stage the reply for writing. The reply is in
	 * m_resp_buf; mark it pending and write it (resuming across field-off windows
	 * if the field interrupts — see nfc_write_response). The record is built into
	 * m_buf there (its parsed contents are no longer needed). */
	if (m_have_resp) {
		m_unknown_count = 0;
		LOG_INF("Writing command response to NFC (%zu B)...", m_resp_len);
		NFC_REPORT_HEX("  response (0x01 ver + protobuf Response):", m_resp_buf,
			       m_resp_len);
		m_resp_write_pending = true;
		return nfc_write_response();
	}

	/* The phone wrote its ack over our response: it has read and accepted the
	 * reply, so restore the info record now (#164). Deterministic — unlike the
	 * dropped field-timing heuristic, this can never clobber an unread reply. */
	if (m_seen_ack) {
		m_unknown_count = 0;
		m_info_restore_pending = false;
		/* The phone has read and acked the reply: a deferred action (reboot/save)
		 * may now fire without cutting off an unread response. */
		if (m_cmd_action != APP_CMD_ACTION_NONE) {
			m_cmd_action_ready = true;
		}
		LOG_INF("Restoring info record after phone ack (#164)...");
		NFC_REPORT("NFC wrote: info record (%zu B) - restored after phone ack", info_len);
		if (info_len) {
			ret = write_mem(0, info, info_len);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("write_mem", ret);
				res = ret;
			}
		}
		return res;
	}

	/* Our response is already on the tag (awaiting the phone read): leave it.
	 * It is restored to the info record on the phone's ack, or by the quiet
	 * backstop if the phone leaves without acking. */
	if (m_seen_resp) {
		m_unknown_count = 0;
		m_info_restore_pending = true;
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
		return res;
	}

	m_unknown_count = 0;
	m_info_restore_pending = false; /* #164: writing info record below */
	LOG_INF("Writing info record to NFC (cleared unknown data)...");
	NFC_REPORT("NFC wrote: info record (%zu B) - cleared unknown data, restored metadata",
		   info_len);
	if (info_len) {
		ret = write_mem(0, info, info_len);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("write_mem", ret);
			res = ret;
		}
	}

	return res;
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
	/* Identical to app_nfc_check() — both do a full tag read under the access
	 * lock; kept as a separate entry point for call-site clarity (#220.F). */
	return app_nfc_check();
}

/* #164: true while a response record is left on the tag and the info record has
 * not yet been restored. The poll thread uses this to shorten its wait and to
 * decide whether to restore the info record once the field goes quiet. */
bool app_nfc_info_restore_pending(void)
{
	return m_info_restore_pending;
}

/* True while a command response is staged but not yet fully written to the tag
 * (the RF field interrupted the write). The poll thread uses this to shorten its
 * wait and to re-run app_nfc_poll() — which rewrites the cached reply — instead
 * of restoring the info record (which would clobber the pending response). */
bool app_nfc_resp_write_pending(void)
{
	return m_resp_write_pending;
}

/* #164: rewrite the plaintext info record over whatever is on the tag (a stale
 * response record), so a later passive read finds valid metadata. Called by the
 * poll thread only after the RF field has been quiet for the debounce window
 * (no GPO events ~10 s → the phone has left), which avoids the #144 race of
 * clobbering the reply while the phone may still be reading it. */
int app_nfc_restore_info(void)
{
	uint8_t info[80];
	size_t info_len = build_info_ndef(info, sizeof(info));

	if (!info_len) {
		return -EINVAL;
	}

	int ret = nfc_access_begin();
	if (ret) {
		return ret;
	}

	ret = write_mem(0, info, info_len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("write_mem", ret);
	} else {
		m_info_restore_pending = false;
		/* Backstop: the phone left without acking the reply. Release any deferred
		 * action now so a reboot/save is never stuck waiting for an ack that will
		 * not come. */
		if (m_cmd_action != APP_CMD_ACTION_NONE) {
			m_cmd_action_ready = true;
		}
		LOG_INF("NFC: restored info record after field loss (#164)");
	}

	nfc_access_end();
	return ret;
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

	if (len == 0 || off >= ST25DV_USER_MEM_SIZE || off + len > ST25DV_USER_MEM_SIZE) {
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

	/* #250: a staged hio.stck:cmd leaves a response on the tag with the deferred
	 * action gated. Mirror the boot path — restore the info record (opens the gate)
	 * and apply the provisioning action here so a bench `nfc check` behaves like a
	 * boot-staged provision. */
	if (app_nfc_info_restore_pending()) {
		app_nfc_restore_info();
	}
	enum app_cmd_action act = app_nfc_take_cmd_action();
	switch (act) {
	case APP_CMD_ACTION_SETTINGS_SAVE:
		ret = app_settings_save(true); /* reboots on success */
		shell_print(sh, "staged config applied%s", ret ? " (save failed!)" : " and saved");
		break;
	case APP_CMD_ACTION_FACTORY_RESET:
		ret = app_settings_reset();
		shell_print(sh, "factory reset%s", ret ? " (failed!)" : "");
		break;
	case APP_CMD_ACTION_NONE:
		shell_print(sh, "no staged command on tag (no action)");
		break;
	default:
		shell_print(sh, "staged command action %d (applied by poll thread)", (int)act);
		break;
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

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_nfc, SHELL_CMD_ARG(dump, NULL, "Hex dump all 512 B of NFC memory.", cmd_nfc_dump, 1, 0),
	SHELL_CMD_ARG(read, NULL, "Read a range. Usage: read <offset> <len>", cmd_nfc_read, 3, 0),
	SHELL_CMD_ARG(write, NULL, "Write hex bytes. Usage: write <offset> <hexbytes>",
		      cmd_nfc_write, 3, 0),
	SHELL_CMD_ARG(clear, NULL, "Zero all 512 B of NFC memory.", cmd_nfc_clear, 1, 0),
	SHELL_CMD_ARG(autocheck, NULL, "Enable/disable periodic check. Usage: autocheck on|off",
		      cmd_nfc_autocheck, 2, 0),
	SHELL_CMD_ARG(check, NULL, "Run the NFC check now (parse + apply config).", cmd_nfc_check,
		      1, 0),
	SHELL_CMD_ARG(reg, NULL, "Read system/dynamic register (E1). Usage: reg <addr> [count]",
		      cmd_nfc_reg, 2, 1),
	SHELL_CMD_ARG(regw, NULL, "Write system/dynamic register (E1). Usage: regw <addr> <hex>",
		      cmd_nfc_regw, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(nfc, &sub_nfc, "ST25DV NFC memory access (debug).", NULL);

#endif /* CONFIG_SHELL */
