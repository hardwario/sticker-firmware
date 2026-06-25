/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_nfc.h"
#include "app_cmd.h"
#include "app_config_ingest.h"
#include "app_config.h"
#include "app_ndef_parser.h"
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
#include <psa/crypto.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(app_nfc, LOG_LEVEL_DBG);

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

/* Mailbox / Fast Transfer Mode (FTM). A 256 B dual-port RAM that the RF and I2C
 * sides exchange messages through *while the RF field is present* (unlike user
 * EEPROM, which the ST25DV can't serve to RF and I2C at once). Used for fast
 * config streaming and (later) firmware update — the phone writes a command via
 * the ISO 15693 Write Message custom command, the firmware reads it over I2C,
 * runs it, and writes the reply back for the phone to Read Message.
 *
 * MB_MODE (static, E1 0x000D, password-protected) bit0 must be 1 to allow the
 * mailbox at all; MB_CTRL_Dyn (dynamic, E0 0x2006) MB_EN then turns it on at
 * runtime. MB_LEN_Dyn (E0 0x2007) holds (message length - 1); the message bytes
 * live in MAILBOX_RAM (E0 0x2008..0x2107). */
#define ST25DV_MB_MODE_REG      0x000D /* static, E1; bit0 = mailbox allowed */
#define ST25DV_MB_MODE_EN       0x01
#define ST25DV_MB_CTRL_DYN      0x2006 /* dynamic, E0 */
#define ST25DV_MB_CTRL_MB_EN    0x01   /* bit0: mailbox enable */
#define ST25DV_MB_CTRL_HOST_PUT 0x02   /* bit1: host (I2C) wrote a message */
#define ST25DV_MB_CTRL_RF_PUT   0x04   /* bit2: RF wrote a message */
#define ST25DV_MB_LEN_DYN       0x2007 /* dynamic, E0; holds (msg length - 1) */
#define ST25DV_MB_RAM           0x2008 /* dynamic, E0; 256 B mailbox RAM */
#define ST25DV_MB_RAM_SIZE      256

/* NFC Forum external type (TNF=0x04, urn:nfc:ext:) records. Short type names
 * ("hio.stck:<kind>") instead of full MIME media-types save ST25DV user memory
 * (512 B total) — leaving more room for the encrypted config payload — while
 * staying typed/filterable: Web NFC exposes them as record.recordType. */
#define NDEF_TNF_EXT        0x04
#define NDEF_SUPPORTED_TYPE "hio.stck:cfg"

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
static enum app_cmd_action m_cmd_action; /* deferred action from app_cmd_handle */

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
 * (the immediate info-restore was dropped in #144 because it raced the phone
 * reading the reply). This flag is set whenever a response record is left on the
 * tag, and cleared whenever the info record is (re)written. The poll thread uses
 * it to restore the info record once the RF field has been quiet for a debounce
 * window (~10 s = no GPO events → phone gone), so a later tap always finds valid
 * info instead of a stale response. */
static bool m_info_restore_pending;

/* Take (and clear) the deferred action from the last processed NFC command, so
 * the poll thread can run reboot/save AFTER the response was written/read. */
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
		for (int attempt = 0; attempt < ST25DV_I2C_RETRIES; attempt++) {
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
		for (int attempt = 0; attempt < ST25DV_I2C_RETRIES; attempt++) {
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
	for (int waited = 0; waited <= NFC_FIELD_OFF_WAIT_MS; waited += NFC_FIELD_POLL_MS) {
		uint8_t eh;
		if (read_reg(ST25DV_EH_CTRL_DYN, &eh, 1) != 0) {
			return true; /* can't tell -> proceed (fall back to I2C retry) */
		}
		if (!(eh & ST25DV_FIELD_ON)) {
			return true; /* field absent -> safe to access the EEPROM */
		}
		k_msleep(NFC_FIELD_POLL_MS);
	}
	LOG_DBG("NFC: RF field still on after %d ms, skipping poll cycle", NFC_FIELD_OFF_WAIT_MS);
	return false;
}

/* Build CC + NDEF Message TLV + a single short NFC-Forum-external-type record +
 * terminator into `out`. Returns the byte length, or 0 if it would not fit
 * (short record, so payload <= 255 B). Shared by the info and command-response
 * writers. */
static size_t build_ndef_record(uint8_t *out, size_t out_size, const char *type,
				const uint8_t *payload, size_t payload_len)
{
	size_t type_len = strlen(type);

	/* NDEF record: header + type_len + payload_len + type + payload */
	size_t msg_len = 1 + 1 + 1 + type_len + payload_len;

	/* Tag content: CC (4) + NDEF Message TLV (0x03, len) + message + Terminator (0xFE) */
	size_t total = 4 + 2 + msg_len + 1;
	if (payload_len > 0xFF || msg_len > 0xFE || total > out_size) {
		return 0;
	}

	size_t i = 0;
	out[i++] = ST25DV_CC0; /* Type 5 Capability Container */
	out[i++] = ST25DV_CC1;
	out[i++] = ST25DV_CC2;
	out[i++] = ST25DV_CC3;
	out[i++] = 0x03;             /* NDEF Message TLV type */
	out[i++] = (uint8_t)msg_len; /* TLV length (single-byte form) */
	out[i++] = 0xD4;             /* MB | ME | SR | TNF=NFC Forum external(0x04) */
	out[i++] = (uint8_t)type_len;
	out[i++] = (uint8_t)payload_len;
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

static int decrypt(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size, size_t *out_len)
{
	int res = 0;

	if (in_len < 8) {
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

	psa_status_t status;
	psa_status_t destroy_status;

	status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		LOG_ERR_CALL_FAILED_INT("psa_crypto_init", status);
		return -EIO;
	}

	psa_key_attributes_t key_attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&key_attributes, PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&key_attributes, PSA_ALG_CCM);
	psa_set_key_type(&key_attributes, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&key_attributes, PSA_BYTES_TO_BITS(sizeof(g_app_config.secret_key)));

	psa_key_id_t key_id;
	status = psa_import_key(&key_attributes, g_app_config.secret_key,
				sizeof(g_app_config.secret_key), &key_id);
	if (status != PSA_SUCCESS) {
		LOG_ERR_CALL_FAILED_INT("psa_import_key", status);
		return -EIO;
	}

	psa_reset_key_attributes(&key_attributes);

	uint8_t nonce[NFC_NONCE_LEN];
	memcpy(nonce, in, 8);
	nonce[8] = NFC_NONCE_DIR_REQUEST;

	status = psa_aead_decrypt(key_id, PSA_ALG_CCM, nonce, sizeof(nonce), /* AAD */ in, 8,
				  &in[8], in_len - 8, out, out_size, out_len);

	destroy_status = psa_destroy_key(key_id);

	if (status != PSA_SUCCESS) {
		LOG_ERR_CALL_FAILED_INT("psa_aead_decrypt", status);
		res = -EIO;
	}

	if (destroy_status != PSA_SUCCESS) {
		LOG_ERR_CALL_FAILED_INT("psa_destroy_key", destroy_status);
		res = -EIO;
	}

	if (!res) {
		/* Advance and persist the anti-replay counter NOW, before the caller runs
		 * the command — otherwise a command that reboots (e.g. lrw_reset) would
		 * lose the bump and stay replayable. Persist a single NVS key, not the
		 * whole settings blob. If the persist fails, roll the counter back and
		 * reject so we never accept a counter we couldn't make durable. */
		uint32_t prev = app_config()->nonce_counter;
		app_config()->nonce_counter = nonce_counter;
		int sret = app_config_save_nonce_counter();
		if (sret) {
			LOG_ERR_CALL_FAILED_INT("app_config_save_nonce_counter", sret);
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

	/* 8 B header + ciphertext (== plaintext) + 16 B CCM tag. */
	if (out_size < 8 + in_len + 16) {
		LOG_ERR("Buffer too short for encryption: need %zu, have %zu", 8 + in_len + 16,
			out_size);
		return -ENOMEM;
	}

	sys_put_be32(g_app_config.serial_number, &out[0]);
	sys_put_be32(nonce_counter, &out[4]);

	psa_status_t status;
	psa_status_t destroy_status;

	status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		LOG_ERR_CALL_FAILED_INT("psa_crypto_init", status);
		return -EIO;
	}

	psa_key_attributes_t key_attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&key_attributes, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&key_attributes, PSA_ALG_CCM);
	psa_set_key_type(&key_attributes, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&key_attributes, PSA_BYTES_TO_BITS(sizeof(g_app_config.secret_key)));

	psa_key_id_t key_id;
	status = psa_import_key(&key_attributes, g_app_config.secret_key,
				sizeof(g_app_config.secret_key), &key_id);
	if (status != PSA_SUCCESS) {
		LOG_ERR_CALL_FAILED_INT("psa_import_key", status);
		return -EIO;
	}

	psa_reset_key_attributes(&key_attributes);

	uint8_t nonce[NFC_NONCE_LEN];
	memcpy(nonce, out, 8);
	nonce[8] = NFC_NONCE_DIR_RESPONSE;

	size_t ct_len = 0;
	status = psa_aead_encrypt(key_id, PSA_ALG_CCM, nonce, sizeof(nonce), /* AAD */ out, 8, in,
				  in_len, &out[8], out_size - 8, &ct_len);

	destroy_status = psa_destroy_key(key_id);

	if (status != PSA_SUCCESS) {
		LOG_ERR_CALL_FAILED_INT("psa_aead_encrypt", status);
		res = -EIO;
	}

	if (destroy_status != PSA_SUCCESS) {
		LOG_ERR_CALL_FAILED_INT("psa_destroy_key", destroy_status);
		res = -EIO;
	}

	if (!res) {
		*out_len = 8 + ct_len;
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

/* Process one encrypted command frame (shared by the NDEF and mailbox channels).
 * Three-way on the nonce counter vs the stored high-water:
 *   counter == cached  -> retransmission: replay the cached response, do NOT re-run
 *   counter  > stored  -> new: decrypt (advances+persists the counter), run, cache
 *   counter <= stored  -> stale replay: decrypt() rejects with -EACCES
 * Writes the encrypted reply into out_buf/out_len and the deferred action into
 * *action (NONE on a replay — the original already ran). Returns 0 or -errno. */
static int handle_encrypted_cmd(const uint8_t *in, size_t in_len, uint8_t *out_buf, size_t out_cap,
				size_t *out_len, enum app_cmd_action *action)
{
	*out_len = 0;
	*action = APP_CMD_ACTION_NONE;

	if (in_len < 8) {
		return -EINVAL;
	}

	uint32_t serial = sys_get_be32(&in[0]);
	uint32_t counter = sys_get_be32(&in[4]);

	if (serial == g_app_config.serial_number && m_resp_cache_len > 0 &&
	    counter == m_resp_cache_counter) {
		if (m_resp_cache_len > out_cap) {
			return -ENOMEM;
		}
		memcpy(out_buf, m_resp_cache_buf, m_resp_cache_len);
		*out_len = m_resp_cache_len;
		NFC_REPORT("  -> retransmission (counter %u): replaying cached response", counter);
		return 0;
	}

	static uint8_t cmd_plain[512];
	static uint8_t resp_plain[512];
	size_t cmd_len = 0;
	int ret = decrypt(in, in_len, cmd_plain, sizeof(cmd_plain), &cmd_len);
	if (ret) {
		return ret;
	}
	uint32_t req_nonce = app_config()->nonce_counter;

	size_t resp_len = 0;
	ret = app_cmd_handle(APP_CMD_TRANSPORT_NFC, cmd_plain, cmd_len, resp_plain,
			     sizeof(resp_plain), &resp_len, action);
	if (ret) {
		return ret;
	}
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

static int parser_callback(const struct app_ndef_parser_record_info *record_info, void *user_data)
{
	int ret;

	enum app_nfc_action *action = (enum app_nfc_action *)user_data;

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
		NFC_REPORT("NFC read: command record (%u B)", record_info->payload_len);
		NFC_REPORT_HEX("  command (protobuf):", record_info->payload,
			       record_info->payload_len);

		enum app_cmd_action cmd_action = APP_CMD_ACTION_NONE;
#ifdef CONFIG_APP_NFC_ENCRYPTION
		/* Encrypted channel: decrypt, run, encrypt the reply (RESPONSE-direction
		 * nonce so it never reuses the request nonce), with same-counter
		 * retransmission served from the response cache. */
		ret = handle_encrypted_cmd(record_info->payload, record_info->payload_len,
					   m_resp_buf, sizeof(m_resp_buf), &m_resp_len,
					   &cmd_action);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("handle_encrypted_cmd", ret);
			NFC_REPORT("  -> command rejected: %d", ret);
			m_resp_len = 0;
			return ret;
		}
#else
		/* Plaintext channel (validation build): hand the raw protobuf Command
		 * to app_cmd and stage the plaintext response. */
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
		m_cmd_action = cmd_action;
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

	size_t expected_type_len = strlen(NDEF_SUPPORTED_TYPE);

	/* Check if type matches */
	if (record_info->type_len != expected_type_len ||
	    strncmp((const char *)record_info->type, NDEF_SUPPORTED_TYPE, expected_type_len) != 0) {
		return 0;
	}

	LOG_INF("Found supported MIME record - length: %u byte(s)", record_info->payload_len);

	const uint8_t *cfg_data;
	size_t cfg_len;
#ifdef CONFIG_APP_NFC_ENCRYPTION
	NFC_REPORT("NFC read: config record (%u B encrypted)", record_info->payload_len);
	static uint8_t buf[512];
	size_t len;
	ret = decrypt(record_info->payload, record_info->payload_len, buf, sizeof(buf), &len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("decrypt", ret);
		NFC_REPORT("  -> decrypt failed: %d (rejected, tag left untouched)", ret);
		return ret;
	}
	cfg_data = buf;
	cfg_len = len;
#else
	/* Plaintext channel (validation build): the record is a bare AppConfigMessage
	 * protobuf — no secret key, serial check or nonce anti-replay. */
	NFC_REPORT("NFC read: config record (%u B plaintext)", record_info->payload_len);
	cfg_data = record_info->payload;
	cfg_len = record_info->payload_len;
#endif /* CONFIG_APP_NFC_ENCRYPTION */

	pb_istream_t stream = pb_istream_from_buffer(cfg_data, cfg_len);
	AppConfigMessage message = AppConfigMessage_init_zero;
	if (!pb_decode(&stream, AppConfigMessage_fields, &message)) {

		LOG_ERR_CALL_FAILED_STR("pb_decode", PB_GET_ERROR(&stream));
		return -EIO;
	}

	if (app_config_ingest(&message)) {
		*action = APP_NFC_ACTION_RESET;
		NFC_REPORT("  -> config decoded & ingested, action: factory reset");
	} else {
		*action = APP_NFC_ACTION_SAVE;
		NFC_REPORT("  -> config decoded & ingested, action: apply + save");
	}

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

	return 0;
}

/* Core NFC check: read the tag, parse/ingest a pending config, and restore the
 * info record. Caller must hold the access lock (nfc_access_begin). */
static int nfc_check_locked(enum app_nfc_action *action)
{
	int ret;
	int res = 0;

	m_have_resp = false;
	m_seen_resp = false;

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
	ret = app_ndef_parser_run(m_buf, ST25DV_USER_MEM_SIZE, parser_callback, action);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_ndef_parser_run", ret);
		res = ret;
	}

	/* A command was processed: write the response back to the tag. Reuse m_buf
	 * as the output buffer (its parsed contents are no longer needed; the reply
	 * is already copied into m_resp_buf) to keep this off the thread stack. */
	if (m_have_resp) {
		m_unknown_count = 0;
		size_t out_len = build_ndef_record(m_buf, ST25DV_USER_MEM_SIZE, NDEF_RESPONSE_TYPE,
						   m_resp_buf, m_resp_len);
		LOG_INF("Writing command response to NFC (%zu B)...", m_resp_len);
		NFC_REPORT("NFC wrote: response record (%zu B NDEF, %zu B payload)", out_len,
			   m_resp_len);
		NFC_REPORT_HEX("  response (0x01 ver + protobuf Response):", m_resp_buf,
			       m_resp_len);
		if (out_len) {
			ret = write_mem(0, m_buf, out_len);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("write_mem", ret);
				res = ret;
			}
		}
		/* #164: a response record now sits on the tag — arm the info-restore so
		 * the poll thread rewrites the info record once the field goes quiet. */
		m_info_restore_pending = true;
		return res;
	}

	/* Our response is already on the tag (awaiting the phone read): leave it. */
	if (m_seen_resp) {
		m_unknown_count = 0;
		m_info_restore_pending = true; /* #164: still need to restore info later */
		return res;
	}

	/* A recognized config was consumed (ingest set *action): clear it right away
	 * for anti-replay, restoring the info record. */
	if (*action != APP_NFC_ACTION_NONE) {
		m_unknown_count = 0;
		m_info_restore_pending = false; /* #164: writing info record below */
		LOG_INF("Writing info record to NFC (consumed config)...");
		NFC_REPORT("NFC wrote: info record (%zu B) - cleared consumed config, restored "
			   "metadata",
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
int app_nfc_check(enum app_nfc_action *action)
{
	*action = APP_NFC_ACTION_NONE;

	int ret = nfc_access_begin();
	if (ret) {
		return ret;
	}

	int res = nfc_check_locked(action);

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
int app_nfc_poll(enum app_nfc_action *action)
{
	*action = APP_NFC_ACTION_NONE;

	int ret = nfc_access_begin();
	if (ret) {
		return ret;
	}

	int res = nfc_check_locked(action);

	nfc_access_end();
	return res;
}

/* #164: true while a response record is left on the tag and the info record has
 * not yet been restored. The poll thread uses this to shorten its wait and to
 * decide whether to restore the info record once the field goes quiet. */
bool app_nfc_info_restore_pending(void)
{
	return m_info_restore_pending;
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
		LOG_INF("NFC: restored info record after field loss (#164)");
	}

	nfc_access_end();
	return ret;
}

/* Write the whole mailbox in one I2C transaction (volatile RAM, no program
 * wait). Setting the first RAM byte arms HOST_PUT_MSG so the RF side can read
 * it. Caller holds the access lock. */
static int mb_write(const uint8_t *data, size_t len)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	if (len == 0 || len > ST25DV_MB_RAM_SIZE) {
		return -EINVAL;
	}

	uint8_t frame[2 + ST25DV_MB_RAM_SIZE];
	sys_put_be16(ST25DV_MB_RAM, frame);
	memcpy(&frame[2], data, len);

	return i2c_write(dev, frame, 2 + len, ST25DV_I2C_ADDR_E0);
}

/* Enable the mailbox: set the static MB_MODE allow-bit (needs the I2C password)
 * then the dynamic MB_EN. Caller holds the access lock. */
static int mb_enable(void)
{
	static const uint8_t default_pwd[8] = {0};
	int ret;

	ret = nfc_present_password(default_pwd);
	if (ret) {
		return ret;
	}
	k_msleep(15);

	uint8_t mode = 0;
	ret = read_reg(ST25DV_MB_MODE_REG, &mode, 1);
	if (ret) {
		return ret;
	}
	if (!(mode & ST25DV_MB_MODE_EN)) {
		mode |= ST25DV_MB_MODE_EN;
		ret = write_reg(ST25DV_MB_MODE_REG, &mode, 1);
		if (ret) {
			return ret;
		}
		k_msleep(ST25DV_TW_MS_PER_PAGE + 5);
	}

	uint8_t ctrl = ST25DV_MB_CTRL_MB_EN;
	ret = write_reg(ST25DV_MB_CTRL_DYN, &ctrl, 1);
	if (ret) {
		return ret;
	}
	k_msleep(1);
	return 0;
}

/* Block until the RF side has read the response we just wrote, i.e. until the
 * ST25DV clears HOST_PUT_MSG (it sets it on our mailbox write and clears it when
 * RF reads the last byte). This serialises the FTM exchange — without it a fast
 * streaming phone can write its next request before reading the current reply,
 * so it ends up reading a stale reply (or we overwrite an unread one), which
 * desynchronises the request/response pairing (the mailbox paged-read frame gap,
 * #194). Bounded by `timeout_ms` so a phone that left mid-stream can't wedge us. */
static void mb_wait_host_put_cleared(uint32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	while (k_uptime_get() < deadline) {
		uint8_t ctrl = 0;
		if (read_reg(ST25DV_MB_CTRL_DYN, &ctrl, 1) == 0 &&
		    !(ctrl & ST25DV_MB_CTRL_HOST_PUT)) {
			return; /* RF read our response */
		}
		k_msleep(3);
	}
}

/* Default mailbox idle window when the caller passes 0 (the deadline resets on
 * every served message). Short on purpose: active streaming (config read,
 * firmware update) keeps resetting it, so it's never cut off, but once the phone
 * goes quiet the device drops back to the NDEF poll within ~2 s — so an NDEF
 * write that follows a read (e.g. saving edited config) isn't blocked for long
 * by lingering Fast-Transfer mode. Measured in milliseconds. Must stay long
 * enough to cover the phone's hand-off from the NDEF EnterMailbox ack to its
 * first mailbox write (a couple of seconds). */
#define APP_NFC_MAILBOX_IDLE_MS 5000

/* Serve the mailbox (FTM) until `idle_timeout_s` of inactivity, holding the
 * ST25DV powered (LPD low) the whole time so the phone's RF field and our I2C
 * polling both reach it (the tag is dual-port in this mode). Each RF command
 * message is run through app_cmd_handle and the reply written back. The window
 * is an *idle* timeout — it resets on every served message, so a config stream
 * or firmware update keeps the device here as long as traffic flows, then falls
 * back to the low-power NDEF poll once the phone goes quiet. Returns the number
 * of messages served, or a negative errno if the mailbox could not be brought
 * up. Entered from the NFC poll thread on an EnterMailbox command (the device
 * acks that over NDEF first), or directly via the `nfc mailbox` shell command. */
int app_nfc_serve_mailbox(uint32_t idle_timeout_ms)
{
	if (idle_timeout_ms == 0) {
		idle_timeout_ms = APP_NFC_MAILBOX_IDLE_MS;
	}

	int ret = nfc_access_begin();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("nfc_access_begin", ret);
		return ret;
	}

	ret = mb_enable();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("mb_enable", ret);
		nfc_access_end();
		return ret;
	}

	LOG_INF("NFC mailbox: serving (idle timeout %u ms)", idle_timeout_ms);

	int64_t deadline = k_uptime_get() + idle_timeout_ms;
	uint32_t served = 0;

	while (k_uptime_get() < deadline) {
		uint8_t ctrl = 0;
		if (read_reg(ST25DV_MB_CTRL_DYN, &ctrl, 1) || !(ctrl & ST25DV_MB_CTRL_RF_PUT)) {
			k_msleep(15);
			continue;
		}

		uint8_t lenm1 = 0;
		if (read_reg(ST25DV_MB_LEN_DYN, &lenm1, 1)) {
			k_msleep(15);
			continue;
		}
		size_t cmd_len = (size_t)lenm1 + 1;

		if (read_mem(ST25DV_MB_RAM, m_buf, cmd_len)) {
			k_msleep(15);
			continue;
		}

		enum app_cmd_action action = APP_CMD_ACTION_NONE;
		m_resp_len = 0;
#ifdef CONFIG_APP_NFC_ENCRYPTION
		/* Mailbox frames are AES-CCM encrypted exactly like the NDEF channel
		 * (#194): same direction nonce + AAD + anti-replay, and same-counter
		 * retransmissions are served from the response cache. */
		ret = handle_encrypted_cmd(m_buf, cmd_len, m_resp_buf, sizeof(m_resp_buf),
					   &m_resp_len, &action);
#else
		/* Plaintext channel (validation build only). */
		ret = app_cmd_handle(APP_CMD_TRANSPORT_NFC, m_buf, cmd_len, m_resp_buf,
				     sizeof(m_resp_buf), &m_resp_len, &action);
#endif
		if (ret || m_resp_len == 0) {
			NFC_REPORT("mailbox: handle ret=%d resp=%zu", ret, m_resp_len);
			k_msleep(15);
			continue;
		}

		if (mb_write(m_resp_buf, m_resp_len)) {
			k_msleep(15);
			continue;
		}
		served++;
		deadline = k_uptime_get() + idle_timeout_ms;
		LOG_INF("NFC mailbox: served %zu B (#%u)", m_resp_len, served);

		/* Serialise the exchange: wait until the phone has read this reply before
		 * accepting the next request (or tearing down on a terminal action), so a
		 * fast streamer can't read a stale reply or have an unread one overwritten
		 * — the mailbox paged-read frame gap (#194). */
		mb_wait_host_put_cleared(200);

		/* The phone signalled end of stream (ExitMailbox): its ack has been read
		 * (waited for above), so leave mailbox mode immediately. */
		if (action == APP_CMD_ACTION_LEAVE_MAILBOX) {
			LOG_INF("NFC mailbox: ExitMailbox -> leaving (served %u)", served);
			break;
		}

		/* Any other deferred action (settings save / reboot / factory reset /
		 * alarm-rules save): hand it to the poll thread the same way the NDEF path
		 * does, then leave mailbox mode so the action can run (ack already read). */
		if (action != APP_CMD_ACTION_NONE) {
			m_cmd_action = action;
			break;
		}
	}

	uint8_t off = 0;
	write_reg(ST25DV_MB_CTRL_DYN, &off, 1);
	nfc_access_end();
	LOG_INF("NFC mailbox: off, served %u message(s)", served);
	return (int)served;
}

#if defined(CONFIG_SHELL)

/* Shell wrapper around app_nfc_serve_mailbox(): serve with the given idle
 * timeout in seconds (0 = firmware default). */
static int cmd_nfc_mailbox(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t seconds = (argc >= 2) ? (uint32_t)strtoul(argv[1], NULL, 0) : 30;
	if (seconds > 300) {
		shell_error(sh, "seconds must be 0..300");
		return -EINVAL;
	}
	int ret = app_nfc_serve_mailbox(seconds * 1000);
	if (ret < 0) {
		shell_error(sh, "mailbox failed: %d", ret);
		return ret;
	}
	shell_print(sh, "mailbox done, served %d message(s)", ret);
	return 0;
}

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

	enum app_nfc_action action;

	/* Route the check's read/decode/react/write trace to this shell. */
	m_report_sh = sh;
	int ret = app_nfc_check(&action);
	m_report_sh = NULL;
	if (ret) {
		shell_error(sh, "nfc check failed: %d", ret);
		return ret;
	}

	if (action == APP_NFC_ACTION_SAVE) {
		ret = app_settings_save(true);
		shell_print(sh, "config applied%s", ret ? " (save failed!)" : " and saved");
	} else if (action == APP_NFC_ACTION_RESET) {
		ret = app_settings_reset();
		shell_print(sh, "factory reset%s", ret ? " (failed!)" : "");
	} else {
		shell_print(sh, "no config on tag (no action)");
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
	SHELL_CMD_ARG(mailbox, NULL, "Serve the mailbox (FTM) for N s. Usage: mailbox [seconds]",
		      cmd_nfc_mailbox, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(nfc, &sub_nfc, "ST25DV NFC memory access (debug).", NULL);

#endif /* CONFIG_SHELL */
