/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "app_ndef_parser.h"

/* Nanopb includes */
#include <pb_decode.h>
#include "src/nfc_config.pb.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
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
		LOG_ERR("Serial number does not match: %u != %u", serial_number, g_app_config.serial_number);
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
	status = psa_import_key(&key_attributes, g_app_config.secret_key, sizeof(g_app_config.secret_key), &key_id);
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

	return res;
}

static void ingest_message(const NfcConfigMessage *message)
{
	if (message->has_lorawan) {
		if (message->lorawan.has_region) {
			LOG_INF("Parameter `lorawan.region`: %d", message->lorawan.region);
		}

		if (message->lorawan.has_deveui) {
			LOG_INF("Parameter `lorawan.deveui`: %s", message->lorawan.deveui);
		}
	}
}

static int parser_callback(const struct app_ndef_parser_record_info *record_info, void *user_data)
{
	int ret;

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

	uint8_t buf[448];
	size_t len;
	ret = decrypt(record_info->payload, record_info->payload_len, buf, sizeof(buf), &len);
	if (ret) {
		LOG_ERR("Call `decrypt` failed: %d", ret);
		return ret;
	}

	pb_istream_t stream = pb_istream_from_buffer(buf, len);
	NfcConfigMessage message = NfcConfigMessage_init_zero;
	if (!pb_decode(&stream, NfcConfigMessage_fields, &message)) {
		LOG_ERR("Failed to decode Protobuf message: %s", PB_GET_ERROR(&stream));
		return -EIO;
	}

	ingest_message(&message);

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

static int init(void)
{
	int ret;
	int res = 0;

	uint8_t buf[512];
	ret = read_mem(0, buf, sizeof(buf));
	if (ret) {
		LOG_ERR("Call `read_mem` failed: %d", ret);
		return ret;
	}

	if (is_buffer_zero(buf, sizeof(buf))) {
		return 0;
	}

	ret = app_ndef_parser_run(buf, sizeof(buf), parser_callback, NULL);
	if (ret) {
		LOG_ERR("Call `app_ndef_parser_run` failed: %d", ret);
		res = ret;
	}

	LOG_INF("Clearing memory...");

	memset(buf, 0, sizeof(buf));

	/* TODO Implement */

	return res;
}

SYS_INIT(init, APPLICATION, 99);
