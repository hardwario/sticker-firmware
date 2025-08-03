/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_ndef_parser.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_ndef_parser, LOG_LEVEL_DBG);

#define NDEF_TLV_TYPE_NDEF_MSG   0x03
#define NDEF_TLV_TYPE_TERMINATOR 0xfe

#define NDEF_RECORD_HEADER_TNF_MASK 0x07
#define NDEF_RECORD_HEADER_IL_FLAG  0x08
#define NDEF_RECORD_HEADER_SR_FLAG  0x10
#define NDEF_RECORD_HEADER_CF_FLAG  0x20
#define NDEF_RECORD_HEADER_ME_FLAG  0x40
#define NDEF_RECORD_HEADER_MB_FLAG  0x80

static bool advance_buffer(const uint8_t **buffer, size_t *remaining_len, size_t step)
{
	if (*remaining_len < step) {
		return false;
	}

	*buffer += step;
	*remaining_len -= step;

	return true;
}

int app_ndef_parser_run(const uint8_t *buffer, size_t buffer_len,
			app_ndef_parser_callback_t callback, void *user_data)
{
	int ret;

	const uint8_t *ndef_msg_start = NULL;
	size_t ndef_msg_len = 0;
	size_t remaining_len = buffer_len;

	/* Find the NDEF Message TLV (Type-Length-Value) block */
	while (remaining_len > 0) {
		uint8_t type = *buffer;

		if (!advance_buffer(&buffer, &remaining_len, 1)) {
			return -ENODATA;
		}

		if (type == NDEF_TLV_TYPE_NDEF_MSG) {
			if (remaining_len == 0) {
				LOG_ERR("Found NDEF message type but no length");
				return -EMSGSIZE;
			}

			uint8_t length = *buffer;

			if (!advance_buffer(&buffer, &remaining_len, 1)) {
				return -ENODATA;
			}

			if (length == 0xff) {
				/* Three-byte format: 0xff followed by 2-byte length */
				if (remaining_len < 2) {
					LOG_ERR("Invalid 3-byte TLV length");
					return -EMSGSIZE;
				}

				/* Safely read length after boundary check */
				ndef_msg_len = sys_get_be16(buffer);

				advance_buffer(&buffer, &remaining_len, 2);
			} else {
				/* Single-byte format */
				ndef_msg_len = length;
			}

			if (remaining_len < ndef_msg_len) {
				LOG_ERR("NDEF message length (%zu) exceeds buffer size (%zu)",
					ndef_msg_len, remaining_len);
				return -EMSGSIZE;
			}

			ndef_msg_start = buffer;

			/* Found our message */
			break;
		}

		if (type == NDEF_TLV_TYPE_TERMINATOR) {
			LOG_INF("Terminator TLV found, no NDEF message");
			return 0;
		}
	}

	if (!ndef_msg_start) {
		LOG_WRN("No NDEF message found in the provided buffer");
		return -ENOMSG;
	}

	/* Iterate through records within the NDEF message */
	const uint8_t *record = ndef_msg_start;
	size_t records_len = ndef_msg_len;

	while (records_len > 0) {
		struct app_ndef_parser_record_info info = {0};

		uint8_t header = *record;

		if (!advance_buffer(&record, &records_len, 1)) {
			return -ENODATA;
		}

		info.tnf = header & NDEF_RECORD_HEADER_TNF_MASK;

		/* Get type length */
		if (records_len == 0) {
			return -EMSGSIZE;
		}

		info.type_len = *record;

		if (!advance_buffer(&record, &records_len, 1)) {
			return -ENODATA;
		}

		/* Get payload length */
		bool short_record = (header & NDEF_RECORD_HEADER_SR_FLAG);
		if (short_record) {
			if (records_len == 0) {
				return -EMSGSIZE;
			}

			info.payload_len = *record;

			if (!advance_buffer(&record, &records_len, 1)) {
				return -ENODATA;
			}
		} else {
			/* Check boundary before reading the 4-byte length */
			if (records_len < 4) {
				LOG_ERR("Buffer too small for 4-byte payload length");
				return -EMSGSIZE;
			}

			/* Safely read length after boundary check */
			info.payload_len = sys_get_be32(record);

			advance_buffer(&record, &records_len, 4);
		}

		/* Get ID length if present (we will just skip it) */
		bool id_present = (header & NDEF_RECORD_HEADER_IL_FLAG);
		if (id_present) {
			if (records_len == 0) {
				return -EMSGSIZE;
			}

			uint8_t id_len = *record;

			if (!advance_buffer(&record, &records_len, 1 + id_len)) {
				LOG_WRN("Record ID length exceeds buffer");
				return -EMSGSIZE;
			}
		}

		/* Get type */
		info.type = record;

		if (!advance_buffer(&record, &records_len, info.type_len)) {
			LOG_ERR("Record type length exceeds buffer");
			return -EMSGSIZE;
		}

		/* Get payload */
		info.payload = record;

		if (!advance_buffer(&record, &records_len, info.payload_len)) {
			LOG_ERR("Record payload length exceeds buffer");
			return -EMSGSIZE;
		}

		/* Invoke callback for the found record */
		if (callback) {
			ret = callback(&info, user_data);
			if (ret) {
				LOG_INF("Callback requested to stop parsing");
				return ret;
			}
		}

		/* If this was the last record, stop */
		if (header & NDEF_RECORD_HEADER_ME_FLAG) {
			break;
		}
	}

	return 0;
}
