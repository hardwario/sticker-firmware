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

/* Reset the claim-record lifecycle (#247) back to CLM_UNSET and persist, so the
 * device re-opens provisioning (as if freshly manufactured) — used by
 * app_settings_vendor_reset() (#299), the one reset tier deep enough to matter;
 * device_reset/factory_reset deliberately leave clm state alone (see app_nfc.c). */
void app_nfc_clm_reset(void);

/* Explicit claim-ack (#308, clm_ack command): transitions CLM_PENDING -> CLM_CONSUMED
 * and persists, no reboot. A no-op if not currently PENDING (already consumed, or
 * never armed) - always safe to call. */
void app_nfc_clm_ack(void);

/* Current claim-record lifecycle state (0=unset, 1=pending, 2=consumed), for
 * `ats claim status` (moved out of the nfc shell group so claim commands live
 * in one place). */
uint8_t app_nfc_clm_state_get(void);

/* Whether the "processing"/"rejected" NFC LED blink timer is currently armed
 * (#340 L1 regression test support: a hard response-write failure on the
 * boot-staged path, which has no RF-session backstop, must stop it). */
bool app_nfc_led_blink_active(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_NFC_H_ */
