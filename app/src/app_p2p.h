/*
 * Copyright (c) 2025 HARDWARIO a.s.
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

/* Raw-LoRa point-to-point transport (#118 / #121).
 *
 * A drop-in alternative to app_lrw for deployments without LoRaWAN
 * infrastructure. It mirrors the slice of the app_lrw public surface the
 * transport-agnostic layers (app_report / app_compose / app_alarm) need, but
 * talks raw LoRa via the Zephyr drivers/lora API instead of LoRaMac, so there
 * is no join, no network server and no per-DR payload budget.
 *
 * The payload layer is reused unchanged: app_compose builds the protobuf
 * Telemetry snapshot exactly as for LoRaWAN; this module only frames it (an
 * 8+ byte cleartext header that replaces the LoRaWAN fPort, see app_p2p.c) and
 * AES-CCM encrypts+authenticates the body with the configured p2p_key.
 *
 * v1 is unconfirmed fire-and-forget (no ACK/retry, no RX windows on a sending
 * node). The optional listen mode (CONFIG_SHELL) puts the radio in continuous
 * RX for the reference receiver / diagnostics. */

/* Wire frame types — replace the LoRaWAN fPort. Uplink types mirror the fPort
 * values app_lrw uses so the off-device decoder logic is shared. */
enum app_p2p_frame_type {
	APP_P2P_FRAME_TELEMETRY = 2,
	APP_P2P_FRAME_ALARM = 3,
	APP_P2P_FRAME_RESPONSE = 85,
	APP_P2P_FRAME_COMMAND = 86, /* inbound (RX): a Command for app_cmd */
};

/* Configure the radio from the p2p config group and set up the work queue.
 * Returns 0 or a negative errno. */
int app_p2p_init(void);

/* Mark the transport ready and kick the report cadence (there is no join, so
 * the link is "up" immediately). Mirrors app_lrw_join() on the transport facade. */
void app_p2p_start(void);

/* Always true once started (no join handshake). */
bool app_p2p_is_ready(void);

/* Fixed application-payload budget for one frame (LoRa MTU minus the P2P header
 * and AES-CCM tag). app_compose() bin-packs telemetry groups against this. */
uint8_t app_p2p_get_max_payload(void);

/* Compose + send a telemetry snapshot (frame type TELEMETRY). Triggered by
 * app_report after it samples + captures history, same as app_lrw_send_telemetry(). */
void app_p2p_send_telemetry(void);

/* Send a staged command response (frame type RESPONSE). */
int app_p2p_queue_response(uint8_t port, const uint8_t *buf, size_t len);

/* Send an alarm-detail batch (frame type ALARM). */
int app_p2p_send_alarm(const uint8_t *buf, size_t len);

/* Register the link-ready kick fired by app_p2p_start() so app_report can begin
 * the cadence. NULL clears it. */
void app_p2p_register_ready_cb(void (*cb)(void));

/* Stop P2P radio activity ahead of a deep-sleep poweroff. */
void app_p2p_suspend(void);

#if defined(CONFIG_SHELL)
/* Reference-receiver / diagnostic continuous RX. enable=true reconfigures the
 * radio for RX and starts async receive (each frame is validated, AES-CCM
 * decrypted and logged with RSSI/SNR; COMMAND frames are routed to app_cmd);
 * enable=false stops it and returns to TX config. Returns 0 or a negative errno. */
int app_p2p_listen(bool enable);
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_P2P_H_ */
