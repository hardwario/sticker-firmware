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
 * AES-CCM encrypts+authenticates the body under the derived `session_key`
 * (doc/p2p.md §4, derived directly from the device's existing LoRaWAN OTAA
 * AppKey -- there is no manual p2p_key config parameter and no separate
 * join_key).
 *
 * Phase 1 shipped deliberately unpaired/unACKed fire-and-forget: net_id/
 * dev_addr were the fixed pre-join value 0 (doc/p2p.md §5.3's own
 * JoinRequest convention), with no join/ACK handshake at all. Phase 2
 * (#118) adds the join handshake itself: on first start with no persisted
 * pairing state, the device sends JoinRequest, opens a bounded RX1 window
 * for JoinAccept, and on success persists net_id/dev_addr/session_key/
 * rx1_delay to NVS and switches the data plane on to session_key
 * (doc/p2p.md §5.3). app_p2p_is_ready() (and therefore the report
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
 * doc/p2p.md §5.3), the rest (Detach/RejoinRequest, §7) are not yet. */
enum app_p2p_frame_type {
	APP_P2P_FRAME_TELEMETRY = 2,
	APP_P2P_FRAME_ALARM = 3,
	APP_P2P_FRAME_RESPONSE = 85,
	APP_P2P_FRAME_COMMAND = 86, /* inbound (RX): reserved, not dispatched yet */
	APP_P2P_FRAME_JOIN_REQUEST = 0xF0,
	APP_P2P_FRAME_JOIN_ACCEPT = 0xF1,
	APP_P2P_FRAME_ACK = 0xFA,
};

/* LoRa PHY max payload -- the largest a single P2P wire frame (header + body
 * + tag) can ever be. Keep in sync with P2P_FRAME_MAX in app_p2p.c; sizes a
 * caller's dry-run compose buffer (app_p2p_debug_compose() below). */
#define APP_P2P_FRAME_MAX_LEN 255

/* Join/session state (#118 phase 2, doc/p2p.md §5.3) -- public so `ats radio
 * status` can report it via struct app_p2p_info below. */
enum p2p_link_state {
	P2P_LINK_UNPAIRED, /* no valid pairing in NVS; not currently joining */
	P2P_LINK_JOINING,  /* boot-window join attempts in progress */
	P2P_LINK_PAIRED,   /* net_id/dev_addr/session_key valid, data plane live */
};

/* Configure the radio from the p2p config group and set up the work queue.
 * Returns 0 or a negative errno. */
int app_p2p_init(void);

/* Boot-time bring-up. Refuses outright, and logs an error, if `lrw_appkey`
 * is all-zero: it is the root key for the whole transport, so an all-zero one
 * is a publicly known key and joining under it is forgeable by anyone in
 * range (doc/p2p.md §4). This is checked before the paired shortcut below, so
 * a device re-enabled into `radio-mode p2p` after a factory_reset -- which
 * wipes lrw_appkey but leaves the persisted pairing intact -- refuses rather
 * than resuming a session it can never renew (doc/p2p.md §7).
 *
 * Otherwise: if already paired (persisted NVS state from a prior
 * join), mark the radio ready and kick the report cadence immediately --
 * unlike app_lrw_join() on the radio facade, an existing pairing is treated
 * as sufficient, so a normal power cycle never wastes a JoinRequest
 * (doc/p2p.md §7). Otherwise starts the join handshake (#118 phase 2,
 * doc/p2p.md §5.3); the ready callback fires later, only once JoinAccept
 * succeeds. See app_p2p_rejoin() below for forcing a fresh join on demand. */
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

/* Snapshot for `ats radio status` (#118) -- the P2P analogue of struct
 * app_lrw_info, but the raw-LoRa protocol has no per-frame link-quality
 * feedback (no ADR/margin/gateway count), so this only surfaces
 * pairing/session state. */
struct app_p2p_info {
	enum p2p_link_state link_state;
	uint32_t net_id;   /* 0 pre-pairing */
	uint16_t dev_addr; /* 0 pre-pairing */
	uint8_t rx1_delay_s;
	uint32_t fcnt;              /* next data-plane TX counter */
	uint32_t dev_nonce;         /* JoinRequest anti-replay counter, never resets */
	uint32_t ack_retry_pending; /* frames currently awaiting an Ack retry */
	/* False means lrw_appkey is all-zero, i.e. the device has no root key
	 * for P2P at all and app_p2p_start()/app_p2p_rejoin() refuse to bring
	 * the radio up (doc/p2p.md §4). Without this, such a device is
	 * indistinguishable from a plain UNPAIRED one on the bench. */
	bool app_key_set;
};

/* Fill `info` with the current pairing/session snapshot. Always succeeds. */
void app_p2p_get_info(struct app_p2p_info *info);

#if defined(CONFIG_SHELL)
/* Bench-rig reference receiver (doc/p2p.md §14): enable=true reconfigures the
 * radio for continuous RX and starts async receive -- each frame is
 * filtered by net_id and logged with RSSI/SNR/frame_type/counter.
 * JoinRequest/JoinAccept bodies are cleartext (see app_p2p.c's
 * P2P_JOIN_TAG_LABEL comment) and are logged as-is; any other frame_type is
 * a session_key-encrypted data-plane frame this diagnostic listener has no
 * key for and cannot decode. enable=false stops it and returns to TX
 * config. Returns 0 or a negative errno. TX (send_telemetry/queue_response/
 * send_alarm) is refused with -EBUSY while listening. */
int app_p2p_listen(bool enable);

/* Force a fresh join handshake RIGHT NOW, even if currently PAIRED. Subject
 * to the same all-zero `lrw_appkey` refusal as app_p2p_start() -- the shell
 * is not a way around it --
 * unlike app_p2p_start(), an existing pairing is not treated as sufficient.
 * A successful JoinAccept overwrites the old pairing via pairing_persist(),
 * so this never needs a reboot or NVS wipe (contrast with app_p2p_unjoin()
 * below). This is what makes the shell's `join` command genuinely force a
 * fresh session on both radio stacks. */
void app_p2p_rejoin(void);

/* Clear the persisted pairing (net_id/dev_addr/session_key/rx1_delay) so the
 * next boot starts a fresh JoinRequest. NEVER touches the dev_nonce
 * anti-replay counter -- see dnonce_persist()'s comment in app_p2p.c for why
 * a re-join must never risk presenting a dev_nonce the central already saw.
 * Mirrors app_lrw_reset_nvm(): settings only, reboot required to take
 * effect. Returns 0 or a negative errno. */
int app_p2p_unjoin(void);

/* Debug: override the live rx1_delay used for the next TX's RX window,
 * without persisting it or requiring a re-join (doc/p2p.md §13). A real
 * JoinAccept overwrites it back to the paired value. */
void app_p2p_debug_set_rx1_delay(uint8_t rx1_delay_s);

/* Debug: make the next `count` confirmed-uplink Acks appear dropped (as if
 * the central never replied), to exercise the retry path (doc/p2p.md §6)
 * deterministically without a real RF outage. 0 disables the injection. */
void app_p2p_debug_drop_acks(uint32_t count);

/* Debug: build (frame + encrypt) one TELEMETRY frame under the CURRENT
 * session state WITHOUT transmitting or advancing the frame counter -- lets
 * a bench tech inspect the exact bytes that would go on air. Runs on the P2P
 * work queue like a real send (app_compose.c's "solely on m_work_q"
 * invariant, mirrored here for P2P's own queue). `*more` reports whether
 * app_compose has additional frames pending (call again to drain them, same
 * idiom as app_compose_ex()/app_lrw_run_on_work_q()). Returns 0, -ENOTCONN
 * if not paired yet, -ENOMEM if `out_size` is too small, or a negative
 * errno. */
int app_p2p_debug_compose(uint8_t *out, size_t out_size, size_t *out_len, bool *more);
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_P2P_H_ */
