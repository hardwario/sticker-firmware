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

/* Reconcile the resting NDEF record now (boot and `nfc check`): lays it down on
 * an empty tag, refreshes a stale one, restores it over foreign data. Needs the
 * RF field off (EEPROM); a phone holding the field makes it a no-op this pass. */
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

/* True once app_nfc_init() has succeeded (ST25DV tag usable). False means the
 * tag is unavailable and the device runs degraded (#88). */
bool app_nfc_ready(void);

/* Whether the ST25DV Fast-Transfer-Mode mailbox could be authorised at boot
 * (static MB_MODE set + verified). false = the mailbox command channel is dead
 * on this unit — a hardware/production defect, reported as
 * APP_DEVICE_STATUS_MAILBOX_DOWN for the production tester (#313 D7). */
bool app_nfc_mailbox_available(void);

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
