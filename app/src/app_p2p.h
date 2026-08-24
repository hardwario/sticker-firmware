/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_P2P_H_
#define APP_P2P_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Raw-LoRa point-to-point transport, phase 1 (#118, doc/p2p.md). A drop-in
 * alternative to app_lrw for deployments without LoRaWAN infrastructure,
 * selected at boot by `radio-mode p2p` via the app_radio facade. It
 * mirrors the slice of the app_lrw public surface the radio-agnostic
 * layers (app_report / app_compose / app_alarm) need, but talks raw LoRa via
 * the Zephyr drivers/lora API instead of LoRaMac -- no join, no network
 * server, no per-DR payload budget.
 *
 * The payload layer is reused unchanged: app_compose builds the protobuf
 * Telemetry snapshot exactly as for LoRaWAN; this module only frames it (an
 * 11 B cleartext header replacing the LoRaWAN fPort, see app_p2p.c) and
 * AES-CCM encrypts+authenticates the body under the derived `join_key`
 * (doc/p2p.md §4 -- there is no manual p2p_key config parameter).
 *
 * Phase 1 shipped deliberately unpaired/unACKed fire-and-forget: net_id/
 * dev_addr were the fixed pre-join value 0 (doc/p2p.md §5.3's own
 * JoinRequest convention), with no join/ACK handshake at all. Phase 2
 * (#118) adds the join handshake itself: on first start with no persisted
 * pairing state, the device sends JoinRequest, opens a bounded RX1 window
 * for JoinAccept, and on success persists net_id/dev_addr/session_key/
 * rx1_delay to NVS and switches from join_key to session_key for the data
 * plane (doc/p2p.md §5.3). app_p2p_is_ready() (and therefore the report
 * cadence) only goes true once paired -- a device stuck unpaired past its
 * boot join window (§5.2, 120 s) stays silent until the next boot or an NFC
 * `p2p_join` trigger (not yet wired). The confirmed-uplink Ack/retry (§6),
 * self-healing re-join (§7) and Detach (§5.4) are not implemented yet. The
 * optional listen mode (CONFIG_SHELL) puts the radio in continuous RX for
 * the two-STICKER bench rig (doc/p2p.md §14) -- it decrypts and logs
 * received frames but does not dispatch COMMAND frames to app_cmd (that
 * arrives with a later, real anti-replay-protected command channel).
 */

/* Wire frame types -- mirror the LoRaWAN fPort values so the off-device
 * decoder logic is shared (doc/p2p.md §3.2). 0xF0-0xFE are reserved for
 * link control; JOIN_REQUEST/JOIN_ACCEPT are implemented (#118 phase 2,
 * doc/p2p.md §5.3), the rest (Ack/Detach/RejoinRequest, §6/§7) are not yet. */
enum app_p2p_frame_type {
	APP_P2P_FRAME_TELEMETRY = 2,
	APP_P2P_FRAME_ALARM = 3,
	APP_P2P_FRAME_RESPONSE = 85,
	APP_P2P_FRAME_COMMAND = 86, /* inbound (RX): reserved, not dispatched yet */
	APP_P2P_FRAME_JOIN_REQUEST = 0xF0,
	APP_P2P_FRAME_JOIN_ACCEPT = 0xF1,
};

/* Configure the radio from the p2p config group and set up the work queue.
 * Returns 0 or a negative errno. */
int app_p2p_init(void);

/* If already paired (persisted NVS state from a prior join), mark the
 * radio ready and kick the report cadence immediately -- mirrors
 * app_lrw_join() on the radio facade. Otherwise starts the join
 * handshake (#118 phase 2, doc/p2p.md §5.3); the ready callback fires later,
 * only once JoinAccept succeeds. */
void app_p2p_start(void);

/* True once paired and started -- immediately if NVS already had a valid
 * pairing, otherwise only after the join handshake (#118 phase 2)
 * completes. */
bool app_p2p_is_ready(void);

/* Fixed application-payload budget for one frame (LoRa MTU minus the P2P
 * header and AES-CCM tag). app_compose() bin-packs telemetry groups against
 * this. */
uint8_t app_p2p_get_max_payload(void);

/* Compose + send a telemetry snapshot (frame type TELEMETRY) via
 * app_compose_budget(). Triggered by app_report after it samples + captures
 * history, same as app_lrw_send_telemetry(). */
void app_p2p_send_telemetry(void);

/* Send a staged command response (frame type RESPONSE). */
int app_p2p_queue_response(uint8_t port, const uint8_t *buf, size_t len);

/* Send an alarm-detail batch (frame type ALARM). */
int app_p2p_send_alarm(const uint8_t *buf, size_t len);

/* Register the link-ready kick fired by app_p2p_start() so app_report can
 * begin the cadence. NULL clears it. */
void app_p2p_register_ready_cb(void (*cb)(void));

/* Stop P2P radio activity ahead of a deep-sleep poweroff. */
void app_p2p_suspend(void);

#if defined(CONFIG_SHELL)
/* Bench-rig reference receiver (doc/p2p.md §14): enable=true reconfigures the
 * radio for continuous RX and starts async receive -- each frame is
 * validated, AES-CCM decrypted under the derived join_key and logged with
 * RSSI/SNR/frame_type/counter; enable=false stops it and returns to TX
 * config. Returns 0 or a negative errno. TX (send_telemetry/queue_response/
 * send_alarm) is refused with -EBUSY while listening. */
int app_p2p_listen(bool enable);
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_P2P_H_ */
