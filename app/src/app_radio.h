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

/* Force a fresh join attempt RIGHT NOW, regardless of current state --
 * unlike app_radio_start(), an existing P2P pairing is not treated as
 * sufficient. LoRaWAN already behaves this way unconditionally (every
 * app_radio_lrw_join() call deinits and rejoins); this is what makes `join`
 * genuinely symmetric across both stacks -- the shell command and the lrw_join
 * command (NFC / downlink). A successful P2P JoinAccept simply overwrites the
 * old pairing via pairing_persist(), so this never needs a reboot or NVS
 * wipe. */
void app_radio_rejoin(void);

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

/* The active radio retires a delivered command only on a matching answer: the
 * P2P central keeps a 0x56 at the head of its queue and re-delivers it until a
 * 0x55 with the same seq arrives (doc/p2p.md B4/S3). LoRaWAN downlinks are
 * unconfirmed, so a command whose answer is an uplink of its own (force_send,
 * sample) needs no reply there. */
bool app_radio_needs_command_answer(void);

/* Stage a command response for the next uplink. */
int app_radio_queue_response(uint8_t port, const uint8_t *buf, size_t len);

/* Stage an alarm-detail batch. */
int app_radio_send_alarm(const uint8_t *buf, size_t len);

/* Register the link-ready kick app_report uses to (re)start the cadence. */
void app_radio_register_ready_cb(void (*cb)(void));

/* Stop radio activity ahead of a deep-sleep poweroff. */
void app_radio_suspend(void);

/* ---- Boot/join announce (#412, #409 A5a, #425; doc/plan/439 T3) ----------
 * One path for both radios. When the link comes up (LoRaWAN join, P2P paired
 * at boot or by a JoinAccept) the backend calls app_radio_announce(); app_radio
 * then sends the Info (seq 0) followed by the settings-info ConfigDump, each
 * paged for the current budget. A frame that does not fit yet, or that waits
 * for a running page stream, stays pending and goes out on a later
 * app_radio_announce_run(): the backend runs it on its work queue whenever
 * room may have appeared (link up, DR rise, page stream end, queue space). */
void app_radio_announce(void);

/* Something of the announce is still to be sent. */
bool app_radio_announce_pending(void);

/* Backend: a queued announce frame had to be dropped (the budget fell under
 * it); arm it again for the next run. */
void app_radio_announce_rearm(bool settings);

/* Backend work queue only: send what is pending while the link is up. Returns
 * true while something stays pending that a later run can send. */
bool app_radio_announce_run(void);

/* Backend work queue only: an Info carrying `seq` (the clock_sync answer),
 * paged like the announce. When not even page 0 can go now, the seq-0
 * announce Info is armed instead. Returns 0 or a negative errno. */
int app_radio_send_info(uint32_t seq);

/* clock_sync with an empty body: re-sync the RTC from the network and answer
 * with an Info carrying `seq` once the time has landed. LoRaWAN asks with
 * DeviceTimeReq; P2P sends an uplink now and uses the time tail of its Ack. */
void app_radio_clock_sync(uint32_t seq);

#ifdef __cplusplus
}
#endif

#endif /* APP_RADIO_H_ */
