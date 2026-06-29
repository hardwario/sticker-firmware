/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_NFC_PARSER_H_
#define APP_NFC_PARSER_H_

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

struct app_nfc_parser_record_info {
	uint8_t tnf;
	const uint8_t *type;
	uint32_t type_len;
	const uint8_t *payload;
	uint32_t payload_len;
};

typedef int (*app_nfc_parser_callback_t)(const struct app_nfc_parser_record_info *record_info,
					 void *user_data);

int app_nfc_parser_run(const uint8_t *buffer, size_t buffer_len, app_nfc_parser_callback_t callback,
		       void *user_data);

#endif /* APP_NFC_PARSER_H_ */
