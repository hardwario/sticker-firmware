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
 * selected at boot by `radio-mode p2p` via the app_transport facade. It
 * mirrors the slice of the app_lrw public surface the transport-agnostic
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
 * Phase 1 is deliberately unpaired/unACKed fire-and-forget: net_id/dev_addr
 * are the fixed pre-join value 0 (doc/p2p.md §5.3's own JoinRequest
 * convention); the join/ACK handshake and net_id/dev_addr allocation are
 * phase 2. The optional listen mode (CONFIG_SHELL) puts the radio in
 * continuous RX for the two-STICKER bench rig (doc/p2p.md §14) -- it
 * decrypts and logs received frames but does not dispatch COMMAND frames to
 * app_cmd (that arrives with phase 2's real, anti-replay-protected channel).
 */

/* Wire frame types -- mirror the LoRaWAN fPort values so the off-device
 * decoder logic is shared (doc/p2p.md §3.2). 0xF0-0xFE are reserved for the
 * phase-2 join/ACK link control and not implemented yet. */
enum app_p2p_frame_type {
	APP_P2P_FRAME_TELEMETRY = 2,
	APP_P2P_FRAME_ALARM = 3,
	APP_P2P_FRAME_RESPONSE = 85,
	APP_P2P_FRAME_COMMAND = 86, /* inbound (RX): reserved, not dispatched in phase 1 */
};

/* Configure the radio from the p2p config group and set up the work queue.
 * Returns 0 or a negative errno. */
int app_p2p_init(void);

/* Mark the transport ready and kick the report cadence (there is no join, so
 * the link is "up" immediately). Mirrors app_lrw_join() on the transport
 * facade. */
void app_p2p_start(void);

/* Always true once started (no join handshake in phase 1). */
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
