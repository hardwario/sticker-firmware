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
