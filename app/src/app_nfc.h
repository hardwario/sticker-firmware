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
 * (reboot/save/device-reset/factory-reset/...). The caller runs it after the
 * response is on the tag, so the phone can still read the Ack first. Returns
 * APP_CMD_ACTION_NONE when there is nothing pending. */
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

/* True once app_nfc_init() has succeeded (ST25DV tag usable). False means the
 * tag is unavailable and the device runs degraded (#88). */
bool app_nfc_ready(void);

/* True while an NFC exchange is in progress (a phone is interacting). The main
 * loop suppresses its periodic status/heartbeat LED blink while this is set so it
 * does not fight the NFC interaction LED (app_nfc.c). */
bool app_nfc_session_active(void);

/* Claim window states, persisted under the "clm/state" key (#247/#415).
 * Returned by app_nfc_claim_state_get(); the numeric values are wire-stable and
 * match the legacy pending/consumed bytes so the NVS migration is a no-op for
 * the common cases (see app_nfc.c). */
#define APP_NFC_CLAIM_ACTIVE 1 /* claimable: clm record laid, get_claim_info discloses token */
#define APP_NFC_CLAIM_DONE   2 /* claimed: no clm record, get_claim_info -> NOT_READY */

/* (Re)open the claim window (#415): CLAIM_ACTIVE + persist, no reboot. Reached
 * from the claim_active command, `ats claim active`, and app_settings_vendor_reset()
 * (#299) — the one reset tier deep enough to re-provision; device_reset/
 * factory_reset deliberately leave the claim state alone (see app_nfc.c). */
void app_nfc_claim_active(void);

/* Close the claim window (#415, claim_done command / `ats claim done`):
 * CLAIM_DONE + persist, no reboot. Idempotent — always safe to call. Replaces
 * the #308 implicit close (any decrypted command); the app must now send this
 * explicitly after storing the keys. */
void app_nfc_claim_done(void);

/* Current claim window state (APP_NFC_CLAIM_ACTIVE / APP_NFC_CLAIM_DONE), for
 * `ats claim status`, the get_claim_info handler, and tests. */
uint8_t app_nfc_claim_state_get(void);

/* Whether the "processing"/"rejected" NFC LED blink timer is currently armed
 * (#340 L1 regression test support: a hard response-write failure on the
 * boot-staged path, which has no RF-session backstop, must stop it). */
bool app_nfc_led_blink_active(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_NFC_H_ */
