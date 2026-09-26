/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_RADIO_H_
#define APP_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Link state of the active radio, shared by both backends (doc/plan/439 T1).
 * The values are the wire values of Info.lrw_state (app_config.proto), so keep
 * the order. LoRaWAN: IDLE before the first join, JOINING, HEALTHY, WARNING
 * while link checks fail, RECONNECT after the link was lost, DISABLED when the
 * DevEUI is all-zero (#98). P2P: IDLE when unpaired and not joining (after a
 * Detach), JOINING for a boot/forced join, HEALTHY when paired, WARNING after
 * P2P_WARNING_FAIL_THRESHOLD failed confirmed cycles, RECONNECT for a
 * self-heal/RejoinRequest join, DISABLED when lrw_appkey or lrw_deveui is
 * all-zero. */
enum app_radio_state {
	APP_RADIO_STATE_IDLE,
	APP_RADIO_STATE_JOINING,
	APP_RADIO_STATE_HEALTHY,
	APP_RADIO_STATE_WARNING,
	APP_RADIO_STATE_RECONNECT,
	APP_RADIO_STATE_DISABLED,
};

/* Radio facade (#118): one image links both the LoRaWAN stack (app_radio_lrw) and
 * the raw-LoRa P2P stack (app_radio_p2p, when CONFIG_RADIO_P2P=y). The active
 * one is chosen at boot from the `radio_mode` config parameter (off/lorawan/p2p)
 * and never changes at runtime (the SX126x radio is shared). The
 * radio-agnostic layers (app_report, app_compose, app_alarm, main) call
 * through this facade; the few LoRaWAN-only operations (join NVM reset, link
 * check, history replay, calibration's ABP join, …) stay direct app_radio_lrw_* calls
 * guarded by CONFIG_LORAWAN, unaffected by radio_mode. */

enum app_radio_kind {
	APP_RADIO_LORAWAN,
	APP_RADIO_P2P,
};

/* Read `radio_mode` from config and bring up the chosen stack (app_radio_lrw_init or
 * app_radio_p2p_init). Falls back to LoRaWAN if P2P is selected but not compiled in.
 * `radio_mode == off` also routes to app_radio_lrw_init(), which its own
 * radio_disabled() check keeps radio-silent. Returns 0 or a negative errno. */
int app_radio_init(void);

/* Start the link at boot: LoRaWAN always (re)joins; P2P only starts the join
 * handshake if NVS has no valid pairing yet, otherwise just marks ready --
 * a P2P session persists across a normal power cycle (doc/p2p.md §7), so
 * boot-time app_radio_p2p_start() must not waste a JoinRequest on every reboot. */
void app_radio_start(void);

#if defined(CONFIG_SHELL)
/* Force a fresh join attempt RIGHT NOW, regardless of current state --
 * unlike app_radio_start(), an existing P2P pairing is not treated as
 * sufficient. LoRaWAN already behaves this way unconditionally (every
 * app_radio_lrw_join() call deinits and rejoins); this is what makes `join`
 * genuinely symmetric across both stacks for the interactive shell command.
 * A successful P2P JoinAccept simply overwrites the old pairing via
 * pairing_persist(), so this never needs a reboot or NVS wipe. */
void app_radio_rejoin(void);
#endif

/* Which stack was selected at boot. */
enum app_radio_kind app_radio_get_kind(void);

/* Link state of the active radio (see enum app_radio_state): drives the status
 * LED, Info.lrw_state and the device_status radio bits for either backend. */
enum app_radio_state app_radio_get_state(void);

/* Link quality of the last downlink the node received (LoRaWAN: any downlink;
 * P2P: the last authenticated Ack / command / link-control frame), as measured
 * by the node, with its age in seconds. Returns false before the first one. */
bool app_radio_last_downlink(int16_t *rssi, int8_t *snr, uint32_t *age_s);

/* True when the link can carry an uplink now. */
bool app_radio_is_ready(void);

/* Application-payload budget (bytes) for the next uplink. */
uint8_t app_radio_get_max_payload(void);

/* Compose + send a telemetry snapshot (triggered by app_report). */
void app_radio_send_telemetry(void);

/* Same, for a host-requested uplink (force_send / sample, F14): LoRaWAN skips
 * its fleet pre-send jitter (app_radio_lrw_send_telemetry_now()); P2P has no
 * pre-send jitter, so it is the plain app_radio_p2p_send_telemetry(). */
void app_radio_send_telemetry_now(void);

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
