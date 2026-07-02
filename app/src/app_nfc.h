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

int app_nfc_init(void);

/* Full check: always reads the tag. Use at boot and for on-demand checks. A
 * staged command (hio.stck:cmd) is run through app_cmd_handle(); its deferred
 * action is taken separately via app_nfc_take_cmd_action() — offline/boot-staged
 * provisioning is unified on the encrypted Command/SetParam path (#250). */
int app_nfc_check(void);

/* Reads the tag and processes any pending command, restoring the info record
 * otherwise. Run from the poll thread after app_nfc_wait_event(). */
int app_nfc_poll(void);

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

/* True while a command response is staged but not yet fully written to the tag
 * (the RF field interrupted the write). The poll thread shortens its wait and
 * re-runs app_nfc_poll() to rewrite the cached reply, rather than restoring the
 * info record. */
bool app_nfc_resp_write_pending(void);
int app_nfc_restore_info(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_NFC_H_ */
