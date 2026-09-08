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

/* Frame geometry (data plane), doc/p2p.md §3. Single source of truth, shared
 * by app_p2p.c and tests/p2p_logic. */
#define P2P_HDR_LEN   11
#define P2P_TAG_LEN   4 /* data-plane (session_key) CCM tag length only */
#define P2P_NONCE_LEN 13
#define P2P_KEY_LEN   16
#define P2P_DIR_TX    0x00
#define P2P_DIR_RX    0x01
#define P2P_LORA_MTU  255
#define P2P_MAX_BODY  (P2P_LORA_MTU - P2P_HDR_LEN - P2P_TAG_LEN) /* 240 */
#define P2P_FRAME_MAX (P2P_HDR_LEN + P2P_MAX_BODY + P2P_TAG_LEN)

/* Duty-cycle ledger tuning (B2 / decision D1), shared with tests/p2p_logic. */
#define P2P_DUTY_WINDOW_MS 3600000 /* the sliding window: one hour */
#define P2P_DUTY_BUDGET_MS 36000   /* 1% of it -- the air-time allowance */

/* Ring capacity. One entry per transmission still inside the window, so this
 * bounds how many frames an hour may contain before the LEDGER rather than
 * the air-time budget becomes the limit: at SF10 the smallest frame the node
 * sends is 17 B / 330 ms, so 36 000 ms buys ~109 of them and 48 entries bind
 * first above ~48 uplinks/hour. That is deliberately conservative -- it can
 * only ever delay a transmission, never permit one the budget forbids (see
 * p2p_duty_wait_ms) -- but it means a bench run wanting the air-time budget
 * to be the visible limit needs an interval above ~75 s. doc/p2p.md §6. */
#define P2P_DUTY_LEDGER_ENTRIES 48

/* Ack (0xFA) body layout, doc/p2p.md §6:
 *
 *   flags(1) | rssi(i8) | snr(i8) | [pending_frame_len(1) if bit0] |
 *   [unix_be32 if bit1]
 *
 * giving valid lengths 3, 4, 7 and 8. The body is NOT wire-versioned (P2P is
 * pre-deployment) -- it is self-describing by length, so the node derives the
 * shape from the received frame length and treats the flags as a refinement,
 * never as the authority. Shared with the pure parser and tests/p2p_logic. */
#define P2P_ACK_FLAG_PENDING    0x01 /* bit 0: a downlink command is pending (B4) */
#define P2P_ACK_FLAG_TIME       0x02 /* bit 1: 4-byte Unix time tail present (B5) */
#define P2P_ACK_BODY_BASE_LEN   3    /* flags | rssi_i8 | snr_i8 */
#define P2P_ACK_PENDING_LEN_LEN 1    /* optional on-air length of the pending 0x56 */
#define P2P_ACK_TIME_LEN        4    /* optional big-endian Unix seconds */
#define P2P_ACK_BODY_MAX_LEN                                                                       \
	(P2P_ACK_BODY_BASE_LEN + P2P_ACK_PENDING_LEN_LEN + P2P_ACK_TIME_LEN) /* 8 */

/* Parsed Ack body, filled by p2p_parse_ack_body(). */
struct p2p_ack_info {
	uint8_t flags;      /* raw flags byte (P2P_ACK_FLAG_*) */
	int8_t rssi;        /* central-measured uplink RSSI, dBm */
	int8_t snr;         /* central-measured uplink SNR, dB */
	bool time_present;  /* a valid Unix time tail was present */
	uint32_t unix_time; /* wall-clock seconds (valid iff time_present) */
	/* Total on-air length of the NEXT 0x56 the central will deliver
	 * (11 B header + ciphertext + 4 B tag), so the node can size its RX1
	 * window exactly instead of opening for a 255 B worst case. False when
	 * the central is still on the pre-announcement 3/7-byte body -- the
	 * node then falls back to the old worst case (doc/p2p.md §6). */
	bool pending_len_present;
	uint8_t pending_frame_len;
};

/* Exact sliding-hour duty ledger (B2, decision D1) -- one entry per
 * transmission that is still inside the window. Replaces the earlier token
 * bucket, which refilled at 1% of wall time and capped at the full hourly
 * allowance: that held the long-run average at 1% but let a node idle for an
 * hour and then burst 36 s of air in one go, so the worst-case SLIDING hour
 * reached ~2%. Summing the real window costs 384 B of RAM and makes the
 * bound exact instead of amortised.
 *
 * Defined here so tests/p2p_logic can declare one; the ledger functions are
 * internal to app_p2p.c (given external linkage only under CONFIG_ZTEST --
 * see the block at the end of this header). */
struct p2p_duty_entry {
	uint32_t end_ms; /* uptime (ms, truncated) at which the frame finished */
	uint16_t air_ms; /* its time-on-air; a 255 B SF12 frame is ~9.2 s, so u16 fits */
};

struct p2p_duty {
	struct p2p_duty_entry entries[P2P_DUTY_LEDGER_ENTRIES];
	uint8_t head;  /* index of the oldest entry */
	uint8_t count; /* entries in use */
};

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
 * self-healing re-join (§7) and the Detach/RejoinRequest link-control
 * downlinks (§5.4) are all implemented. The optional listen mode
 * (CONFIG_SHELL) puts the radio in continuous RX for the two-STICKER bench
 * rig (doc/p2p.md §14) -- it decrypts and logs received frames but does not
 * dispatch COMMAND frames to app_cmd (that arrives with a later, real
 * anti-replay-protected command channel).
 */

/* Wire frame types -- mirror the LoRaWAN fPort values so the off-device
 * decoder logic is shared (doc/p2p.md §3.2). 0xF0-0xFE are reserved for
 * link control; all of them are handled now that Detach/RejoinRequest land
 * (§5.4/§7). Values are the wire contract shared with the central
 * (proximos-v2 control-radio, src/p2p/frame.rs::frame_type) -- never
 * renumber one without changing it there in the same release. */
enum app_p2p_frame_type {
	APP_P2P_FRAME_TELEMETRY = 2,
	APP_P2P_FRAME_ALARM = 3,
	APP_P2P_FRAME_RESPONSE = 85,
	APP_P2P_FRAME_COMMAND = 86, /* inbound (RX): downlink command, dispatched (B4) */
	APP_P2P_FRAME_JOIN_REQUEST = 0xF0,
	APP_P2P_FRAME_JOIN_ACCEPT = 0xF1,
	APP_P2P_FRAME_ACK = 0xFA,
	/* Link control, both inbound (RX), both empty-bodied and authenticated
	 * under session_key with the acknowledged uplink's counter (§5.4). */
	APP_P2P_FRAME_DETACH = 0xFD,
	APP_P2P_FRAME_REJOIN_REQUEST = 0xFE,
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
	/* B1: RSSI/SNR the central reported in the last Ack (its measurement of
	 * this device's uplink). last_ack_valid is false until the first Ack of
	 * the current session. */
	int8_t last_ack_rssi;
	int8_t last_ack_snr;
	bool last_ack_valid;
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

#if defined(CONFIG_ZTEST)
/* Pure decision-logic helpers, internal to app_p2p.c (static in the firmware),
 * exposed with external linkage for tests/p2p_logic only. Not part of the
 * runtime API -- do not call from firmware. */
uint32_t p2p_toa_ms(int sf, uint8_t payload_len);
void build_nonce(uint8_t nonce[13], uint32_t counter, uint16_t dev_addr, uint8_t frame_type,
		 uint8_t dir);
int build_frame_keyed(uint32_t net_id, uint16_t dev_addr, const uint8_t session_key[16],
		      uint8_t frame_type, const uint8_t *body, size_t body_len, uint32_t counter,
		      uint8_t *frame);
void p2p_duty_init(struct p2p_duty *d);
void p2p_duty_charge(struct p2p_duty *d, int64_t now_ms, uint32_t air_ms);
int64_t p2p_duty_wait_ms(struct p2p_duty *d, int64_t now_ms, uint32_t air_ms);
uint32_t p2p_rejoin_backoff_ms(uint8_t attempt);
bool p2p_parse_ack_body(const uint8_t *body, size_t body_len, struct p2p_ack_info *out);
void p2p_test_set_fcnt(uint32_t next, uint32_t reserved);
uint32_t p2p_test_get_fcnt(void);
int p2p_test_fcnt_next(uint32_t *counter_out);
#endif /* defined(CONFIG_ZTEST) */

#ifdef __cplusplus
}
#endif

#endif /* APP_P2P_H_ */
