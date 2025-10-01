/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_nfc.h"
#include "app_nfc_ingest.h"
#include "app_config.h"
#include "app_ndef_parser.h"

/* Nanopb includes */
#include <pb_decode.h>
#include "src/nfc_config.pb.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

/* PSA Crypto includes */
#include <psa/crypto.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_nfc, LOG_LEVEL_DBG);

#define ST25DV_I2C_ADDR_E0 0x53
#define ST25DV_I2C_ADDR_E1 0x55

#define ST25DV_MAX_SEQ_WRITE_BYTES 256
#define ST25DV_INT_PAGE_BYTES      4
#define ST25DV_TW_MS_PER_PAGE      5

#define NDEF_TNF_MIME       0x02
#define NDEF_SUPPORTED_TYPE "application/vnd.hardwario.sticker-config.v1"

typedef void (*ndef_text_callback_t)(const char *text, size_t len);

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
		LOG_ERR("Call `i2c_write_read` failed: %d", ret);
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
			LOG_ERR("Call `i2c_write` failed: %d", ret);
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
			nonce_counter, g_app_config.nonce_counter);
		return -EACCES;
	}

	psa_status_t status;
	psa_status_t destroy_status;

	status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		LOG_ERR("Call `psa_crypto_init` failed: %d", status);
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
		LOG_ERR("Call `psa_import_key` failed: %d", status);
		return -EIO;
	}

	psa_reset_key_attributes(&key_attributes);

	status = psa_aead_decrypt(key_id, PSA_ALG_CCM, &in[0], 8, NULL, 0, &in[8], in_len - 8, out,
				  out_size, out_len);

	destroy_status = psa_destroy_key(key_id);

	if (status != PSA_SUCCESS) {
		LOG_ERR("Call `psa_aead_decrypt` failed: %d", status);
		res = -EIO;
	}

	if (destroy_status != PSA_SUCCESS) {
		LOG_ERR("Call `psa_destroy_key` failed: %d", destroy_status);
		res = -EIO;
	}

	app_config()->nonce_counter = nonce_counter;

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
		LOG_ERR("Call `decrypt` failed: %d", ret);
		return ret;
	}

	pb_istream_t stream = pb_istream_from_buffer(buf, len);
	NfcConfigMessage message = NfcConfigMessage_init_zero;
	if (!pb_decode(&stream, NfcConfigMessage_fields, &message)) {
		LOG_ERR("Call `pb_decode` failed: %s", PB_GET_ERROR(&stream));
		return -EIO;
	}

	app_nfc_ingest(&message);

	*action = APP_NFC_ACTION_SAVE;

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

int app_nfc_check(enum app_nfc_action *action)
{
	int ret;
	int res = 0;

	const struct gpio_dt_spec lpd_spec = GPIO_DT_SPEC_GET(DT_NODELABEL(lpd), gpios);

	if (!gpio_is_ready_dt(&lpd_spec)) {
		LOG_ERR("GPIO device not ready (LPD)");
		return -ENODEV;
	}

	ret = gpio_pin_set_dt(&lpd_spec, 0);
	if (ret) {
		LOG_ERR("Call `gpio_pin_set_dt` failed: %d", ret);
		return ret;
	}

	k_sleep(K_MSEC(150));

	static uint8_t buf[512];
	ret = read_mem(0, buf, sizeof(buf));
	if (ret) {
		LOG_ERR("Call `read_mem` failed: %d", ret);
		res = ret;

		ret = gpio_pin_set_dt(&lpd_spec, 1);
		if (ret) {
			LOG_ERR("Call `gpio_pin_set_dt` failed: %d", ret);
			return ret;
		}

		return res;
	}

	if (is_buffer_zero(buf, sizeof(buf))) {
		ret = gpio_pin_set_dt(&lpd_spec, 1);
		if (ret) {
			LOG_ERR("Call `gpio_pin_set_dt` failed: %d", ret);
			return ret;
		}

		return 0;
	}

	*action = APP_NFC_ACTION_NONE;
	ret = app_ndef_parser_run(buf, sizeof(buf), parser_callback, action);
	if (ret) {
		LOG_ERR("Call `app_ndef_parser_run` failed: %d", ret);
		res = ret;
	}

	LOG_INF("Clearing memory...");

	memset(buf, 0, sizeof(buf));

	ret = write_mem(0, buf, sizeof(buf));
	if (ret) {
		LOG_ERR("Call `write_mem` failed: %d", ret);
		res = ret;
	}

	ret = gpio_pin_set_dt(&lpd_spec, 1);
	if (ret) {
		LOG_ERR("Call `gpio_pin_set_dt` failed: %d", ret);
		res = ret;
	}

	return res;
}

static int init(void)
{
	int ret;

	const struct gpio_dt_spec lpd_spec = GPIO_DT_SPEC_GET(DT_NODELABEL(lpd), gpios);

	if (!gpio_is_ready_dt(&lpd_spec)) {
		LOG_ERR("GPIO device not ready (LPD)");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&lpd_spec, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		LOG_ERR("Call `gpio_pin_configure_dt` failed: %d", ret);
		return ret;
	}

	return 0;
}

SYS_INIT(init, POST_KERNEL, 99);
