/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_RADIO_P2P_H_
#define APP_RADIO_P2P_H_

#include "app_radio.h" /* enum app_radio_state */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame layer: the TOWER radio frame, adopted verbatim (doc/plan/470 §4).
 * Single source of truth, shared by app_radio_p2p.c and tests/p2p_logic:
 *
 *   ver_type(1) | flags(1) | src(4) | dest(4) | counter(4) | ciphertext | tag(8)
 *
 * ver_type = version(3 bits, 1) << 5 | type(5 bits); every multi-byte field is
 * little-endian; the 14 B header is the CCM AAD. The nonce is src(4) |
 * counter(4) | bulk_idx(3) | 0x0000 -- no direction byte, the two directions
 * differ by src. Checked byte for byte against tests/ccm/tower_frame_kat.json
 * and tower_join_kat.json. */
#define TWR_VERSION         1
#define TWR_HDR_LEN         14
#define TWR_TAG_LEN         8
#define TWR_NONCE_LEN       13
#define TWR_TYPE_DATA       0
#define TWR_TYPE_ACK        1
#define TWR_FLAG_CONFIRMED  0x01
#define TWR_ACK_PENDING     0x01 /* ACK payload flags */
#define TWR_ACK_PAYLOAD_LEN 6    /* acked(4) rssi(1) flags(1); receivers accept >= 4 */
#define TWR_ACK_FRAME_LEN   (TWR_HDR_LEN + TWR_ACK_PAYLOAD_LEN + TWR_TAG_LEN) /* 28 */
#define TWR_ADDR_NONE       0x00000000u /* reserved; the JoinRequest's dest */
#define TWR_ADDR_BROADCAST  0xFFFFFFFFu /* reserved */

struct twr_hdr {
	uint8_t type;
	uint8_t flags;
	uint32_t src;
	uint32_t dest;
	uint32_t counter;
};

struct twr_ack {
	uint32_t acked;
	int8_t rssi; /* the gateway's RSSI of the acked frame; 0 if absent */
	bool pending;
};

/* The lora profile's MTU (D14, P1 decision D-g): a frame of at most 100 B, so a
 * 78 B payload -- the STICKER envelope (2 B) and a 76 B body, the LoRaWAN fPort
 * payload including its APP_PROTO_VERSION byte (D-a). */
#define P2P_FRAME_MAX   100
#define TWR_PAYLOAD_MAX (P2P_FRAME_MAX - TWR_HDR_LEN - TWR_TAG_LEN) /* 78 */
#define P2P_ENV_LEN     2
#define P2P_MAX_BODY    (TWR_PAYLOAD_MAX - P2P_ENV_LEN) /* 76 */
#define P2P_KEY_LEN     16

/* STICKER application envelopes (plan §8), the payload's first byte. */
#define P2P_ENV_DATA 0x81 /* port(1) | LoRaWAN fPort payload */
#define P2P_ENV_CTRL 0x91 /* { cmd(1) | len(1) | value(len) } x n */

/* 0x81 ports: the LoRaWAN fPort values, so the central reuses its decoder. */
enum p2p_port {
	P2P_PORT_TELEMETRY = 2,
	P2P_PORT_ALARM = 3,
	P2P_PORT_RESPONSE = 85, /* answers and the announce */
	P2P_PORT_COMMAND = 86,  /* downlink */
};

/* 0x91 command IDs (plan §8.2, final). P1 sends Capabilities, Hello, JoinReq,
 * LinkCheckReq and TimeReq, and handles Capabilities, Detach, RejoinReq,
 * JoinAccept, LinkCheckAns and TimeAns; any other ID is skipped by its length. */
#define P2P_CTRL_CAPABILITIES 0x01
#define P2P_CTRL_HELLO        0x02
#define P2P_CTRL_DETACH       0x03
#define P2P_CTRL_REJOIN_REQ   0x04
#define P2P_CTRL_JOIN_REQ     0x07
#define P2P_CTRL_JOIN_ACCEPT  0x08
#define P2P_CTRL_LINK_CHECK   0x10
#define P2P_CTRL_TIME         0x20

#define P2P_CAPS_LEN           12 /* proto(1) mtu(1) profiles(1) cmds(8) power_class(1) */
#define P2P_HELLO_LEN          9  /* session_id(4) reset_reason(1) fw(4) */
#define P2P_LINK_CHECK_ANS_LEN 4  /* rssi(i8) snr(i8) margin(i8) gw_count(1) */
#define P2P_TIME_ANS_LEN       9  /* unix(4) frac(1, 1/256 s) req_counter(4) */

/* Join frames (plan §6.3): TOWER Data frames under join_key, 0x91 payloads.
 *
 * JoinReq value: product_type(1) | proto_version(1) | dev_eui(8, MSB-first) |
 * fw_version(4: major, minor, patch, 0). JoinAccept value: net_id(4) |
 * central_nonce(4) | rx_delay(1) | tx_power(1, 0 = none) | reserved(3). */
#define P2P_JOIN_REQ_VALUE_LEN    14
#define P2P_JOIN_ACCEPT_VALUE_LEN 13
#define P2P_JOIN_REQ_LEN          (TWR_HDR_LEN + 3 + P2P_JOIN_REQ_VALUE_LEN + TWR_TAG_LEN) /* 39 */
#define P2P_JOIN_ACCEPT_LEN (TWR_HDR_LEN + 3 + P2P_JOIN_ACCEPT_VALUE_LEN + TWR_TAG_LEN)    /* 38 */

/* Join retry tuning (§5.2 / §5.3), shared with tests/p2p_logic.
 *
 * The window is a deadline, not a hint: the retry wait is capped against it
 * (p2p_join_retry_delay_ms), because the duty-cycle wait it competes with can
 * be as long as APP_RADIO_DUTY_WINDOW_MS. */
#define P2P_JOIN_BOOT_WINDOW_MS  (120 * 1000)
#define P2P_JOIN_RETRY_JITTER_MS 2000

/* JoinAccept tx_power: 0 = no assignment (the node keeps p2p_tx_power), else
 * within the config parameter's own range. */
#define P2P_TX_POWER_MIN_DBM 2  /* mirrors app_config.yml::p2p_tx_power min */
#define P2P_TX_POWER_MAX_DBM 22 /* ... and its max */

/* A parsed, validated JoinAccept (p2p_parse_join_accept()). */
struct p2p_join_accept {
	uint32_t net_id;
	uint32_t central_nonce;
	uint8_t rx_delay_s;
	bool tx_power_assigned;
	int8_t tx_power_dbm; /* valid iff tx_power_assigned */
};

/* One 0x91 entry (p2p_tlv_next()). `val` points into the parsed buffer. */
struct p2p_tlv {
	uint8_t cmd;
	uint8_t len;
	const uint8_t *val;
};

/* p2p_replay_check() verdicts on an authenticated gateway frame (plan §4). */
#define P2P_RX_REPEAT 0 /* == last: a retransmission -- re-ACK, never re-deliver */
#define P2P_RX_FRESH  1 /* > last: new */

/* Raw-LoRa point-to-point transport (#118, doc/p2p.md), on TOWER frames since
 * P1 of doc/plan/470. A drop-in alternative to app_radio_lrw for deployments
 * without LoRaWAN infrastructure, selected at boot by `radio-mode p2p` via the
 * app_radio facade. It talks raw LoRa through the Zephyr drivers/lora API.
 *
 * The payload layer is reused unchanged: an uplink is the exact LoRaWAN fPort
 * payload behind a 2 B data envelope (0x81 | port), sealed in a TOWER Data
 * frame under the session_key the join derived from the device's LoRaWAN OTAA
 * AppKey (plan §6.2). Link control (Capabilities, Hello, LinkCheck, Time,
 * Detach, RejoinReq and the join itself) travels in the 0x91 TLV envelope.
 *
 * A confirmed uplink is one TOWER confirmed send: up to 3 byte-identical
 * transmissions, each followed by an ACK window sized from the SF. An ACK with
 * PENDING keeps the receiver on for one gateway Data frame: a command (0x81
 * port 86) or a control answer (0x91), auto-ACKed when it is confirmed.
 *
 * On first start with no persisted pairing the device joins (JoinRequest, a
 * 1 s RX1 for the JoinAccept) and persists net_id/session_key; a pairing
 * survives reboots. app_radio_p2p_is_ready() goes true once paired; an
 * unanswered boot window (§5.2, 120 s) hands the episode to the slow backoff
 * curve (§7) and the node keeps looking. The optional listen mode
 * (CONFIG_SHELL) logs the TOWER headers heard on the channel.
 */

/* Join/session state (#118 phase 2, doc/p2p.md §5.3) -- public so `ats radio
 * status` can report it via struct app_radio_p2p_info below. */
enum p2p_link_state {
	P2P_LINK_UNPAIRED, /* no valid pairing in NVS; not currently joining */
	P2P_LINK_JOINING,  /* join attempts in progress: boot window, then slow backoff */
	P2P_LINK_PAIRED,   /* net_id/session_key valid, data plane live */
};

/* Configure the radio from the p2p config group and set up the work queue.
 * Returns 0 or a negative errno. */
int app_radio_p2p_init(void);

/* Boot-time bring-up. Refuses outright, and logs an error, if `radio_appkey`
 * is all-zero: it is the root key for the whole transport, so an all-zero one
 * is a publicly known key and joining under it is forgeable by anyone in
 * range (doc/p2p.md §4). This is checked before the paired shortcut below, so
 * a device re-enabled into `radio-mode p2p` after a factory_reset -- which
 * wipes radio_appkey but leaves the persisted pairing intact -- refuses rather
 * than resuming a session it can never renew (doc/p2p.md §7).
 *
 * Otherwise: if already paired (persisted NVS state from a prior
 * join), mark the radio ready and kick the report cadence immediately --
 * unlike app_radio_lrw_join() on the radio facade, an existing pairing is treated
 * as sufficient, so a normal power cycle never wastes a JoinRequest
 * (doc/p2p.md §7). Otherwise starts the join handshake (#118 phase 2,
 * doc/p2p.md §5.3); the ready callback fires later, only once JoinAccept
 * succeeds. See app_radio_p2p_rejoin() below for forcing a fresh join on demand. */
void app_radio_p2p_start(void);

/* True while the data plane is live: paired and started -- immediately if NVS
 * already had a valid pairing, otherwise only after the join handshake (#118
 * phase 2) completes. False again while a self-heal / RejoinRequest join runs
 * or after a Detach, so the report cadence skips instead of sending into a
 * session that is being replaced. */
bool app_radio_p2p_is_ready(void);

/* Link state in the common app_radio terms (see enum app_radio_state). */
enum app_radio_state app_radio_p2p_get_state(void);

/* Fixed application-payload budget for one frame (P2P_MAX_BODY: the frame MTU
 * minus the TOWER header, tag and envelope). app_compose() bin-packs telemetry
 * groups against this. */
uint8_t app_radio_p2p_get_max_payload(void);

/* The P2P TX backend of the common scheduler (app_radio, doc/plan/460 F4):
 * sends one frame -- answers on port 85, alarms on 3, both confirmed;
 * telemetry on 2, confirmed on the N-th report. */
extern const struct app_radio_backend app_radio_p2p_backend;

/* Register the link-ready kick fired by app_radio_p2p_start() so app_report can
 * begin the cadence. NULL clears it. */
void app_radio_p2p_register_ready_cb(void (*cb)(void));

/* Stop P2P radio activity ahead of a deep-sleep poweroff. */
void app_radio_p2p_suspend(void);

/* Snapshot for `ats radio status` (#118) -- the P2P analogue of struct
 * app_radio_lrw_info. */
struct app_radio_p2p_info {
	enum p2p_link_state link_state;
	uint32_t addr;   /* this node's TOWER address, low32(DevEUI) */
	uint32_t net_id; /* the gateway address; 0 pre-pairing */
	uint8_t rx_delay_s;
	uint8_t sf; /* the spreading factor the radio is tuned to */
	/* Session TX power: the value the central assigned in JoinAccept when
	 * tx_power_assigned, otherwise the node's own p2p_tx_power config. */
	bool tx_power_assigned;
	int8_t tx_power_dbm;
	uint32_t fcnt;              /* next TX counter */
	uint32_t dev_nonce;         /* JoinRequest anti-replay counter, never resets */
	uint32_t gw_last;           /* highest gateway counter accepted this session */
	uint32_t ack_retry_pending; /* 1: a confirmed frame awaits its retry (app_radio) */
	/* The gateway's RSSI of this node's last acknowledged uplink (the TOWER
	 * ACK carries no SNR); false until the first ACK of the session. */
	int8_t last_ack_rssi;
	bool last_ack_valid;
	/* The last LinkCheckAns: the central's view of this node's uplink. */
	bool lc_valid;
	int8_t lc_rssi;
	int8_t lc_snr;
	int8_t lc_margin;
	uint8_t lc_gw_count;
	/* False means radio_appkey is all-zero, i.e. the device has no root key
	 * for P2P at all and app_radio_p2p_start()/app_radio_p2p_rejoin() refuse to bring
	 * the radio up (doc/p2p.md §4). Without this, such a device is
	 * indistinguishable from a plain UNPAIRED one on the bench. */
	bool app_key_set;
};

/* Fill `info` with the current pairing/session snapshot. Always succeeds. */
void app_radio_p2p_get_info(struct app_radio_p2p_info *info);

/* Force a fresh join handshake RIGHT NOW, even if currently PAIRED. Subject
 * to the same all-zero `radio_appkey` refusal as app_radio_p2p_start() -- the shell
 * is not a way around it --
 * unlike app_radio_p2p_start(), an existing pairing is not treated as sufficient.
 * A successful JoinAccept overwrites the old pairing via pairing_persist(),
 * so this never needs a reboot or NVS wipe (contrast with app_radio_p2p_unjoin()
 * below). This is what makes `join` (shell) and lrw_join (NFC / downlink,
 * via app_radio_rejoin()) genuinely force a fresh session on both radio stacks. */
void app_radio_p2p_rejoin(void);

/* app_radio_reset_link() on P2P: clear the persisted pairing (RAM + NVS) ahead
 * of a reset-tier reboot; the dev_nonce and frame counter are kept (see
 * app_radio_p2p_unjoin()). */
void app_radio_p2p_forget_pairing(void);

#if defined(CONFIG_SHELL)
/* Bench-rig reference receiver (doc/p2p.md §14): enable=true reconfigures the
 * radio for continuous RX and logs the TOWER header, RSSI and SNR of every
 * frame heard (it has no key to open them with). enable=false stops it and
 * returns to TX config. Returns 0 or a negative errno. Uplinks are refused
 * with -EBUSY while listening. */
int app_radio_p2p_listen(bool enable);

/* Clear the persisted pairing (net_id/session_key/rx_delay) so the next boot
 * starts a fresh JoinRequest. NEVER touches the dev_nonce anti-replay counter
 * -- see dnonce_persist()'s comment in app_radio_p2p.c for why a re-join must
 * never risk presenting a dev_nonce the central already saw. Mirrors
 * app_radio_lrw_reset_nvm(): settings only, reboot required to take effect.
 * Returns 0 or a negative errno. */
int app_radio_p2p_unjoin(void);

/* Debug: make the next `count` valid ACKs appear lost (as if the gateway never
 * replied), to exercise the net-layer repetitions and app_radio's retry
 * deterministically without a real RF outage. 0 disables the injection. */
void app_radio_p2p_debug_drop_acks(uint32_t count);

/* Debug: build (frame + encrypt) one telemetry frame under the CURRENT
 * session state WITHOUT transmitting or advancing the frame counter -- lets
 * a bench tech inspect the exact bytes that would go on air. Runs on the P2P
 * work queue like a real send (app_compose.c's "solely on m_work_q"
 * invariant, mirrored here for P2P's own queue). `*more` reports whether
 * app_compose has additional frames pending (call again to drain them, same
 * idiom as app_compose_ex()/app_radio_lrw_run_on_work_q()). Returns 0, -ENOTCONN
 * if not paired yet, -ENOMEM if `out_size` is too small, or a negative
 * errno. */
int app_radio_p2p_debug_compose(uint8_t *out, size_t out_size, size_t *out_len, bool *more);
#endif

#if defined(CONFIG_ZTEST)
/* Pure decision-logic helpers, internal to app_radio_p2p.c (static in the firmware),
 * exposed with external linkage for tests/p2p_logic only. Not part of the
 * runtime API -- do not call from firmware. */
uint32_t p2p_toa_ms(int sf, uint8_t payload_len);
uint32_t rx1_preamble_catch_ms(int sf);
uint32_t p2p_rx1_timeout_ms(int sf, uint8_t expected_frame_len);
uint32_t p2p_twr_window_ms(int sf, uint8_t frame_len);
int p2p_replay_check(uint32_t last, uint32_t counter);
int p2p_tlv_next(const uint8_t *buf, size_t len, size_t *off, struct p2p_tlv *tlv);
int p2p_parse_join_accept(const uint8_t *val, size_t len, uint32_t hdr_src,
			  struct p2p_join_accept *out);
uint32_t p2p_time_at(uint32_t unix_s, uint8_t frac, int64_t elapsed_ms);
int64_t p2p_join_retry_delay_ms(bool slow, int64_t elapsed_ms, int64_t duty_wait_ms,
				uint32_t backoff_ms, uint32_t jitter_ms);
void twr_hdr_put(uint8_t out[TWR_HDR_LEN], const struct twr_hdr *h);
int twr_hdr_get(const uint8_t *frame, size_t frame_len, struct twr_hdr *h);
void twr_nonce(uint8_t nonce[TWR_NONCE_LEN], uint32_t src, uint32_t counter);
int twr_seal(const uint8_t key[16], const struct twr_hdr *h, const uint8_t *pt, size_t pt_len,
	     uint8_t *frame, size_t frame_size, size_t *frame_len);
int twr_open(const uint8_t key[16], const uint8_t *frame, size_t frame_len, struct twr_hdr *h,
	     uint8_t *pt, size_t pt_size, size_t *pt_len);
int twr_parse_ack(const uint8_t *pt, size_t pt_len, struct twr_ack *ack);
void p2p_test_join_setup(int cfg_sf);
void p2p_test_link_reset(void);
void p2p_test_join_stop(void);
void p2p_test_set_session_tx_power(bool assigned, int8_t dbm);
void p2p_test_join_step(void);
void p2p_test_join_arm_retry(int64_t ms);
void p2p_test_set_paired(void);
void p2p_test_set_session(uint32_t net_id, const uint8_t key[P2P_KEY_LEN]);
void p2p_test_tx_reset(void);
void p2p_test_set_link(enum p2p_link_state state, bool started, bool slow, bool disabled);
void p2p_test_note_downlink(int16_t rssi, int8_t snr);
void p2p_test_join_restart(void);
int64_t p2p_test_join_pending_ms(void);
void p2p_test_get_join(uint8_t *sf, bool *slow, uint8_t *rejoin, enum p2p_link_state *state);
void p2p_test_set_join_started_at(int64_t at_ms);
void p2p_test_derive_join_key(uint8_t out[P2P_KEY_LEN]);
void p2p_test_derive_session_key(uint32_t dev_nonce, uint32_t central_nonce,
				 uint8_t out[P2P_KEY_LEN]);
int p2p_test_build_join_request(uint32_t dev_nonce, uint8_t out[P2P_JOIN_REQ_LEN]);
int p2p_test_open_join_accept(uint32_t dev_nonce, const uint8_t *frame, size_t len);
void p2p_test_set_dev_nonce(uint32_t v);
uint32_t p2p_test_get_dev_nonce(void);
void p2p_test_set_fcnt(uint32_t next, uint32_t reserved);
uint32_t p2p_test_get_fcnt(void);
int p2p_test_fcnt_next(uint32_t *counter_out);
uint32_t p2p_test_get_gw_last(void);
int p2p_test_uplink(const uint8_t *pt, size_t pt_len, bool confirmed);
void p2p_test_ctrl_downlink(const uint8_t *val, size_t len);
uint32_t p2p_test_ctrl_pending(void);
void p2p_test_ctrl_run(void);
void p2p_test_set_time_req(uint32_t counter, int64_t done_ms);
#endif /* defined(CONFIG_ZTEST) */

#ifdef __cplusplus
}
#endif

#endif /* APP_RADIO_P2P_H_ */
