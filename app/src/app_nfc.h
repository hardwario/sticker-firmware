/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_NFC_H_
#define APP_NFC_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

enum app_nfc_action {
	APP_NFC_ACTION_NONE = 0,
	APP_NFC_ACTION_SAVE = 1,
	APP_NFC_ACTION_RESET = 2,
};

int app_nfc_init(void);

/* Full check: always reads the tag. Use at boot and for on-demand checks. */
int app_nfc_check(enum app_nfc_action *action);

/* Reads the tag and processes any pending command/config, restoring the info
 * record otherwise. Run from the poll thread after app_nfc_wait_event(). */
int app_nfc_poll(enum app_nfc_action *action);

/* Block until the ST25DV GPO line signals RF activity (the phone touched the
 * tag) or `fallback_ms` elapses. Lets the poll thread sleep instead of busy
 * polling. Returns 0 if woken by the GPO interrupt, -EAGAIN on timeout. */
int app_nfc_wait_event(int fallback_ms);

/* Take (and clear) the deferred action requested by the last NFC command
 * (reboot/save/factory-reset). The caller runs it after the response is on the
 * tag, so the phone can still read the Ack first. Returns APP_CMD_ACTION_NONE
 * when there is nothing pending. */
enum app_cmd_action app_nfc_take_cmd_action(void);

/* Whether the main loop should run the periodic NFC check. Toggled by the
 * `nfc autocheck on|off` shell command so a multi-step `nfc write` of a config
 * blob is not raced (and overwritten) by the periodic check mid-write. */
bool app_nfc_periodic_enabled(void);

/* #164: a command/response exchange leaves the response record on the tag (the
 * immediate info-restore was dropped in #144 to avoid racing the phone read).
 * `app_nfc_info_restore_pending()` is true while that stale response is still on
 * the tag; the poll thread shortens its wait and, once the RF field has been
 * quiet for the debounce window (no GPO events), calls `app_nfc_restore_info()`
 * to rewrite the plaintext info record so a later tap finds valid metadata. */
bool app_nfc_info_restore_pending(void);
int app_nfc_restore_info(void);

/* Enter mailbox (ST25DV Fast-Transfer-Mode) serving mode until `idle_timeout_s`
 * of inactivity (0 = firmware default), holding the tag powered so the phone's
 * RF and our I2C reach it at once. Runs commands off the mailbox and returns the
 * number served (or a negative errno). Called from the poll thread when an
 * EnterMailbox command is taken via app_nfc_take_cmd_action(). */
int app_nfc_serve_mailbox(uint32_t idle_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* APP_NFC_H_ */
