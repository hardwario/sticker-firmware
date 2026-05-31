/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CMD_H_
#define APP_CMD_H_

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum app_cmd_transport {
	APP_CMD_TRANSPORT_LRW,
	APP_CMD_TRANSPORT_NFC,
	APP_CMD_TRANSPORT_SHELL_DEBUG,
};

/* Action the caller must perform AFTER the response has been sent (so the Ack
 * leaves before the device reboots). Set by app_cmd_handle(). */
enum app_cmd_action {
	APP_CMD_ACTION_NONE = 0,
	APP_CMD_ACTION_SETTINGS_SAVE, /* persist staged config + reboot */
	APP_CMD_ACTION_REBOOT,        /* reboot (discards staged edits) */
	APP_CMD_ACTION_FACTORY_RESET, /* erase NVS + reboot */
};

/* Decode a serialized DownlinkCommand from `in`, dispatch it, encode the
 * resulting DownlinkResponse into `out`.
 *
 * Returns 0 on success with *out_len set to the encoded length. Returns a
 * negative errno only on encode failure (the caller's buffer is too small or
 * the protobuf library refused to serialize). A successful dispatch may still
 * produce an Error{...} response — that is not a function-level failure.
 *
 * On decode failure the function still returns 0 and encodes a
 * DownlinkResponse{ seq=0, error={ code=BAD_REQUEST } } into out.
 *
 * *action (if non-NULL) is set to a deferred action the caller must run after
 * transmitting the response (reboot/save/factory-reset). APP_CMD_ACTION_NONE
 * otherwise.
 */
int app_cmd_handle(enum app_cmd_transport transport,
		   const uint8_t *in, size_t in_len,
		   uint8_t *out, size_t out_cap, size_t *out_len,
		   enum app_cmd_action *action);

#ifdef __cplusplus
}
#endif

#endif /* APP_CMD_H_ */
