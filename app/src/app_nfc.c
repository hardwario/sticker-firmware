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
#define ST25DV_IT_STS_DYN 0x2005

#define NDEF_TNF_MIME       0x02
#define NDEF_SUPPORTED_TYPE "application/vnd.hardwario.sticker-config.v1"

/* Plaintext info record the firmware keeps on the tag so a phone learns the
 * sticker identity and schema version the moment it taps, without decrypting
 * anything. Payload layout (11 bytes):
 *   [0]    format version (NDEF_INFO_FORMAT)
 *   [1..4] serial number (big-endian uint32)
 *   [5]    fw major   [6] fw minor   [7] fw patch
 *   [8]    build type (0=main, 1=dev, 2=custom)
 *   [9]    config/schema version (APP_CONFIG_VERSION)
 *   [10]   flags: bit0 = debug build
 */
#define NDEF_INFO_TYPE   "application/vnd.hardwario.sticker-info.v1"
#define NDEF_INFO_FORMAT 0x01

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

/* Periodic NFC check enable (toggled via `nfc autocheck`). Lets a config blob
 * be written over several `nfc write` calls without the periodic check racing
 * it and rewriting the tag to the info record mid-write. */
static bool m_periodic = true;

bool app_nfc_periodic_enabled(void)
{
	return m_periodic;
}

static int read_mem(uint16_t reg, void *buf, size_t len)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	uint8_t reg_[2];
	sys_put_be16(reg, reg_);

	ret = i2c_write_read(dev, ST25DV_I2C_ADDR_E0, reg_, sizeof(reg_), buf, len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("i2c_write_read", ret);
		return ret;
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
		size_t within_256 =
			ST25DV_MAX_SEQ_WRITE_BYTES - (reg & (ST25DV_MAX_SEQ_WRITE_BYTES - 1));

		size_t chunk = MIN(remaining, within_256);

		if (chunk > ST25DV_MAX_SEQ_WRITE_BYTES) {
			chunk = ST25DV_MAX_SEQ_WRITE_BYTES;
		}

		uint8_t frame[2 + ST25DV_MAX_SEQ_WRITE_BYTES];
		sys_put_be16(reg, frame);
		memcpy(&frame[2], p, chunk);

		ret = i2c_write(dev, frame, 2 + chunk, ST25DV_I2C_ADDR_E0);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("i2c_write", ret);
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

	int ret = i2c_write_read(dev, reg_dev_addr(reg), reg_, sizeof(reg_), buf, len);
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

	int ret = i2c_write(dev, frame, 2 + len, reg_dev_addr(reg));
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("i2c_write", ret);
		return ret;
	}

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

/* Build the plaintext info NDEF (TLV + single MIME record + terminator) into
 * `out`. Returns the byte length, or 0 if it would not fit. Deterministic for a
 * given firmware/config (no uptime/clock), so app_nfc_check() can compare it to
 * the tag content and skip rewriting when already present. */
static size_t build_info_ndef(uint8_t *out, size_t out_size)
{
	struct app_cmd_info info;
	app_cmd_get_info(&info);

	uint8_t payload[11];
	payload[0] = NDEF_INFO_FORMAT;
	sys_put_be32(info.serial_number, &payload[1]);
	payload[5] = info.fw_major;
	payload[6] = info.fw_minor;
	payload[7] = info.fw_patch;
	payload[8] = info.build_type;
	payload[9] = (uint8_t)g_app_config.config_version;
	payload[10] = info.debug ? 0x01 : 0x00;

	size_t type_len = strlen(NDEF_INFO_TYPE);
	size_t payload_len = sizeof(payload);

	/* NDEF record: header + type_len + payload_len + type + payload */
	size_t msg_len = 1 + 1 + 1 + type_len + payload_len;

	/* Tag content: CC (4) + NDEF Message TLV (0x03, len) + message + Terminator (0xFE) */
	size_t total = 4 + 2 + msg_len + 1;
	if (total > out_size || msg_len > 0xFE) {
		return 0;
	}

	size_t i = 0;
	out[i++] = ST25DV_CC0; /* Type 5 Capability Container */
	out[i++] = ST25DV_CC1;
	out[i++] = ST25DV_CC2;
	out[i++] = ST25DV_CC3;
	out[i++] = 0x03;             /* NDEF Message TLV type */
	out[i++] = (uint8_t)msg_len; /* TLV length (single-byte form) */
	out[i++] = 0xD2;             /* MB | ME | SR | TNF=MIME(0x02) */
	out[i++] = (uint8_t)type_len;
	out[i++] = (uint8_t)payload_len;
	memcpy(&out[i], NDEF_INFO_TYPE, type_len);
	i += type_len;
	memcpy(&out[i], payload, payload_len);
	i += payload_len;
	out[i++] = 0xFE; /* Terminator TLV */

	return i;
}

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

	if (g_app_config.serial_number != serial_number) {
		LOG_ERR("Serial number does not match: %u != %u", serial_number,
			g_app_config.serial_number);
		return -EACCES;
	}

	/* Verify nonce counter (part of nonce) */
	uint32_t nonce_counter = sys_get_be32(&in[4]);
	LOG_INF("Nonce counter: %u", nonce_counter);

	if (g_app_config.nonce_counter >= nonce_counter) {
		LOG_ERR("Nonce counter is not greater than the last used nonce: %u >= %u",
			g_app_config.nonce_counter, nonce_counter);
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

	status = psa_aead_decrypt(key_id, PSA_ALG_CCM, &in[0], 8, NULL, 0, &in[8], in_len - 8, out,
				  out_size, out_len);

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
		app_config()->nonce_counter = nonce_counter;
	}

	return res;
}

static int parser_callback(const struct app_ndef_parser_record_info *record_info, void *user_data)
{
	int ret;

	enum app_nfc_action *action = (enum app_nfc_action *)user_data;

	/* Check if TNF type is MIME */
	if (record_info->tnf != NDEF_TNF_MIME) {
		return 0;
	}

	size_t expected_type_len = strlen(NDEF_SUPPORTED_TYPE);

	/* Check if type matches */
	if (record_info->type_len != expected_type_len ||
	    strncmp((const char *)record_info->type, NDEF_SUPPORTED_TYPE, expected_type_len) != 0) {
		return 0;
	}

	LOG_INF("Found supported MIME record - length: %u byte(s)", record_info->payload_len);

	static uint8_t buf[448];
	size_t len;
	ret = decrypt(record_info->payload, record_info->payload_len, buf, sizeof(buf), &len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("decrypt", ret);
		return ret;
	}

	pb_istream_t stream = pb_istream_from_buffer(buf, len);
	AppConfigMessage message = AppConfigMessage_init_zero;
	if (!pb_decode(&stream, AppConfigMessage_fields, &message)) {

		LOG_ERR_CALL_FAILED_STR("pb_decode", PB_GET_ERROR(&stream));
		return -EIO;
	}

	if (app_config_ingest(&message)) {
		*action = APP_NFC_ACTION_RESET;
	} else {
		*action = APP_NFC_ACTION_SAVE;
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

int app_nfc_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&m_lpd)) {
		LOG_ERR("GPIO device not ready (LPD)");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&m_lpd, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	return 0;
}

/* Core NFC check: read the tag, parse/ingest a pending config, and restore the
 * info record. Caller must hold the access lock (nfc_access_begin). */
static int nfc_check_locked(enum app_nfc_action *action)
{
	int ret;
	int res = 0;

	/* Build the expected info record up front (no I2C); used both to detect
	 * "tag already holds our info" and to (re)write it. */
	uint8_t info[80];
	size_t info_len = build_info_ndef(info, sizeof(info));

	ret = read_mem(0, m_buf, ST25DV_USER_MEM_SIZE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("read_mem", ret);
		return ret;
	}

	/* Empty tag: lay down the info record so a phone always finds metadata. */
	if (is_buffer_zero(m_buf, ST25DV_USER_MEM_SIZE)) {
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
		return 0;
	}

	/* Pending data written by a phone: parse it (config ingest sets *action). */
	ret = app_ndef_parser_run(m_buf, ST25DV_USER_MEM_SIZE, parser_callback, action);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_ndef_parser_run", ret);
		res = ret;
	}

	/* Replace the tag content with the info record. This clears the consumed
	 * config (anti-replay) and restores the metadata a phone expects to read. */
	LOG_INF("Writing info record to NFC...");
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

/* Gated poll for the periodic main-loop check: read the 1-byte IT_STS_Dyn
 * (read-clears). Only when it shows RF activity since the last poll do the full
 * 512 B read + parse. Saves waking the bus / parsing when nothing changed. */
int app_nfc_poll(enum app_nfc_action *action)
{
	*action = APP_NFC_ACTION_NONE;

	int ret = nfc_access_begin();
	if (ret) {
		return ret;
	}

	int res = 0;
	uint8_t it_sts = 0;
	ret = read_reg(ST25DV_IT_STS_DYN, &it_sts, sizeof(it_sts));
	if (ret) {
		res = ret;
	} else if (it_sts != 0) {
		res = nfc_check_locked(action);
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
	int ret = app_nfc_check(&action);
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
	SHELL_CMD_ARG(write, NULL, "Write hex bytes. Usage: write <offset> <hexbytes>", cmd_nfc_write,
		      3, 0),
	SHELL_CMD_ARG(clear, NULL, "Zero all 512 B of NFC memory.", cmd_nfc_clear, 1, 0),
	SHELL_CMD_ARG(autocheck, NULL, "Enable/disable periodic check. Usage: autocheck on|off",
		      cmd_nfc_autocheck, 2, 0),
	SHELL_CMD_ARG(check, NULL, "Run the NFC check now (parse + apply config).", cmd_nfc_check, 1,
		      0),
	SHELL_CMD_ARG(reg, NULL, "Read system/dynamic register (E1). Usage: reg <addr> [count]",
		      cmd_nfc_reg, 2, 1),
	SHELL_CMD_ARG(regw, NULL, "Write system/dynamic register (E1). Usage: regw <addr> <hex>",
		      cmd_nfc_regw, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(nfc, &sub_nfc, "ST25DV NFC memory access (debug).", NULL);

#endif /* CONFIG_SHELL */
