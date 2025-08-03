/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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
	LOG_INF("TNF: %u", record_info->tnf);

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

	LOG_INF("Found supported MIME record; Length: %u byte(s)", record_info->payload_len);

	NfcConfigMessage message = NfcConfigMessage_init_zero;

	pb_istream_t stream =
		pb_istream_from_buffer(record_info->payload, record_info->payload_len);

	bool status = pb_decode(&stream, NfcConfigMessage_fields, &message);
	if (!status) {
		LOG_ERR("Failed to decode Protobuf message: %s", PB_GET_ERROR(&stream));
		return -EIO;
	}

	ingest_message(&message);

	return 0;
}

static int init(void)
{
	int ret;

	uint8_t buf[512];
	ret = read_mem(0, buf, sizeof(buf));
	if (ret) {
		LOG_ERR("Call `read_mem` failed: %d", ret);
		return ret;
	}

	/*
	 * e1 40 40 01 03 99 d2 2b 6b 61 70 70 6c 69 63 61
	 * 74 69 6f 6e 2f 76 6e 64 2e 68 61 72 64 77 61 72
	 * 69 6f 2e 73 74 69 63 6b 65 72 2d 63 6f 6e 66 69
	 * 67 2e 76 31 08 01 10 00 1a 2c 08 00 10 01 18 00
	 * 20 01 2a 10 37 32 66 64 36 31 31 63 30 65 35 34
	 * 64 32 34 34 32 10 38 61 34 37 63 39 65 36 38 62
	 * 63 65 62 66 32 61 22 37 08 00 10 1e 18 ac 02 20
	 * 84 07 2d 00 00 70 41 35 00 00 c8 41 3d 00 00 00
	 * 3f 45 00 00 70 41 4d 00 00 c8 41 55 00 00 00 3f
	 * 5d 00 00 70 41 65 00 00 c8 41 6d 00 00 00 3f 00
	 * fe
	 */

	/* LOG_HEXDUMP_DBG(buf, sizeof(buf), "Memory:"); */

	ret = app_ndef_parser_run(buf, sizeof(buf), parser_callback, NULL);
	if (ret) {
		LOG_ERR("Call `app_ndef_parser_run` failed: %d", ret);
		return ret;
	}

	return 0;
}

SYS_INIT(init, APPLICATION, 99);
