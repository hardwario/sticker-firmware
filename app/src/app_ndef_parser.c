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

#define NDEF_TLV_TYPE_NULL       0x00
#define NDEF_TLV_TYPE_NDEF_MSG   0x03
#define NDEF_TLV_TYPE_TERMINATOR 0xfe

/* NFC Forum Type-5 Capability Container magic bytes (first CC byte): 0xE1 for the
 * standard one-byte memory-size form, 0xE2 for the extended form. */
#define NDEF_T5T_CC_MAGIC_E1 0xe1
#define NDEF_T5T_CC_MAGIC_E2 0xe2

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

/* Read a TLV length field at *buffer (NFC Forum Type 5): either a single length
 * byte, or the three-byte form (0xff followed by a big-endian 16-bit length).
 * Advances past the length field and returns the value length via *out_len.
 * Returns false when the buffer does not hold the whole length field. */
static bool read_tlv_length(const uint8_t **buffer, size_t *remaining_len, size_t *out_len)
{
	if (*remaining_len == 0) {
		return false;
	}

	uint8_t length = **buffer;

	if (!advance_buffer(buffer, remaining_len, 1)) {
		return false;
	}

	if (length == 0xff) {
		/* Three-byte format: 0xff followed by a 2-byte length */
		if (*remaining_len < 2) {
			return false;
		}

		*out_len = sys_get_be16(*buffer);
		advance_buffer(buffer, remaining_len, 2);
	} else {
		/* Single-byte format */
		*out_len = length;
	}

	return true;
}

int app_ndef_parser_run(const uint8_t *buffer, size_t buffer_len,
			app_ndef_parser_callback_t callback, void *user_data)
{
	int ret;

	const uint8_t *ndef_msg_start = NULL;
	size_t ndef_msg_len = 0;
	size_t remaining_len = buffer_len;

	/* NFC Forum Type-5 user memory is laid out as a fixed Capability Container
	 * (CC) followed by the TLV area, with the NDEF Message TLV at offset 4. The
	 * CC is NOT a TLV, so skip it before the scan: otherwise its magic byte
	 * (0xE1/0xE2) is read as an unknown TLV and the following memory-size byte
	 * (e.g. 0x40 = 64) is taken as a length that jumps clean past the NDEF
	 * message at offset 4 (#218 — regression of the #199 value-skip). A raw TLV
	 * buffer written from offset 0 (no CC magic) is parsed unchanged. */
	if (remaining_len >= 4 &&
	    (buffer[0] == NDEF_T5T_CC_MAGIC_E1 || buffer[0] == NDEF_T5T_CC_MAGIC_E2)) {
		/* 4-byte CC normally; 8-byte extended form when the MLEN byte is 0. */
		size_t cc_len = (buffer[2] == 0x00) ? 8 : 4;

		if (!advance_buffer(&buffer, &remaining_len, cc_len)) {
			LOG_WRN("Capability Container (%zu B) exceeds buffer", cc_len);
			return -ENOMSG;
		}
	}

	/* Find the NDEF Message TLV (Type-Length-Value) block */
	while (remaining_len > 0) {
		uint8_t type = *buffer;

		if (!advance_buffer(&buffer, &remaining_len, 1)) {
			return -ENODATA;
		}

		if (type == NDEF_TLV_TYPE_TERMINATOR) {
			LOG_INF("Terminator TLV found, no NDEF message");
			return 0;
		}

		/* NULL TLV: a single padding byte with no length/value field. */
		if (type == NDEF_TLV_TYPE_NULL) {
			continue;
		}

		/* Every other TLV (NDEF message + proprietary/unknown) carries a
		 * length followed by that many value bytes. */
		size_t tlv_len;

		if (!read_tlv_length(&buffer, &remaining_len, &tlv_len)) {
			LOG_ERR("Invalid TLV length");
			return -EMSGSIZE;
		}

		if (remaining_len < tlv_len) {
			LOG_ERR("TLV length (%zu) exceeds buffer size (%zu)", tlv_len,
				remaining_len);
			return -EMSGSIZE;
		}

		if (type == NDEF_TLV_TYPE_NDEF_MSG) {
			ndef_msg_len = tlv_len;
			ndef_msg_start = buffer;

			/* Found our message */
			break;
		}

		/* Unknown TLV: skip its value bytes and keep scanning (#199). The
		 * loop previously advanced only the type byte, so the value bytes of
		 * a leading non-NDEF TLV were reinterpreted as TLV types — a crafted
		 * tag could make the parser latch onto an attacker-chosen offset as
		 * the NDEF message. */
		if (!advance_buffer(&buffer, &remaining_len, tlv_len)) {
			LOG_ERR("Unknown TLV value exceeds buffer");
			return -EMSGSIZE;
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
