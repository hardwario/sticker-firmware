/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_RADIO_H_
#define APP_RADIO_H_

#include "app_lrw.h" /* enum app_lrw_state */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Radio facade (#118): one image links both the LoRaWAN stack (app_lrw) and
 * the raw-LoRa P2P stack (app_p2p, when CONFIG_APP_LORA_P2P=y). The active
 * one is chosen at boot from the `radio_mode` config parameter (off/lorawan/p2p)
 * and never changes at runtime (the SX126x radio is shared). The
 * radio-agnostic layers (app_report, app_compose, app_alarm, main) call
 * through this facade; the few LoRaWAN-only operations (join NVM reset, link
 * check, history replay, calibration's ABP join, …) stay direct app_lrw_* calls
 * guarded by CONFIG_LORAWAN, unaffected by radio_mode. */

enum app_radio_kind {
	APP_RADIO_LORAWAN,
	APP_RADIO_P2P,
};

/* Read `radio_mode` from config and bring up the chosen stack (app_lrw_init or
 * app_p2p_init). Falls back to LoRaWAN if P2P is selected but not compiled in.
 * `radio_mode == off` also routes to app_lrw_init(), which its own
 * radio_disabled() check keeps radio-silent. Returns 0 or a negative errno. */
int app_radio_init(void);

/* Start the link at boot: LoRaWAN always (re)joins; P2P only starts the join
 * handshake if NVS has no valid pairing yet, otherwise just marks ready --
 * a P2P session persists across a normal power cycle (doc/p2p.md §7), so
 * boot-time app_p2p_start() must not waste a JoinRequest on every reboot. */
void app_radio_start(void);

#if defined(CONFIG_SHELL)
/* Force a fresh join attempt RIGHT NOW, regardless of current state --
 * unlike app_radio_start(), an existing P2P pairing is not treated as
 * sufficient. LoRaWAN already behaves this way unconditionally (every
 * app_lrw_join() call deinits and rejoins); this is what makes `join`
 * genuinely symmetric across both stacks for the interactive shell command.
 * A successful P2P JoinAccept simply overwrites the old pairing via
 * pairing_persist(), so this never needs a reboot or NVS wipe. */
void app_radio_rejoin(void);
#endif

/* Which stack was selected at boot. */
enum app_radio_kind app_radio_get_kind(void);

/* Coarse link state for the status LED. P2P maps to HEALTHY once started so the
 * main loop's join/warning LED animations stay LoRaWAN-only. */
enum app_lrw_state app_radio_get_state(void);

/* True when the link can carry an uplink now. */
bool app_radio_is_ready(void);

/* Application-payload budget (bytes) for the next uplink. */
uint8_t app_radio_get_max_payload(void);

/* Compose + send a telemetry snapshot (triggered by app_report). */
void app_radio_send_telemetry(void);

/* Stage a command response for the next uplink. */
int app_radio_queue_response(uint8_t port, const uint8_t *buf, size_t len);

/* Stage an alarm-detail batch. */
int app_radio_send_alarm(const uint8_t *buf, size_t len);

/* Register the link-ready kick app_report uses to (re)start the cadence. */
void app_radio_register_ready_cb(void (*cb)(void));

/* Stop radio activity ahead of a deep-sleep poweroff. */
void app_radio_suspend(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_RADIO_H_ */
