/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native unit tests for app_radio_p2p.c on the TOWER wire (doc/plan/470): the
 * time-on-air and window arithmetic, the TOWER frame codec and the join, both
 * pinned byte for byte to the shared KATs (tests/ccm/tower_frame_kat.json,
 * tower_join_kat.json), the join retry policy, and the exchange -- confirmed
 * repetitions, the ACK, a PENDING downlink and its ACK, the 0x81/0x91
 * envelopes. app_radio_p2p.c is compiled directly, with a fake LoRa device
 * (src/emul_lora.c) and thin stubs (src/stubs.c); its internals are reached
 * via the CONFIG_ZTEST hooks in app_radio_p2p.h. The gateway side of every
 * exchange is played by gw_respond() below, answering from the fake radio.
 */

#include "app_ccm.h"
#include "app_config.h"
#include "app_radio.h"
#include "app_radio_p2p.h"
#include "emul_lora.h"
#include "tower_join_kat.h"
#include "tower_kat.h"

#include <zephyr/ztest.h>
#include <zephyr/sys/byteorder.h>

#include <errno.h>
#include <string.h>

/* src/stubs.c */
extern bool p2p_test_time_wanted;
extern int p2p_test_time_events;
extern int p2p_test_air_begins;
extern int p2p_test_air_ends;
extern uint32_t g_test_network_time;
extern int64_t test_duty_wait_ms;
extern uint32_t test_duty_charges;
extern uint64_t test_duty_charged_ms;
extern int test_settings_save_ret;
extern int p2p_test_announce_calls;
extern int p2p_test_tx_kick_calls;
extern int p2p_test_link_ups;
extern int p2p_test_link_ok_calls;
extern int p2p_test_link_fail_calls;
extern int16_t p2p_test_dl_rssi;
extern int8_t p2p_test_dl_snr;
extern int p2p_test_dl_calls;
extern uint8_t p2p_test_ul_margin;
extern uint8_t p2p_test_ul_gw_count;
extern int p2p_test_downlinks;
extern uint8_t p2p_test_downlink_buf[80];
extern size_t p2p_test_downlink_len;

/* The tower_frame_kat.json session key; the join KAT's app_key is the same. */
static const uint8_t tower_kat_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
					  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

/* ---- the gateway --------------------------------------------------------- */

/* A TOWER gateway on the other end of the fake radio: it opens every frame the
 * node sends under the session key, records it, and answers a confirmed Data
 * frame with an ACK -- and, when a downlink is set, PENDING and that Data
 * frame. What it pushes reaches the node's next receive window. */
static struct {
	uint32_t net_id;
	uint8_t key[16];
	uint32_t counter; /* its last TX counter */
	bool ack;
	uint8_t ack_from;     /* ACK from this transmission of a frame on (0/1 = the first) */
	int32_t acked_offset; /* ACK a counter this far off the uplink's */
	bool bad_tag;         /* break the ACK's tag */
	const uint8_t *dl;
	size_t dl_len;
	bool dl_confirmed;
	uint32_t dl_counter; /* 0 = its next counter */

	/* What it heard. */
	uint32_t uplinks;
	uint8_t reps; /* transmissions of the current counter */
	struct twr_hdr up_hdr;
	uint8_t up_pt[TWR_PAYLOAD_MAX];
	size_t up_len;
	uint32_t node_acks;
	struct twr_hdr node_ack_hdr;
	struct twr_ack node_ack;
	uint8_t ack_frame[P2P_FRAME_MAX]; /* the last ACK it pushed */
	size_t ack_frame_len;
} gw;

static void gw_push(uint8_t type, uint8_t flags, uint32_t counter, const uint8_t *pt, size_t len,
		    int16_t rssi, int8_t snr)
{
	const struct twr_hdr h = {.type = type,
				  .flags = flags,
				  .src = gw.net_id,
				  .dest = TOWER_KAT_NODE_ADDR,
				  .counter = counter};
	uint8_t frame[P2P_FRAME_MAX];
	size_t flen;

	zassert_ok(twr_seal(gw.key, &h, pt, len, frame, sizeof(frame), &flen));
	if (type == TWR_TYPE_ACK) {
		if (gw.bad_tag) {
			frame[flen - 1] ^= 0x01;
		}
		memcpy(gw.ack_frame, frame, flen);
		gw.ack_frame_len = flen;
	}
	test_lora_rx_push(frame, flen, rssi, snr);
}

static void gw_respond(const uint8_t *frame, uint32_t len)
{
	struct twr_hdr h;
	uint8_t pt[TWR_PAYLOAD_MAX];
	size_t pt_len;

	if (twr_open(gw.key, frame, len, &h, pt, sizeof(pt), &pt_len)) {
		return; /* a JoinRequest (join_key), or a frame of another session */
	}
	if (h.type == TWR_TYPE_ACK) {
		gw.node_acks++;
		gw.node_ack_hdr = h;
		zassert_ok(twr_parse_ack(pt, pt_len, &gw.node_ack));
		return;
	}
	gw.reps = (gw.uplinks > 0 && h.counter == gw.up_hdr.counter) ? gw.reps + 1 : 1;
	gw.uplinks++;
	gw.up_hdr = h;
	memcpy(gw.up_pt, pt, pt_len);
	gw.up_len = pt_len;

	if (!(h.flags & TWR_FLAG_CONFIRMED) || !gw.ack || gw.reps < gw.ack_from) {
		return;
	}

	uint8_t ack[TWR_ACK_PAYLOAD_LEN];

	sys_put_le32(h.counter + gw.acked_offset, &ack[0]);
	ack[4] = (uint8_t)(int8_t)-57;
	ack[5] = gw.dl_len ? TWR_ACK_PENDING : 0;
	gw_push(TWR_TYPE_ACK, 0, ++gw.counter, ack, sizeof(ack), -60, 8);

	if (gw.dl_len) {
		uint32_t c = gw.dl_counter ? gw.dl_counter : ++gw.counter;

		gw_push(TWR_TYPE_DATA, gw.dl_confirmed ? TWR_FLAG_CONFIRMED : 0, c, gw.dl,
			gw.dl_len, -61, 7);
	}
}

/* A live session with the frame KAT's gateway and key (tower_kat.h): the node's
 * frames are then the KAT's own, and the gateway's first ACK is its counter
 * 100, as in the KAT. */
static void paired(void)
{
	p2p_test_set_session(TOWER_KAT_GW_ADDR, tower_kat_key);
	p2p_test_tx_reset();
	gw.net_id = TOWER_KAT_GW_ADDR;
	memcpy(gw.key, tower_kat_key, sizeof(gw.key));
	gw.counter = 99;
	test_lora_responder = gw_respond;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	/* The join KAT's identity: DevEUI 5876070000000413, address 1043. */
	memcpy(g_app_config.radio_appkey, tower_join_kat_app_key,
	       sizeof(g_app_config.radio_appkey));
	memcpy(g_app_config.radio_deveui, tower_join_kat_dev_eui,
	       sizeof(g_app_config.radio_deveui));
	g_app_config.p2p_modulation = APP_CONFIG_P2P_MODULATION_LORA;
	g_app_config.p2p_frequency = 869525000;
	g_app_config.p2p_tx_power = 14;
	g_app_config.radio_link_check_interval = 5;

	test_lora_reset();
	test_duty_wait_ms = 0;
	test_settings_save_ret = 0;
	p2p_test_time_wanted = false;

	p2p_test_join_setup(7);
	p2p_test_join_stop();
	p2p_test_link_reset();
	p2p_test_tx_reset();
	p2p_test_set_link(P2P_LINK_UNPAIRED, false, false, false);
	p2p_test_set_session_tx_power(false, 0);
	p2p_test_set_fcnt(1, 256);
	p2p_test_set_dev_nonce(TOWER_JOIN_KAT_DEV_NONCE);
	memset(&gw, 0, sizeof(gw));
}

/* ---- time on air and windows ----------------------------------------------- */

/* The SF7 airtimes of the frames on the TOWER wire (plan §3.3): the empty Data
 * frame (header + tag), the ACK, the JoinAccept and JoinRequest, the MTU. */
ZTEST(p2p_logic, test_toa_sf7_frame_airtimes)
{
	const struct {
		uint8_t len;
		uint32_t ms;
	} cases[] = {
		{TWR_HDR_LEN + TWR_TAG_LEN, 57}, {TWR_ACK_FRAME_LEN, 67}, {P2P_JOIN_ACCEPT_LEN, 83},
		{P2P_JOIN_REQ_LEN, 83},          {P2P_FRAME_MAX, 175},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		uint32_t got = p2p_toa_ms(7, cases[i].len);

		zassert_equal(got, cases[i].ms, "SF7 ToA(%u B) = %u ms, expected %u", cases[i].len,
			      got, cases[i].ms);
	}

	/* The radio's fixed 4 s TX timeout: the MTU fits it even at SF12. */
	zassert_equal(p2p_toa_ms(12, P2P_FRAME_MAX), 3941);
}

ZTEST(p2p_logic, test_toa_monotonic_in_length)
{
	uint32_t prev = p2p_toa_ms(7, TWR_HDR_LEN + TWR_TAG_LEN);

	for (uint16_t len = TWR_HDR_LEN + TWR_TAG_LEN + 1; len <= 255; len++) {
		uint32_t cur = p2p_toa_ms(7, (uint8_t)len);

		zassert_true(cur >= prev, "ToA not monotonic in length at %u B (%u < %u)", len, cur,
			     prev);
		prev = cur;
	}
}

ZTEST(p2p_logic, test_toa_monotonic_in_sf)
{
	uint32_t prev = p2p_toa_ms(6, 64);

	for (int sf = 7; sf <= 12; sf++) {
		uint32_t cur = p2p_toa_ms(sf, 64);

		zassert_true(cur > prev, "ToA not increasing with SF at SF%d (%u <= %u)", sf, cur,
			     prev);
		prev = cur;
	}
}

/* The JoinAccept window: lora_recv()'s timeout aborts a reception in flight, so
 * it must outlast the preamble-catch budget, the whole 38 B JoinAccept and the
 * 120 ms trailing margin for a late central (F-P2P-2). */
ZTEST(p2p_logic, test_join_accept_window_scales_with_sf)
{
	zassert_equal(p2p_rx1_timeout_ms(7, P2P_JOIN_ACCEPT_LEN), 215u);
	zassert_equal(p2p_rx1_timeout_ms(10, P2P_JOIN_ACCEPT_LEN), 712u);
	zassert_equal(p2p_rx1_timeout_ms(12, P2P_JOIN_ACCEPT_LEN), 2488u);

	uint32_t prev = 0;

	for (int sf = 7; sf <= 12; sf++) {
		uint32_t cur = p2p_rx1_timeout_ms(sf, P2P_JOIN_ACCEPT_LEN);

		zassert_true(cur > prev, "window not increasing at SF%d", sf);
		zassert_true(cur > p2p_toa_ms(sf, P2P_JOIN_ACCEPT_LEN) + rx1_preamble_catch_ms(sf),
			     "SF%d window must outlast the frame plus the catch budget", sf);
		prev = cur;
	}
}

/* TOWER windows (plan §5): turnaround + the awaited frame + 3 symbols + margin,
 * never under 200 ms. The ACK window takes the 28 B ACK, the downlink window
 * after PENDING the MTU. */
ZTEST(p2p_logic, test_twr_windows)
{
	zassert_equal(p2p_twr_window_ms(7, TWR_ACK_FRAME_LEN), 200u, "SF7 ACK: the 200 ms floor");
	zassert_equal(p2p_twr_window_ms(10, TWR_ACK_FRAME_LEN), 479u);
	zassert_equal(p2p_twr_window_ms(12, TWR_ACK_FRAME_LEN), 1786u);
	zassert_equal(p2p_twr_window_ms(7, P2P_FRAME_MAX), 221u);
	zassert_equal(p2p_twr_window_ms(12, P2P_FRAME_MAX), 4080u);

	for (int sf = 7; sf <= 12; sf++) {
		zassert_true(p2p_twr_window_ms(sf, TWR_ACK_FRAME_LEN) >=
				     20 + p2p_toa_ms(sf, TWR_ACK_FRAME_LEN),
			     "SF%d ACK window shorter than turnaround + ACK", sf);
	}
}

/* ---- TOWER frame codec (tower_frame_kat.json) ----------------------------- */

static void tower_kat_hdr(const struct tower_kat *v, struct twr_hdr *h)
{
	h->type = v->frame_type;
	h->flags = v->flags;
	h->src = v->src;
	h->dest = v->dest;
	h->counter = v->counter;
}

ZTEST(p2p_logic, test_tower_seal_matches_the_kat)
{
	for (size_t i = 0; i < ARRAY_SIZE(tower_kat); i++) {
		const struct tower_kat *v = &tower_kat[i];
		struct twr_hdr h;
		uint8_t nonce[TWR_NONCE_LEN];
		uint8_t frame[P2P_FRAME_MAX];
		size_t len = 0;

		tower_kat_hdr(v, &h);
		twr_nonce(nonce, v->src, v->counter);
		zassert_mem_equal(nonce, v->nonce, TWR_NONCE_LEN, "%s: nonce", v->name);
		zassert_ok(twr_seal(tower_kat_key, &h, v->plaintext, v->plaintext_len, frame,
				    sizeof(frame), &len),
			   "%s: seal", v->name);
		zassert_equal(len, v->frame_len, "%s: length %zu", v->name, len);
		zassert_mem_equal(frame, v->frame, v->frame_len, "%s: frame bytes", v->name);
	}
}

ZTEST(p2p_logic, test_tower_open_matches_the_kat)
{
	for (size_t i = 0; i < ARRAY_SIZE(tower_kat); i++) {
		const struct tower_kat *v = &tower_kat[i];
		struct twr_hdr h;
		uint8_t pt[P2P_FRAME_MAX];
		size_t pt_len = 0xffff;

		zassert_ok(twr_open(tower_kat_key, v->frame, v->frame_len, &h, pt, sizeof(pt),
				    &pt_len),
			   "%s: open", v->name);
		zassert_equal(h.type, v->frame_type, "%s: type", v->name);
		zassert_equal(h.flags, v->flags, "%s: flags", v->name);
		zassert_equal(h.src, v->src, "%s: src", v->name);
		zassert_equal(h.dest, v->dest, "%s: dest", v->name);
		zassert_equal(h.counter, v->counter, "%s: counter", v->name);
		zassert_equal(pt_len, v->plaintext_len, "%s: plaintext length", v->name);
		if (pt_len > 0) {
			zassert_mem_equal(pt, v->plaintext, pt_len, "%s: plaintext", v->name);
		}
	}
}

ZTEST(p2p_logic, test_tower_header_layout)
{
	/* ver_type = version << 5 | type; the other fields little-endian. */
	const struct twr_hdr h = {.type = TWR_TYPE_ACK,
				  .flags = 0x5a,
				  .src = 0x04030201,
				  .dest = 0x08070605,
				  .counter = 0x0c0b0a09};
	const uint8_t want[TWR_HDR_LEN] = {0x21, 0x5a, 0x01, 0x02, 0x03, 0x04, 0x05,
					   0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c};
	uint8_t out[TWR_HDR_LEN];

	twr_hdr_put(out, &h);
	zassert_mem_equal(out, want, TWR_HDR_LEN);
	zassert_equal(tower_kat[0].frame[0], 0x20, "KAT data frame: ver 1, type 0");
	zassert_equal(tower_kat[2].frame[0], 0x21, "KAT ack frame: ver 1, type 1");
}

ZTEST(p2p_logic, test_tower_open_rejects_a_bad_frame)
{
	const struct tower_kat *v = &tower_kat[1]; /* uplink_confirmed */
	uint8_t frame[P2P_FRAME_MAX];
	uint8_t pt[P2P_FRAME_MAX];
	struct twr_hdr h;
	size_t pt_len;

	/* A flipped bit anywhere -- header (AAD), ciphertext or tag -- fails auth. */
	const size_t spots[] = {0, 1, 5, 13, TWR_HDR_LEN, v->frame_len - 1};

	for (size_t i = 0; i < ARRAY_SIZE(spots); i++) {
		memcpy(frame, v->frame, v->frame_len);
		frame[spots[i]] ^= (spots[i] == 0) ? 0x01 : 0x80;
		zassert_equal(
			twr_open(tower_kat_key, frame, v->frame_len, &h, pt, sizeof(pt), &pt_len),
			-EBADMSG, "byte %zu", spots[i]);
	}

	/* Another version is not a TOWER v1 frame, whatever its tag. */
	memcpy(frame, v->frame, v->frame_len);
	frame[0] = (2 << 5) | TWR_TYPE_DATA;
	zassert_equal(twr_open(tower_kat_key, frame, v->frame_len, &h, pt, sizeof(pt), &pt_len),
		      -EPROTO);
	zassert_equal(twr_hdr_get(frame, v->frame_len, &h), -EPROTO);

	/* Shorter than header + tag, or truncated: never authentic. */
	zassert_equal(twr_hdr_get(v->frame, TWR_HDR_LEN + TWR_TAG_LEN - 1, &h), -EMSGSIZE);
	zassert_not_ok(
		twr_open(tower_kat_key, v->frame, v->frame_len - 1, &h, pt, sizeof(pt), &pt_len));

	/* A wrong key. */
	uint8_t key[16];

	memcpy(key, tower_kat_key, sizeof(key));
	key[15] ^= 1;
	zassert_equal(twr_open(key, v->frame, v->frame_len, &h, pt, sizeof(pt), &pt_len), -EBADMSG);

	/* No room for the plaintext. */
	zassert_not_ok(twr_open(tower_kat_key, v->frame, v->frame_len, &h, pt, v->plaintext_len - 1,
				&pt_len));
}

ZTEST(p2p_logic, test_tower_seal_refuses_a_short_buffer)
{
	const struct tower_kat *v = &tower_kat[6]; /* uplink_max_payload */
	uint8_t frame[P2P_FRAME_MAX];
	uint8_t big[TWR_PAYLOAD_MAX + 1] = {0};
	struct twr_hdr h;
	size_t len;

	tower_kat_hdr(v, &h);
	zassert_not_ok(twr_seal(tower_kat_key, &h, v->plaintext, v->plaintext_len, frame,
				v->frame_len - 1, &len));
	zassert_ok(twr_seal(tower_kat_key, &h, v->plaintext, v->plaintext_len, frame, v->frame_len,
			    &len));

	/* The MTU is a wire limit, not only the caller's buffer. */
	zassert_ok(twr_seal(tower_kat_key, &h, big, TWR_PAYLOAD_MAX, frame, sizeof(frame), &len));
	zassert_equal(len, P2P_FRAME_MAX);
	zassert_equal(twr_seal(tower_kat_key, &h, big, sizeof(big), frame, 255, &len), -EMSGSIZE);
}

ZTEST(p2p_logic, test_tower_ack_payload)
{
	struct twr_ack ack;
	uint8_t pt[P2P_FRAME_MAX];
	struct twr_hdr h;
	size_t pt_len;

	/* ack_no_pending: acked 2, rssi -57, no PENDING. */
	zassert_ok(twr_open(tower_kat_key, tower_kat[2].frame, tower_kat[2].frame_len, &h, pt,
			    sizeof(pt), &pt_len));
	zassert_equal(h.type, TWR_TYPE_ACK);
	zassert_equal(h.src, TOWER_KAT_GW_ADDR);
	zassert_equal(h.dest, TOWER_KAT_NODE_ADDR);
	zassert_ok(twr_parse_ack(pt, pt_len, &ack));
	zassert_equal(ack.acked, 2);
	zassert_equal(ack.rssi, -57);
	zassert_false(ack.pending);

	/* ack_pending: PENDING set, a downlink follows. */
	zassert_ok(twr_open(tower_kat_key, tower_kat[3].frame, tower_kat[3].frame_len, &h, pt,
			    sizeof(pt), &pt_len));
	zassert_ok(twr_parse_ack(pt, pt_len, &ack));
	zassert_equal(ack.acked, 2);
	zassert_true(ack.pending);

	/* The minimum ACK carries only the acked counter; appended fields are
	 * ignored. */
	const uint8_t bare[4] = {0x07, 0x00, 0x00, 0x80};
	const uint8_t longer[9] = {0x07, 0x00, 0x00, 0x80, 0xc4, 0x01, 0xaa, 0xbb, 0xcc};

	zassert_ok(twr_parse_ack(bare, sizeof(bare), &ack));
	zassert_equal(ack.acked, 0x80000007);
	zassert_equal(ack.rssi, 0);
	zassert_false(ack.pending);
	zassert_equal(twr_parse_ack(bare, 3, &ack), -EMSGSIZE);
	zassert_ok(twr_parse_ack(longer, sizeof(longer), &ack));
	zassert_equal(ack.rssi, -60);
	zassert_true(ack.pending);
}

/* ---- replay lane, TLV list, JoinAccept, TimeAns --------------------------- */

/* Plan §4: above the lane is fresh, equal a retransmission (re-ACK, never
 * re-deliver), below it -- and the reserved counter 0 -- a replay. */
ZTEST(p2p_logic, test_replay_check)
{
	zassert_equal(p2p_replay_check(0, 0), -EALREADY, "counter 0 is reserved");
	zassert_equal(p2p_replay_check(0, 1), P2P_RX_FRESH);
	zassert_equal(p2p_replay_check(5, 6), P2P_RX_FRESH);
	zassert_equal(p2p_replay_check(5, 5), P2P_RX_REPEAT);
	zassert_equal(p2p_replay_check(5, 4), -EALREADY);
	zassert_equal(p2p_replay_check(5, 0), -EALREADY);
	zassert_equal(p2p_replay_check(UINT32_MAX - 1, UINT32_MAX), P2P_RX_FRESH);
}

ZTEST(p2p_logic, test_tlv_walk)
{
	const uint8_t list[] = {0x10, 0x04, 0xb0, 0x05, 0x0c, 0x01, /* LinkCheckAns */
				0x77, 0x00,                         /* unknown, empty */
				0x03, 0x01, 0x02};                  /* Detach */
	struct p2p_tlv t;
	size_t off = 0;

	zassert_equal(p2p_tlv_next(list, sizeof(list), &off, &t), 1);
	zassert_equal(t.cmd, P2P_CTRL_LINK_CHECK);
	zassert_equal(t.len, 4);
	zassert_equal(t.val, &list[2]);
	zassert_equal(p2p_tlv_next(list, sizeof(list), &off, &t), 1);
	zassert_equal(t.cmd, 0x77);
	zassert_equal(t.len, 0);
	zassert_equal(p2p_tlv_next(list, sizeof(list), &off, &t), 1);
	zassert_equal(t.cmd, P2P_CTRL_DETACH);
	zassert_equal(t.val[0], 0x02);
	zassert_equal(p2p_tlv_next(list, sizeof(list), &off, &t), 0, "end of the list");
	zassert_equal(off, sizeof(list));

	/* A value that runs past the list, and a lone cmd byte, are malformed. */
	off = 0;
	zassert_equal(p2p_tlv_next(list, 5, &off, &t), -EBADMSG);
	off = 0;
	zassert_equal(p2p_tlv_next(list, 1, &off, &t), -EBADMSG);
	off = 0;
	zassert_equal(p2p_tlv_next(list, 0, &off, &t), 0, "an empty list");
}

/* The JoinAccept value of the join KAT: net_id, central_nonce, rx_delay 1 s,
 * tx_power 14 dBm. */
static const uint8_t *kat_ja_value(void)
{
	/* plaintext = 0x91 | 0x08 | 0x0d | value(13) */
	return &tower_join_kat_join_accept_plaintext[3];
}

ZTEST(p2p_logic, test_join_accept_parse)
{
	struct p2p_join_accept ja;
	uint8_t v[16];

	zassert_ok(p2p_parse_join_accept(kat_ja_value(), P2P_JOIN_ACCEPT_VALUE_LEN,
					 TOWER_JOIN_KAT_NET_ID, &ja));
	zassert_equal(ja.net_id, TOWER_JOIN_KAT_NET_ID);
	zassert_equal(ja.central_nonce, TOWER_JOIN_KAT_CENTRAL_NONCE);
	zassert_equal(ja.rx_delay_s, 1);
	zassert_true(ja.tx_power_assigned);
	zassert_equal(ja.tx_power_dbm, 14);

	/* tx_power: 0 assigns nothing; an out-of-range one is ignored, and the
	 * JoinAccept still pairs; both ends of the range are taken. */
	const struct {
		uint8_t dbm;
		bool assigned;
	} powers[] = {{0, false},
		      {1, false},
		      {P2P_TX_POWER_MIN_DBM, true},
		      {8, true},
		      {P2P_TX_POWER_MAX_DBM, true},
		      {23, false},
		      {0xff, false}};

	for (size_t i = 0; i < ARRAY_SIZE(powers); i++) {
		memcpy(v, kat_ja_value(), P2P_JOIN_ACCEPT_VALUE_LEN);
		v[9] = powers[i].dbm;
		zassert_ok(p2p_parse_join_accept(v, P2P_JOIN_ACCEPT_VALUE_LEN,
						 TOWER_JOIN_KAT_NET_ID, &ja),
			   "%u dBm must still pair", powers[i].dbm);
		zassert_equal(ja.tx_power_assigned, powers[i].assigned, "%u dBm", powers[i].dbm);
		zassert_equal(ja.tx_power_dbm, powers[i].assigned ? (int8_t)powers[i].dbm : 0);
	}

	/* Refused: the frame's src is not the net_id it assigns; a reserved
	 * net_id; an rx_delay outside 1..15 s; a short value. */
	memcpy(v, kat_ja_value(), P2P_JOIN_ACCEPT_VALUE_LEN);
	zassert_equal(p2p_parse_join_accept(v, P2P_JOIN_ACCEPT_VALUE_LEN, 0x12345678, &ja),
		      -EBADMSG);
	sys_put_le32(TWR_ADDR_NONE, v);
	zassert_equal(p2p_parse_join_accept(v, P2P_JOIN_ACCEPT_VALUE_LEN, TWR_ADDR_NONE, &ja),
		      -EBADMSG);
	sys_put_le32(TWR_ADDR_BROADCAST, v);
	zassert_equal(p2p_parse_join_accept(v, P2P_JOIN_ACCEPT_VALUE_LEN, TWR_ADDR_BROADCAST, &ja),
		      -EBADMSG);

	const uint8_t delays[] = {0, 16, 0xff};

	for (size_t i = 0; i < ARRAY_SIZE(delays); i++) {
		memcpy(v, kat_ja_value(), P2P_JOIN_ACCEPT_VALUE_LEN);
		v[8] = delays[i];
		zassert_equal(p2p_parse_join_accept(v, P2P_JOIN_ACCEPT_VALUE_LEN,
						    TOWER_JOIN_KAT_NET_ID, &ja),
			      -EBADMSG, "rx_delay %u", delays[i]);
	}
	v[8] = 15;
	zassert_ok(p2p_parse_join_accept(v, P2P_JOIN_ACCEPT_VALUE_LEN, TOWER_JOIN_KAT_NET_ID, &ja));
	zassert_equal(
		p2p_parse_join_accept(v, P2P_JOIN_ACCEPT_VALUE_LEN - 1, TOWER_JOIN_KAT_NET_ID, &ja),
		-EBADMSG);

	/* A longer value carries appended fields: the tail is ignored. */
	memcpy(v, kat_ja_value(), P2P_JOIN_ACCEPT_VALUE_LEN);
	v[13] = v[14] = v[15] = 0xee;
	zassert_ok(p2p_parse_join_accept(v, sizeof(v), TOWER_JOIN_KAT_NET_ID, &ja));
}

/* TimeAns (plan §8.2): unix + frac/256 s at the TimeReq's TX-done, plus the
 * time since, rounded to the second. */
ZTEST(p2p_logic, test_time_at)
{
	zassert_equal(p2p_time_at(1790000000u, 0, 0), 1790000000u);
	zassert_equal(p2p_time_at(1790000000u, 127, 0), 1790000000u, "496 ms rounds down");
	zassert_equal(p2p_time_at(1790000000u, 128, 0), 1790000001u, "500 ms rounds up");
	zassert_equal(p2p_time_at(1790000000u, 0, 1499), 1790000001u);
	zassert_equal(p2p_time_at(1790000000u, 0, 1500), 1790000002u);
	zassert_equal(p2p_time_at(1790000000u, 0, -5000), 1790000000u, "no negative age");
	zassert_equal(p2p_time_at(1790000000u, 0, 3600000), 1790003600u);
}

/* ---- the join (tower_join_kat.json) ---------------------------------------- */

/* The keys and frames through the firmware's own KDF and builders (the
 * CONFIG_ZTEST hooks), so the vectors pin what the radio path sends rather than
 * a re-spelling of it. */
ZTEST(p2p_logic, test_join_keys_match_the_kat)
{
	uint8_t key[P2P_KEY_LEN];
	uint8_t other[P2P_KEY_LEN];

	p2p_test_derive_join_key(key);
	zassert_mem_equal(key, tower_join_kat_join_key, P2P_KEY_LEN, "join_key");
	p2p_test_derive_session_key(TOWER_JOIN_KAT_DEV_NONCE, TOWER_JOIN_KAT_CENTRAL_NONCE, key);
	zassert_mem_equal(key, tower_join_kat_session_key, P2P_KEY_LEN, "session_key");

	/* Every input moves the session key: dev_nonce, central_nonce, DevEUI. */
	p2p_test_derive_session_key(TOWER_JOIN_KAT_DEV_NONCE + 1, TOWER_JOIN_KAT_CENTRAL_NONCE,
				    other);
	zassert_true(memcmp(key, other, P2P_KEY_LEN) != 0, "dev_nonce is a KDF input");
	p2p_test_derive_session_key(TOWER_JOIN_KAT_DEV_NONCE, TOWER_JOIN_KAT_CENTRAL_NONCE + 1,
				    other);
	zassert_true(memcmp(key, other, P2P_KEY_LEN) != 0, "central_nonce is a KDF input");
	g_app_config.radio_deveui[0] ^= 0x01;
	p2p_test_derive_session_key(TOWER_JOIN_KAT_DEV_NONCE, TOWER_JOIN_KAT_CENTRAL_NONCE, other);
	zassert_true(memcmp(key, other, P2P_KEY_LEN) != 0, "the DevEUI is a KDF input");
	p2p_test_derive_join_key(other);
	zassert_true(memcmp(tower_join_kat_join_key, other, P2P_KEY_LEN) != 0,
		     "the DevEUI is a join_key input");
}

ZTEST(p2p_logic, test_join_request_matches_the_kat)
{
	uint8_t frame[P2P_JOIN_REQ_LEN];

	zassert_equal(sizeof(tower_join_kat_join_request), P2P_JOIN_REQ_LEN, "39 B on the air");
	zassert_ok(p2p_test_build_join_request(TOWER_JOIN_KAT_DEV_NONCE, frame));
	zassert_mem_equal(frame, tower_join_kat_join_request, P2P_JOIN_REQ_LEN,
			  "JoinRequest differs from the shared KAT");
}

ZTEST(p2p_logic, test_join_accept_kat_opens)
{
	uint8_t frame[P2P_JOIN_ACCEPT_LEN];

	zassert_equal(sizeof(tower_join_kat_join_accept), P2P_JOIN_ACCEPT_LEN, "38 B on the air");
	zassert_ok(p2p_test_open_join_accept(TOWER_JOIN_KAT_DEV_NONCE, tower_join_kat_join_accept,
					     P2P_JOIN_ACCEPT_LEN));

	/* The answer to another JoinRequest (counter = its dev_nonce). */
	zassert_not_ok(p2p_test_open_join_accept(TOWER_JOIN_KAT_DEV_NONCE + 1,
						 tower_join_kat_join_accept, P2P_JOIN_ACCEPT_LEN));

	/* Tampered. */
	memcpy(frame, tower_join_kat_join_accept, sizeof(frame));
	frame[TWR_HDR_LEN + 5] ^= 0x10;
	zassert_not_ok(p2p_test_open_join_accept(TOWER_JOIN_KAT_DEV_NONCE, frame, sizeof(frame)));

	/* Addressed to another node. */
	g_app_config.radio_deveui[7] ^= 0x01;
	zassert_not_ok(p2p_test_open_join_accept(TOWER_JOIN_KAT_DEV_NONCE,
						 tower_join_kat_join_accept, P2P_JOIN_ACCEPT_LEN));
}

static void kat_central(const uint8_t *frame, uint32_t len)
{
	if (len == P2P_JOIN_REQ_LEN && memcmp(frame, tower_join_kat_join_request, len) == 0) {
		test_lora_rx_push(tower_join_kat_join_accept, P2P_JOIN_ACCEPT_LEN, -70, 6);
	}
}

/* The whole join on the fake radio: the KAT JoinRequest goes out, the KAT
 * JoinAccept comes back in its window, and the session it leaves sends the
 * KAT's first uplink byte for byte. */
ZTEST(p2p_logic, test_join_end_to_end_under_the_kat)
{
	struct app_radio_p2p_info info;
	uint32_t sends = test_lora_send_count;
	int announces = p2p_test_announce_calls;
	int ups = p2p_test_link_ups;

	p2p_test_join_setup(7);
	test_lora_responder = kat_central;
	p2p_test_join_step();

	zassert_equal(test_lora_send_count, sends + 1, "one JoinRequest");
	zassert_mem_equal(test_lora_sent_frame(sends)->buf, tower_join_kat_join_request,
			  P2P_JOIN_REQ_LEN);
	zassert_true(app_radio_p2p_is_ready(), "paired");
	zassert_equal(p2p_test_announce_calls, announces + 1, "the link-up announces");
	zassert_equal(p2p_test_link_ups, ups + 1);

	app_radio_p2p_get_info(&info);
	zassert_equal(info.link_state, P2P_LINK_PAIRED);
	zassert_equal(info.addr, TOWER_JOIN_KAT_ADDR);
	zassert_equal(info.net_id, TOWER_JOIN_KAT_NET_ID);
	zassert_equal(info.rx_delay_s, 1);
	zassert_true(info.tx_power_assigned);
	zassert_equal(info.tx_power_dbm, 14);
	zassert_equal(info.dev_nonce, TOWER_JOIN_KAT_DEV_NONCE + 1, "the next dev_nonce kept");
	zassert_equal(info.gw_last, 0, "a fresh replay lane");
	/* Capabilities + Hello wait for their jittered 0x91 uplink. */
	zassert_equal(p2p_test_ctrl_pending(), BIT(0) | BIT(1));

	/* The first uplink of the session: counter 1 (0 is reserved), under the
	 * derived session key. Unanswered, it goes three times, byte-identical. */
	p2p_test_tx_reset();
	test_lora_responder = NULL;
	sends = test_lora_send_count;
	zassert_equal(p2p_test_uplink(tower_join_kat_first_uplink_plaintext,
				      sizeof(tower_join_kat_first_uplink_plaintext), true),
		      -ETIMEDOUT);
	zassert_equal(test_lora_send_count, sends + 3);
	for (uint32_t i = 0; i < 3; i++) {
		const struct test_lora_frame *f = test_lora_sent_frame(sends + i);

		zassert_equal(f->len, sizeof(tower_join_kat_first_uplink));
		zassert_mem_equal(f->buf, tower_join_kat_first_uplink,
				  sizeof(tower_join_kat_first_uplink), "transmission %u", i);
	}
}

/* A JoinRequest is TOWER counter dev_nonce, and TOWER reserves 0: a new device
 * starts at 1. */
ZTEST(p2p_logic, test_join_request_counter_skips_zero)
{
	struct twr_hdr h;
	uint32_t sends = test_lora_send_count;

	p2p_test_set_dev_nonce(0);
	p2p_test_join_setup(7);
	p2p_test_join_step();

	zassert_equal(test_lora_send_count, sends + 1);
	zassert_ok(twr_hdr_get(test_lora_last_frame, test_lora_last_len, &h));
	zassert_equal(h.counter, 1);
	zassert_equal(h.src, TOWER_KAT_NODE_ADDR);
	zassert_equal(h.dest, TWR_ADDR_NONE);
	zassert_equal(p2p_test_get_dev_nonce(), 2);
	p2p_test_join_stop();
}

/* ---- join retry policy ------------------------------------------------------ */

ZTEST(p2p_logic, test_join_retry_stays_inside_the_boot_window)
{
	const uint32_t jitter = P2P_JOIN_RETRY_JITTER_MS;

	/* app_radio_duty_wait_ms can return up to APP_RADIO_DUTY_WINDOW_MS. A boot
	 * join that waited that long would be answered long after its 120 s
	 * window closed (bench 2026-09-10 §9): the wait is capped at the window
	 * edge, so the NEXT wake-up is the one that hands over, on time. */
	int64_t d = p2p_join_retry_delay_ms(false, 119000, 3500000, 0, jitter);

	zassert_true(d >= 0, "119 s into a 120 s window is not yet expired");
	zassert_true(d <= 1000, "a 3500 s duty wait must be capped to the 1000 ms left, got %lld",
		     (long long)d);

	zassert_true(p2p_join_retry_delay_ms(false, P2P_JOIN_BOOT_WINDOW_MS, 0, 0, jitter) < 0,
		     "at the window edge the boot join must report the window closed");
	zassert_true(p2p_join_retry_delay_ms(false, 500000, 0, 0, jitter) < 0,
		     "well past the window the boot join must report the window closed");

	/* The slow policy has no window (§7). */
	for (uint8_t a = 0; a < 255; a++) {
		uint32_t base = app_radio_rejoin_backoff_ms(a);

		zassert_true(p2p_join_retry_delay_ms(true, 999999999, 3500000, base, jitter) >= 0,
			     "a slow re-join is never capped by the boot window (attempt %u)", a);
	}

	zassert_true(p2p_join_retry_delay_ms(false, 0, 0, 0, jitter) < (int64_t)jitter,
		     "an unblocked retry goes essentially immediately");
	zassert_equal(p2p_join_retry_delay_ms(false, 1000, 30000, 0, jitter), 30000,
		      "a 30 s duty wait 1 s into the window is not capped");
}

/* A JoinRequest the ledger refuses never reaches the air, so a round that waits
 * only its backoff wakes to be refused again: the wait is the longer of the
 * two. */
ZTEST(p2p_logic, test_slow_retry_waits_for_duty_and_stays_bounded)
{
	const uint32_t jitter = P2P_JOIN_RETRY_JITTER_MS;

	zassert_equal(p2p_join_retry_delay_ms(true, 999999, 1500, 0, jitter), 1500);
	zassert_equal(p2p_join_retry_delay_ms(true, 999999, 90000, 60000, jitter), 90000);
	zassert_equal(p2p_join_retry_delay_ms(true, 999999, 1500, 60000, jitter), 60000);

	int64_t d = p2p_join_retry_delay_ms(true, 999999, APP_RADIO_DUTY_WINDOW_MS - 1000,
					    app_radio_rejoin_backoff_ms(0), jitter);

	zassert_equal(d, APP_RADIO_DUTY_WINDOW_MS - 1000);
	zassert_true(d <= APP_RADIO_DUTY_WINDOW_MS);
}

/* The fleet-spreading jitter may push the wait up but never under the duty
 * wait, or the node wakes into a refusal and burns a backoff step. */
ZTEST(p2p_logic, test_slow_retry_jitter_never_dips_below_the_duty_wait)
{
	const uint32_t base = app_radio_rejoin_backoff_ms(0); /* 60 s */
	const int64_t duty = 90000;

	zassert_equal(app_radio_backoff_jitter_ms(duty, duty, base, 0), duty);
	for (uint32_t r = 0; r <= base / 2; r += 1000) {
		int64_t d = app_radio_backoff_jitter_ms(duty, duty, base, r);

		zassert_true(d >= duty, "draw %u dipped to %lld ms", r, (long long)d);
		zassert_true(d <= duty + (int64_t)(base / 4), "draw %u overshot", r);
	}
	zassert_equal(app_radio_backoff_jitter_ms(base, 0, base, 0), base - base / 4);
	zassert_equal(app_radio_backoff_jitter_ms(base, 0, base, base / 2), base + base / 4);
}

/* §3.3: the SF is a fixed network constant -- every JoinRequest goes out on
 * the configured one, no sweep. */
ZTEST(p2p_logic, test_join_stays_on_the_configured_sf)
{
	uint8_t sf, rejoin;
	bool slow;
	enum p2p_link_state state;

	p2p_test_join_setup(9);
	for (int i = 0; i < 4; i++) {
		uint32_t sends = test_lora_send_count;

		p2p_test_join_step();
		p2p_test_get_join(&sf, &slow, &rejoin, &state);
		zassert_equal(sf, 9, "attempt %d must go out on SF9, got SF%u", i, sf);
		zassert_equal(test_lora_send_count, sends + 1, "attempt %d: one JoinRequest", i);
		zassert_equal(test_lora_last_len, P2P_JOIN_REQ_LEN);
		zassert_false(slow, "inside the boot window: the fast policy");
		zassert_equal(rejoin, 0);
		zassert_equal(state, P2P_LINK_JOINING);
	}
	p2p_test_join_stop();
}

/* A duty bounce puts nothing on the air and spends neither a dev_nonce nor a
 * backoff step. */
ZTEST(p2p_logic, test_join_duty_block_sends_nothing)
{
	uint8_t rejoin;
	uint32_t sends = test_lora_send_count;

	p2p_test_join_setup(7);
	test_duty_wait_ms = APP_RADIO_DUTY_WINDOW_MS / 2;
	p2p_test_join_step();
	test_duty_wait_ms = 0;

	p2p_test_get_join(NULL, NULL, &rejoin, NULL);
	zassert_equal(test_lora_send_count, sends, "nothing on the air");
	zassert_equal(p2p_test_get_dev_nonce(), TOWER_JOIN_KAT_DEV_NONCE, "no dev_nonce spent");
	zassert_equal(rejoin, 0);
	p2p_test_join_stop();
}

/* The boot window ends the fast policy, not the episode: the node keeps
 * looking on the slow curve, one backoff step per unanswered JoinRequest. */
ZTEST(p2p_logic, test_join_window_expiry_switches_to_slow_policy_not_silence)
{
	uint8_t rejoin;
	bool slow;
	enum p2p_link_state state;

	p2p_test_join_setup(7);
	p2p_test_set_join_started_at(k_uptime_get() - P2P_JOIN_BOOT_WINDOW_MS - 1);

	p2p_test_join_step();
	p2p_test_get_join(NULL, &slow, &rejoin, &state);
	zassert_equal(state, P2P_LINK_JOINING, "an expired boot window leaves the episode JOINING");
	zassert_true(slow, "and hands it to the slow policy");
	zassert_equal(rejoin, 1, "the first slow round spent its step, got %u", rejoin);
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_RECONNECT);

	p2p_test_join_step();
	p2p_test_get_join(NULL, &slow, &rejoin, &state);
	zassert_equal(rejoin, 2);
	zassert_true(p2p_test_join_pending_ms() == 0, "the step's retry was cancelled");
	p2p_test_join_stop();
}

/* R-06: a hard radio fault tried nothing but must still end the round, or the
 * slow policy collapses into a jitter-only loop with a dev_nonce write each
 * turn. The dev_nonce is spent: it could have reached the air. */
ZTEST(p2p_logic, test_join_send_failure_still_ends_the_round)
{
	uint8_t rejoin;

	p2p_test_join_setup(7);
	p2p_test_set_join_started_at(k_uptime_get() - P2P_JOIN_BOOT_WINDOW_MS - 1);
	test_lora_send_ret = -EIO;
	p2p_test_join_step();
	test_lora_send_ret = 0;

	p2p_test_get_join(NULL, NULL, &rejoin, NULL);
	zassert_equal(rejoin, 1, "the round ends, so the slow policy backs off");
	zassert_equal(p2p_test_get_dev_nonce(), TOWER_JOIN_KAT_DEV_NONCE + 1);
	zassert_equal(p2p_test_air_ends, p2p_test_air_begins, "a failed send ends at once");
	p2p_test_join_stop();
}

/* R-01: the shell `join` must pre-empt a slow retry pending for up to an hour
 * (k_work_schedule is a no-op on a scheduled item). */
ZTEST(p2p_logic, test_shell_join_preempts_a_pending_slow_retry)
{
	p2p_test_join_setup(7);
	p2p_test_set_join_started_at(k_uptime_get() - P2P_JOIN_BOOT_WINDOW_MS - 1);
	p2p_test_join_arm_retry(3600000);
	zassert_true(p2p_test_join_pending_ms() > 10000, "the retry to pre-empt is pending");

	p2p_test_join_restart();
	zassert_equal(p2p_test_join_pending_ms(), 0, "an operator join goes now, %lld ms left",
		      (long long)p2p_test_join_pending_ms());
	p2p_test_join_stop();
}

/* Review of #400 (M4): the next dev_nonce is persisted BEFORE the JoinRequest
 * leaves, fail-closed. */
ZTEST(p2p_logic, test_join_request_not_sent_without_a_durable_dev_nonce)
{
	uint32_t sends = test_lora_send_count;

	p2p_test_join_setup(7);
	test_settings_save_ret = -EIO;
	p2p_test_join_step();
	test_settings_save_ret = 0;

	zassert_equal(test_lora_send_count, sends, "no JoinRequest without a durable dev_nonce");
	zassert_equal(p2p_test_get_dev_nonce(), TOWER_JOIN_KAT_DEV_NONCE);
	p2p_test_join_stop();
}

/* ---- start, state, rejoin ---------------------------------------------------- */

/* doc/plan/439 T3: a P2P link coming up announces through the common app_radio
 * path, the boot with a persisted pairing included. */
ZTEST(p2p_logic, test_paired_boot_announces_through_app_radio)
{
	int announces = p2p_test_announce_calls;

	p2p_test_set_paired();
	app_radio_p2p_start();

	zassert_true(app_radio_p2p_is_ready(), "paired boot must be ready");
	zassert_equal(p2p_test_announce_calls, announces + 1);
	zassert_equal(p2p_test_ctrl_pending(), BIT(0) | BIT(1), "Capabilities + Hello queued");
}

/* The address is in every frame header (plan §6.1): no session is any use
 * without it, so a zero DevEUI refuses even a persisted pairing. */
ZTEST(p2p_logic, test_start_refuses_without_an_address_even_when_paired)
{
	p2p_test_set_paired();
	memset(g_app_config.radio_deveui, 0, sizeof(g_app_config.radio_deveui));
	app_radio_p2p_start();

	zassert_false(app_radio_p2p_is_ready());
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_DISABLED);

	/* An all-ones low word is the reserved broadcast address. */
	p2p_test_set_link(P2P_LINK_PAIRED, false, false, false);
	memset(&g_app_config.radio_deveui[4], 0xff, 4);
	app_radio_p2p_start();
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_DISABLED);
}

/* p2p-modulation fsk is plan P5: nothing that would transmit LoRa into an fsk
 * network runs -- not the boot, not a self-heal. */
ZTEST(p2p_logic, test_fsk_refuses_to_start)
{
	const struct app_radio_backend *be = &app_radio_p2p_backend;
	uint32_t sends = test_lora_send_count;

	g_app_config.p2p_modulation = APP_CONFIG_P2P_MODULATION_FSK;
	app_radio_p2p_start();
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_DISABLED, "unpaired boot");

	p2p_test_set_link(P2P_LINK_PAIRED, false, false, false);
	app_radio_p2p_start();
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_DISABLED, "paired boot");
	zassert_false(app_radio_p2p_is_ready());

	p2p_test_set_link(P2P_LINK_PAIRED, true, false, false);
	zassert_equal(be->rejoin(false), -ENOTSUP, "no self-heal on fsk");
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_HEALTHY, "the session is kept");
	zassert_equal(test_lora_send_count, sends, "nothing on the air");
}

/* §7: app_radio's rejoin op is the self-healing re-join on the slow policy,
 * refused while unprovisioned. */
ZTEST(p2p_logic, test_rejoin_op_self_heals_or_refuses_unprovisioned)
{
	const struct app_radio_backend *be = &app_radio_p2p_backend;

	p2p_test_set_link(P2P_LINK_PAIRED, true, false, false);
	memset(g_app_config.radio_appkey, 0, sizeof(g_app_config.radio_appkey));
	zassert_equal(be->rejoin(false), -ENOTSUP, "all-zero app_key: refused");
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_HEALTHY, "session kept");

	memcpy(g_app_config.radio_appkey, tower_join_kat_app_key,
	       sizeof(g_app_config.radio_appkey));
	memset(g_app_config.radio_deveui, 0, sizeof(g_app_config.radio_deveui));
	zassert_equal(be->rejoin(true), -ENOTSUP, "all-zero DevEUI: refused");

	memcpy(g_app_config.radio_deveui, tower_join_kat_dev_eui,
	       sizeof(g_app_config.radio_deveui));
	zassert_equal(be->rejoin(false), 0, "provisioned: re-join");
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_RECONNECT, "on the slow policy");
	zassert_false(app_radio_p2p_is_ready(), "not ready while the session is replaced");
	p2p_test_join_stop();
}

/* Answers and alarms stay queued in app_radio while unpaired; the link-up
 * kicks them out under the new session. */
ZTEST(p2p_logic, test_not_ready_while_unpaired_and_link_up_kicks)
{
	p2p_test_set_link(P2P_LINK_JOINING, true, false, false);
	zassert_false(app_radio_p2p_backend.tx_ready(), "frames wait while joining");

	int kicks = p2p_test_tx_kick_calls;

	p2p_test_set_paired();
	app_radio_p2p_start();
	zassert_true(app_radio_p2p_backend.tx_ready(), "ready once paired");
	zassert_true(p2p_test_tx_kick_calls > kicks, "the link-up sends what waited");
}

/* doc/plan/439 T1: the link state in the common app_radio terms. */
ZTEST(p2p_logic, test_radio_state_mapping)
{
	p2p_test_set_link(P2P_LINK_PAIRED, true, false, false);
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_HEALTHY);
	zassert_true(app_radio_p2p_is_ready());

	p2p_test_set_link(P2P_LINK_JOINING, false, false, false);
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_JOINING, "boot / forced join");
	zassert_false(app_radio_p2p_is_ready());

	p2p_test_set_link(P2P_LINK_JOINING, true, true, false);
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_RECONNECT);
	zassert_false(app_radio_p2p_is_ready(), "not ready while the session is replaced");

	p2p_test_set_link(P2P_LINK_UNPAIRED, false, false, false);
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_IDLE, "after a Detach");

	p2p_test_set_link(P2P_LINK_UNPAIRED, false, false, true);
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_DISABLED, "unprovisioned");
}

ZTEST(p2p_logic, test_last_downlink_recorded)
{
	int calls = p2p_test_dl_calls;

	p2p_test_note_downlink(-65, 12);
	zassert_equal(p2p_test_dl_calls, calls + 1, "pushed once");
	zassert_equal(p2p_test_dl_rssi, -65);
	zassert_equal(p2p_test_dl_snr, 12);
}

/* §7.4: no power control on P2P -- WARNING has no rung, straight to the rejoin
 * budget. */
ZTEST(p2p_logic, test_warning_has_no_rung)
{
	p2p_test_set_link(P2P_LINK_PAIRED, true, false, false);
	p2p_test_set_session_tx_power(true, 10);
	zassert_false(app_radio_p2p_backend.warning_step());
	p2p_test_set_session_tx_power(false, 0);
	zassert_false(app_radio_p2p_backend.warning_step());
}

/* ---- the exchange -------------------------------------------------------------- */

/* The node's confirmed uplink is the KAT's uplink_confirmed frame, and the
 * gateway's answer the KAT's ack_no_pending (counter 100, acked 2, rssi -57). */
ZTEST(p2p_logic, test_confirmed_uplink_matches_the_kat_and_takes_the_kat_ack)
{
	const struct tower_kat *up = &tower_kat[1];
	struct app_radio_p2p_info info;
	uint32_t sends;

	paired();
	gw.ack = true;
	p2p_test_set_fcnt(2, 256);
	sends = test_lora_send_count;

	zassert_ok(p2p_test_uplink(up->plaintext, up->plaintext_len, true));
	zassert_equal(test_lora_send_count, sends + 1, "ACKed at the first transmission");
	zassert_equal(test_lora_last_len, up->frame_len);
	zassert_mem_equal(test_lora_last_frame, up->frame, up->frame_len, "the KAT uplink");
	zassert_equal(gw.ack_frame_len, tower_kat[2].frame_len);
	zassert_mem_equal(gw.ack_frame, tower_kat[2].frame, tower_kat[2].frame_len, "the KAT ACK");

	app_radio_p2p_get_info(&info);
	zassert_equal(info.gw_last, 100, "the ACK lifted the replay lane");
	zassert_true(info.last_ack_valid);
	zassert_equal(info.last_ack_rssi, -57, "the gateway's RSSI of the uplink");
	zassert_equal(info.fcnt, 3);
	zassert_equal(p2p_test_dl_rssi, -60, "this node's RSSI of the ACK");
	zassert_equal(p2p_test_air_ends, p2p_test_air_begins);
}

/* An unconfirmed uplink goes once and opens no receive window. */
ZTEST(p2p_logic, test_unconfirmed_uplink_matches_the_kat)
{
	const struct tower_kat *up = &tower_kat[0];
	uint32_t sends;

	paired();
	gw.ack = true;
	sends = test_lora_send_count;

	zassert_ok(p2p_test_uplink(up->plaintext, up->plaintext_len, false));
	zassert_equal(test_lora_send_count, sends + 1);
	zassert_mem_equal(test_lora_last_frame, up->frame, up->frame_len, "the KAT uplink");
	zassert_equal(gw.ack_frame_len, 0, "nothing to ACK");
	zassert_equal(p2p_test_air_ends, p2p_test_air_begins);
}

/* Plan §7.1: a confirmed frame without its ACK goes P2P_REPS (3) times, byte
 * for byte under the same counter. Unheard, it is neither a link success nor a
 * failure: that verdict is app_radio's, after its retries. */
ZTEST(p2p_logic, test_unacked_confirmed_frame_goes_three_times)
{
	static const uint8_t pt[] = {P2P_ENV_DATA, P2P_PORT_ALARM, 0x01, 0x02};
	int ok = p2p_test_link_ok_calls;
	int fail = p2p_test_link_fail_calls;
	uint32_t sends;

	paired();
	sends = test_lora_send_count;

	zassert_equal(p2p_test_uplink(pt, sizeof(pt), true), -ETIMEDOUT);
	zassert_equal(test_lora_send_count, sends + 3);
	zassert_equal(gw.reps, 3, "three transmissions of one counter");
	for (uint32_t i = 1; i < 3; i++) {
		zassert_mem_equal(
			test_lora_sent_frame(sends + i)->buf, test_lora_sent_frame(sends)->buf,
			test_lora_sent_frame(sends)->len, "transmission %u is byte-identical", i);
	}
	zassert_equal(p2p_test_link_ok_calls, ok);
	zassert_equal(p2p_test_link_fail_calls, fail);
	zassert_equal(p2p_test_air_ends, p2p_test_air_begins, "every transmission ended");
}

ZTEST(p2p_logic, test_ack_on_the_second_transmission_ends_the_send)
{
	static const uint8_t pt[] = {P2P_ENV_DATA, P2P_PORT_ALARM, 0x01};
	uint32_t sends;

	paired();
	gw.ack = true;
	gw.ack_from = 2;
	sends = test_lora_send_count;

	zassert_ok(p2p_test_uplink(pt, sizeof(pt), true));
	zassert_equal(test_lora_send_count, sends + 2);
}

/* An ACK the node must not take: for another counter, failing its tag, or
 * below the replay lane. Each leaves the frame unacknowledged. */
ZTEST(p2p_logic, test_foreign_or_replayed_ack_is_ignored)
{
	static const uint8_t pt[] = {P2P_ENV_DATA, P2P_PORT_ALARM, 0x01};
	uint32_t sends;

	paired();
	gw.ack = true;
	gw.acked_offset = 1;
	sends = test_lora_send_count;
	zassert_equal(p2p_test_uplink(pt, sizeof(pt), true), -ETIMEDOUT, "acks another counter");
	zassert_equal(test_lora_send_count, sends + 3);

	gw.acked_offset = 0;
	gw.bad_tag = true;
	zassert_equal(p2p_test_uplink(pt, sizeof(pt), true), -ETIMEDOUT, "tag check fails");

	/* A good ACK lifts the lane to its counter; one below it is a replay. */
	gw.bad_tag = false;
	zassert_ok(p2p_test_uplink(pt, sizeof(pt), true));
	zassert_equal(p2p_test_get_gw_last(), gw.counter);
	gw.counter -= 5; /* all three ACKs of the next send land below the lane */
	zassert_equal(p2p_test_uplink(pt, sizeof(pt), true), -ETIMEDOUT, "a replayed ACK");

	/* Another session's gateway. */
	gw.counter = 1000;
	gw.net_id = 0x01020304;
	zassert_equal(p2p_test_uplink(pt, sizeof(pt), true), -ETIMEDOUT, "another gateway");
}

/* Plan §9.1: PENDING keeps the receiver on for one Data frame. A confirmed
 * command is delivered to app_cmd and ACKed with the node's own fresh counter,
 * acked = the downlink's counter, rssi = this node's RSSI of it. */
ZTEST(p2p_logic, test_pending_command_is_delivered_and_acked)
{
	static const uint8_t up[] = {P2P_ENV_DATA, P2P_PORT_TELEMETRY, 0x08, 0x01};
	static const uint8_t cmd[] = {P2P_ENV_DATA, P2P_PORT_COMMAND, 0x01, 0x0a, 0x0b};
	int dls = p2p_test_downlinks;

	paired();
	gw.ack = true;
	gw.dl = cmd;
	gw.dl_len = sizeof(cmd);
	gw.dl_confirmed = true;

	zassert_ok(p2p_test_uplink(up, sizeof(up), true));
	zassert_equal(p2p_test_downlinks, dls + 1, "delivered once");
	zassert_equal(p2p_test_downlink_len, 3, "the envelope is stripped");
	zassert_mem_equal(p2p_test_downlink_buf, &cmd[P2P_ENV_LEN], 3);

	zassert_equal(gw.node_acks, 1, "the node ACKed the downlink");
	zassert_equal(gw.node_ack.acked, 101, "acked = the downlink's counter");
	zassert_equal(gw.node_ack.rssi, -61, "the node's RSSI of the downlink");
	zassert_false(gw.node_ack.pending);
	zassert_equal(gw.node_ack_hdr.counter, gw.up_hdr.counter + 1, "the node's next counter");
	zassert_equal(gw.node_ack_hdr.src, TOWER_KAT_NODE_ADDR);
	zassert_equal(gw.node_ack_hdr.dest, TOWER_KAT_GW_ADDR);
	zassert_equal(p2p_test_get_gw_last(), 101);
	zassert_equal(p2p_test_air_ends, p2p_test_air_begins);
}

ZTEST(p2p_logic, test_unconfirmed_downlink_is_delivered_without_an_ack)
{
	static const uint8_t up[] = {P2P_ENV_DATA, P2P_PORT_TELEMETRY, 0x08};
	static const uint8_t cmd[] = {P2P_ENV_DATA, P2P_PORT_COMMAND, 0x01};
	int dls = p2p_test_downlinks;

	paired();
	gw.ack = true;
	gw.dl = cmd;
	gw.dl_len = sizeof(cmd);

	zassert_ok(p2p_test_uplink(up, sizeof(up), true));
	zassert_equal(p2p_test_downlinks, dls + 1);
	zassert_equal(gw.node_acks, 0);
}

/* A recorded downlink replayed under an older counter is neither delivered nor
 * ACKed. */
ZTEST(p2p_logic, test_replayed_downlink_is_not_delivered)
{
	static const uint8_t up[] = {P2P_ENV_DATA, P2P_PORT_TELEMETRY, 0x08};
	static const uint8_t cmd[] = {P2P_ENV_DATA, P2P_PORT_COMMAND, 0x01};
	int dls = p2p_test_downlinks;

	paired();
	gw.ack = true;
	gw.dl = cmd;
	gw.dl_len = sizeof(cmd);
	gw.dl_confirmed = true;
	zassert_ok(p2p_test_uplink(up, sizeof(up), true));
	zassert_equal(p2p_test_downlinks, dls + 1);

	gw.dl_counter = 101; /* the one just delivered, now below the lane */
	zassert_ok(p2p_test_uplink(up, sizeof(up), true));
	zassert_equal(p2p_test_downlinks, dls + 1, "not delivered again");
	zassert_equal(gw.node_acks, 1, "and not ACKed again");
}

/* Only a command (0x81 port 86) or a control list (0x91) is taken. */
ZTEST(p2p_logic, test_unknown_downlink_envelope_is_dropped)
{
	static const uint8_t up[] = {P2P_ENV_DATA, P2P_PORT_TELEMETRY, 0x08};
	static const uint8_t wrong_port[] = {P2P_ENV_DATA, P2P_PORT_TELEMETRY, 0x01};
	static const uint8_t wrong_env[] = {0x55, P2P_PORT_COMMAND, 0x01};
	int dls = p2p_test_downlinks;

	paired();
	gw.ack = true;
	gw.dl = wrong_port;
	gw.dl_len = sizeof(wrong_port);
	zassert_ok(p2p_test_uplink(up, sizeof(up), true));
	gw.dl = wrong_env;
	gw.dl_len = sizeof(wrong_env);
	zassert_ok(p2p_test_uplink(up, sizeof(up), true));
	zassert_equal(p2p_test_downlinks, dls, "neither reaches app_cmd");
}

/* ---- the TX backend ------------------------------------------------------------ */

static int backend_send(enum app_radio_frame_kind kind, uint8_t port, uint8_t flags,
			const uint8_t *body, size_t len, struct app_radio_tx_result *res)
{
	static uint8_t buf[P2P_MAX_BODY + 1];
	struct app_radio_frame f = {
		.kind = kind, .port = port, .flags = flags, .len = (uint16_t)len, .buf = buf};

	memcpy(buf, body, MIN(len, sizeof(buf)));
	return app_radio_p2p_backend.send(&f, res);
}

/* D-a: the LoRaWAN fPort payload behind the 0x81 envelope, on the LoRaWAN
 * port: telemetry 2, alarm 3, answers 85 (or the command's own port). */
ZTEST(p2p_logic, test_backend_envelopes_and_ports)
{
	static const uint8_t body[] = {0x01, 0xaa, 0xbb};
	const struct {
		enum app_radio_frame_kind kind;
		uint8_t port;
		uint8_t flags;
		uint8_t want_port;
	} cases[] = {
		{APP_RADIO_FRAME_TELEMETRY, 0, 0, P2P_PORT_TELEMETRY},
		{APP_RADIO_FRAME_ALARM, 0, APP_RADIO_FRAME_CONFIRMED, P2P_PORT_ALARM},
		{APP_RADIO_FRAME_ANSWER, 0, APP_RADIO_FRAME_CONFIRMED, P2P_PORT_RESPONSE},
		{APP_RADIO_FRAME_ANSWER, 90, APP_RADIO_FRAME_CONFIRMED, 90},
	};
	struct app_radio_tx_result res = {0};

	paired();
	gw.ack = true;
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		zassert_ok(backend_send(cases[i].kind, cases[i].port, cases[i].flags, body,
					sizeof(body), &res),
			   "case %zu", i);
		zassert_equal(gw.up_len, P2P_ENV_LEN + sizeof(body), "case %zu", i);
		zassert_equal(gw.up_pt[0], P2P_ENV_DATA);
		zassert_equal(gw.up_pt[1], cases[i].want_port, "case %zu port %u", i, gw.up_pt[1]);
		zassert_mem_equal(&gw.up_pt[P2P_ENV_LEN], body, sizeof(body));
		zassert_equal((gw.up_hdr.flags & TWR_FLAG_CONFIRMED) != 0,
			      (cases[i].flags & APP_RADIO_FRAME_CONFIRMED) != 0, "case %zu", i);
		p2p_test_tx_reset();
	}
}

/* D14: a 76 B body fills the 100 B frame; one more is the budget's to split. */
ZTEST(p2p_logic, test_backend_budget_and_oversize)
{
	static const uint8_t body[P2P_MAX_BODY + 1];
	struct app_radio_tx_result res = {0};

	paired();
	zassert_equal(app_radio_p2p_backend.budget(), P2P_MAX_BODY);
	zassert_equal(app_radio_p2p_get_max_payload(), 76);

	zassert_ok(backend_send(APP_RADIO_FRAME_TELEMETRY, 0, 0, body, P2P_MAX_BODY, &res));
	zassert_equal(test_lora_last_len, P2P_FRAME_MAX, "the MTU");
	p2p_test_tx_reset();

	zassert_equal(backend_send(APP_RADIO_FRAME_TELEMETRY, 0, 0, body, P2P_MAX_BODY + 1, &res),
		      -EMSGSIZE);
	zassert_equal(res.budget, P2P_MAX_BODY);
	zassert_equal(backend_send(APP_RADIO_FRAME_TELEMETRY, 0, 0, body, 0, &res), -EINVAL);
}

/* A radio fault is -EIO (app_radio retries it) and its air is charged; a duty
 * hold is -EAGAIN with the wait, nothing sent; an ACKed confirmed frame is a
 * link success. */
ZTEST(p2p_logic, test_backend_send_result_mapping)
{
	static const uint8_t body[10];
	struct app_radio_tx_result res = {0};
	const uint32_t air = p2p_toa_ms(7, TWR_HDR_LEN + P2P_ENV_LEN + 10 + TWR_TAG_LEN);
	uint32_t charges = test_duty_charges;
	uint64_t charged = test_duty_charged_ms;
	uint32_t sends;
	int ok = p2p_test_link_ok_calls;

	paired();
	zassert_equal(app_radio_p2p_backend.airtime_ms(10), air, "airtime of a 10 B body");

	test_lora_send_ret = -EIO;
	zassert_equal(backend_send(APP_RADIO_FRAME_TELEMETRY, 0, 0, body, 10, &res), -EIO);
	test_lora_send_ret = 0;
	zassert_equal(test_duty_charges, charges + 1, "a failed TX is charged");
	p2p_test_tx_reset();

	sends = test_lora_send_count;
	zassert_ok(backend_send(APP_RADIO_FRAME_TELEMETRY, 0, 0, body, 10, &res));
	zassert_equal(test_lora_send_count, sends + 1);
	zassert_equal(test_duty_charged_ms - charged, 2 * air, "both at their air");
	p2p_test_tx_reset();

	test_duty_wait_ms = 5000;
	sends = test_lora_send_count;
	zassert_equal(
		backend_send(APP_RADIO_FRAME_ALARM, 0, APP_RADIO_FRAME_CONFIRMED, body, 10, &res),
		-EAGAIN);
	zassert_equal(res.wait_ms, 5000);
	zassert_equal(test_lora_send_count, sends, "held: nothing on the air");
	test_duty_wait_ms = 0;

	gw.ack = true;
	zassert_ok(
		backend_send(APP_RADIO_FRAME_ALARM, 0, APP_RADIO_FRAME_CONFIRMED, body, 10, &res));
	zassert_equal(p2p_test_link_ok_calls, ok + 1, "an ACK is a passed link check");
	p2p_test_tx_reset();

	gw.ack = false;
	zassert_equal(
		backend_send(APP_RADIO_FRAME_ALARM, 0, APP_RADIO_FRAME_CONFIRMED, body, 10, &res),
		-ETIMEDOUT, "no ACK: app_radio retries");
}

/* Plan §7.3: app_radio's retry of a frame that got no ACK is a new TOWER send,
 * under a new counter. */
ZTEST(p2p_logic, test_retry_takes_a_new_counter)
{
	static const uint8_t body[4] = {0x10};
	struct app_radio_frame f = {.kind = APP_RADIO_FRAME_ANSWER,
				    .flags = APP_RADIO_FRAME_CONFIRMED,
				    .len = sizeof(body),
				    .buf = (uint8_t *)body};
	struct app_radio_tx_result res = {0};

	paired();
	p2p_test_set_fcnt(100, 200);
	zassert_equal(app_radio_p2p_backend.send(&f, &res), -ETIMEDOUT);
	zassert_equal(gw.up_hdr.counter, 100);
	p2p_test_tx_reset();

	f.attempt = 1;
	zassert_equal(app_radio_p2p_backend.send(&f, &res), -ETIMEDOUT);
	zassert_equal(gw.up_hdr.counter, 101, "the retry is a new send");
}

ZTEST(p2p_logic, test_not_paired_sends_nothing)
{
	static const uint8_t body[4];
	struct app_radio_tx_result res = {0};
	uint32_t sends = test_lora_send_count;

	zassert_equal(backend_send(APP_RADIO_FRAME_TELEMETRY, 0, 0, body, 4, &res), -ENOTCONN);
	zassert_equal(test_lora_send_count, sends);
}

/* A new session starts a new replay lane: the next gateway counts from 1. */
ZTEST(p2p_logic, test_new_session_resets_the_replay_lane)
{
	static const uint8_t pt[] = {P2P_ENV_DATA, P2P_PORT_ALARM, 0x01};

	paired();
	gw.ack = true;
	zassert_ok(p2p_test_uplink(pt, sizeof(pt), true));
	zassert_equal(p2p_test_get_gw_last(), 100);

	paired();
	zassert_equal(p2p_test_get_gw_last(), 0);
}

/* ---- the control envelope --------------------------------------------------------- */

/* Plan §8.2: Capabilities (proto 1, MTU 100, lora, the cmd bitmap, battery)
 * and Hello (session id, reset reason, fw 1.5.0) in one confirmed 0x91 frame. */
ZTEST(p2p_logic, test_ctrl_caps_hello_layout)
{
	static const uint8_t caps[] = {P2P_CTRL_CAPABILITIES,
				       P2P_CAPS_LEN,
				       0x01,
				       100,
				       0x02,
				       0x9e,
				       0x01,
				       0x01,
				       0x00,
				       0x01,
				       0x00,
				       0x00,
				       0x00,
				       0x01};

	paired();
	gw.ack = true;
	p2p_test_set_paired();
	app_radio_p2p_start();
	zassert_equal(p2p_test_ctrl_pending(), BIT(0) | BIT(1));
	p2p_test_ctrl_run();

	zassert_equal(p2p_test_ctrl_pending(), 0, "acknowledged: done");
	zassert_true(gw.up_hdr.flags & TWR_FLAG_CONFIRMED);
	zassert_equal(gw.up_len, 1 + sizeof(caps) + 2 + P2P_HELLO_LEN, "26 B");
	zassert_equal(gw.up_pt[0], P2P_ENV_CTRL);
	zassert_mem_equal(&gw.up_pt[1], caps, sizeof(caps));

	const uint8_t *hello = &gw.up_pt[1 + sizeof(caps)];

	zassert_equal(hello[0], P2P_CTRL_HELLO);
	zassert_equal(hello[1], P2P_HELLO_LEN);
	zassert_equal(hello[6], 0x08, "reset reason: the hwinfo POR bit");
	zassert_equal(hello[7], 1);
	zassert_equal(hello[8], 5);
	zassert_equal(hello[9], 0);
	zassert_equal(hello[10], 0);
}

/* Every TLV the node sends fits one frame: Capabilities, Hello, LinkCheckReq
 * and TimeReq are 30 B, in that order. The cmd bitmap names the IDs this node
 * implements. */
ZTEST(p2p_logic, test_ctrl_full_set_is_30_bytes)
{
	paired();
	gw.ack = true;
	p2p_test_time_wanted = true;
	p2p_test_set_paired();
	app_radio_p2p_start();
	(void)app_radio_p2p_backend.report_flags(true); /* a link check: LinkCheckReq */
	zassert_equal(p2p_test_ctrl_pending(), BIT(0) | BIT(1) | BIT(2) | BIT(3));
	p2p_test_ctrl_run();

	zassert_equal(gw.up_len, 30);
	zassert_equal(gw.up_pt[1], P2P_CTRL_CAPABILITIES);
	zassert_equal(gw.up_pt[15], P2P_CTRL_HELLO);
	zassert_mem_equal(&gw.up_pt[26], ((uint8_t[]){P2P_CTRL_LINK_CHECK, 0, P2P_CTRL_TIME, 0}), 4,
			  "LinkCheckReq, then TimeReq, both empty");

	const uint8_t *bitmap = &gw.up_pt[1 + 2 + 3];
	const uint8_t want[] = {0x01, 0x02, 0x03, 0x04, 0x07, 0x08, 0x10, 0x20};
	size_t n = 0;

	for (unsigned int id = 0; id < 64; id++) {
		if (bitmap[id / 8] & BIT(id % 8)) {
			zassert_true(n < ARRAY_SIZE(want), "extra ID 0x%02x", id);
			zassert_equal(id, want[n], "ID %zu is 0x%02x", n, id);
			n++;
		}
	}
	zassert_equal(n, ARRAY_SIZE(want));
}

/* Unacknowledged, the set goes again, up to 3 tries, then it is dropped. */
ZTEST(p2p_logic, test_ctrl_unacked_retries_then_drops)
{
	uint32_t sends;

	paired();
	app_radio_p2p_backend.time_request();
	sends = test_lora_send_count;
	p2p_test_ctrl_run();
	zassert_equal(p2p_test_ctrl_pending(), BIT(3), "kept after the 1st try");
	p2p_test_ctrl_run();
	zassert_equal(p2p_test_ctrl_pending(), BIT(3), "kept after the 2nd try");
	p2p_test_ctrl_run();
	zassert_equal(p2p_test_ctrl_pending(), 0, "dropped after the 3rd");
	zassert_equal(test_lora_send_count, sends + 9, "3 tries x 3 transmissions");
}

/* TimeReq -> TimeAns (plan §8.2, TimeAns addendum): the answer names the
 * TimeReq's counter and the time at its TX-done; the node adds the time since. */
ZTEST(p2p_logic, test_time_req_then_time_ans_sets_the_clock)
{
	int events = p2p_test_time_events;
	uint8_t ans[2 + P2P_TIME_ANS_LEN] = {P2P_CTRL_TIME, P2P_TIME_ANS_LEN};

	paired();
	gw.ack = true;
	p2p_test_time_wanted = true;
	app_radio_p2p_backend.time_request();
	p2p_test_ctrl_run();
	zassert_equal(gw.up_len, 3);
	zassert_mem_equal(gw.up_pt, ((uint8_t[]){P2P_ENV_CTRL, P2P_CTRL_TIME, 0}), 3);

	g_test_network_time = 0;
	sys_put_le32(1790000000u, &ans[2]);
	ans[6] = 0;
	sys_put_le32(gw.up_hdr.counter + 1, &ans[7]); /* another request's */
	p2p_test_ctrl_downlink(ans, sizeof(ans));
	zassert_equal(p2p_test_time_events, events, "not an answer to this TimeReq");

	sys_put_le32(gw.up_hdr.counter, &ans[7]);
	p2p_test_ctrl_downlink(ans, sizeof(ans));
	zassert_equal(p2p_test_time_events, events + 1, "the time landed");
	zassert_between_inclusive(g_test_network_time, 1790000000u, 1790000001u);

	/* Consumed: a replay of the same answer is dropped. */
	g_test_network_time = 0;
	p2p_test_ctrl_downlink(ans, sizeof(ans));
	zassert_equal(p2p_test_time_events, events + 1);
	zassert_equal(g_test_network_time, 0);
}

ZTEST(p2p_logic, test_stale_time_ans_is_dropped)
{
	int events = p2p_test_time_events;
	uint8_t ans[2 + P2P_TIME_ANS_LEN] = {P2P_CTRL_TIME, P2P_TIME_ANS_LEN};

	paired();
	g_test_network_time = 0;
	sys_put_le32(1790000000u, &ans[2]);
	sys_put_le32(55, &ans[7]);
	p2p_test_set_time_req(55, k_uptime_get() - 2LL * 60 * 60 * 1000 - 1);
	p2p_test_ctrl_downlink(ans, sizeof(ans));
	zassert_equal(p2p_test_time_events, events);
	zassert_equal(g_test_network_time, 0);

	/* A TimeAns shorter than its 9 B is not one. */
	p2p_test_set_time_req(55, k_uptime_get());
	ans[1] = P2P_TIME_ANS_LEN - 1;
	p2p_test_ctrl_downlink(ans, sizeof(ans) - 1);
	zassert_equal(p2p_test_time_events, events);
}

/* Plan §7.2 (F6): every report goes CONFIRMED, link check or not, whatever
 * answer is outstanding -- a queued downlink waits one report at most. */
ZTEST(p2p_logic, test_every_report_goes_confirmed)
{
	const struct app_radio_backend *be = &app_radio_p2p_backend;

	paired();
	g_app_config.radio_link_check_interval = 0;
	for (int i = 0; i < 5; i++) {
		zassert_equal(be->report_flags(false), APP_RADIO_FRAME_CONFIRMED, "report %d", i);
	}
	p2p_test_time_wanted = true;
	p2p_test_set_time_req(7, k_uptime_get());
	for (int i = 0; i < 5; i++) {
		zassert_equal(be->report_flags(false), APP_RADIO_FRAME_CONFIRMED,
			      "report %d with a TimeAns outstanding", i);
	}
	zassert_equal(p2p_test_ctrl_pending(), 0, "no LinkCheckReq off the cadence");
}

/* No Poll (plan H3.8): a LinkCheckAns still missing after three reports is
 * lost, so the next link check may ask again. */
ZTEST(p2p_logic, test_lost_link_check_ans_frees_the_next_request)
{
	const struct app_radio_backend *be = &app_radio_p2p_backend;

	paired();
	gw.ack = true;
	zassert_equal(be->report_flags(true), APP_RADIO_FRAME_CONFIRMED);
	zassert_equal(p2p_test_ctrl_pending(), BIT(2), "LinkCheckReq queued");
	p2p_test_ctrl_run(); /* acknowledged, no answer yet */
	zassert_equal(p2p_test_ctrl_pending(), 0);

	/* Outstanding: a due report does not ask again. */
	zassert_equal(be->report_flags(true), APP_RADIO_FRAME_CONFIRMED);
	zassert_equal(p2p_test_ctrl_pending(), 0, "the earlier one is unanswered");
	(void)be->report_flags(false);
	(void)be->report_flags(false);
	(void)be->report_flags(false); /* the 4th since the request: lost */
	zassert_equal(be->report_flags(true), APP_RADIO_FRAME_CONFIRMED);
	zassert_equal(p2p_test_ctrl_pending(), BIT(2), "asked again");
}

/* §3.2: a report the cadence made a link check also asks the central for the
 * numbers (LinkCheckReq); both go CONFIRMED. */
ZTEST(p2p_logic, test_link_check_report_goes_confirmed)
{
	const struct app_radio_backend *be = &app_radio_p2p_backend;

	paired();
	zassert_equal(be->report_flags(false), APP_RADIO_FRAME_CONFIRMED);
	zassert_equal(p2p_test_ctrl_pending(), 0);
	zassert_equal(be->report_flags(true), APP_RADIO_FRAME_CONFIRMED);
	zassert_equal(p2p_test_ctrl_pending(), BIT(2), "LinkCheckReq queued");
}

ZTEST(p2p_logic, test_link_check_ans_applies)
{
	static const uint8_t ans[] = {P2P_CTRL_LINK_CHECK, 4, (uint8_t)-80, 5, 12, 1};
	struct app_radio_p2p_info info;
	int ok = p2p_test_link_ok_calls;

	paired();
	p2p_test_ctrl_downlink(ans, sizeof(ans));
	app_radio_p2p_get_info(&info);
	zassert_true(info.lc_valid);
	zassert_equal(info.lc_rssi, -80);
	zassert_equal(info.lc_snr, 5);
	zassert_equal(info.lc_margin, 12);
	zassert_equal(info.lc_gw_count, 1);
	zassert_equal(p2p_test_ul_margin, 12);
	zassert_equal(p2p_test_ul_gw_count, 1);
	zassert_equal(p2p_test_link_ok_calls, ok + 1, "an answer is a passed link check");
}

/* Plan §6.5: a Detach drops the session at once; nothing after it applies. */
ZTEST(p2p_logic, test_detach_clears_the_pairing)
{
	static const uint8_t dl[] = {P2P_CTRL_DETACH, 1, 2, P2P_CTRL_LINK_CHECK, 4, 1, 2, 3, 4};
	struct app_radio_p2p_info info;

	paired();
	p2p_test_ctrl_downlink(dl, sizeof(dl));
	zassert_false(app_radio_p2p_is_ready());
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_IDLE);
	app_radio_p2p_get_info(&info);
	zassert_false(info.lc_valid, "the LinkCheckAns after the Detach is ignored");
}

/* RejoinReq: a slow-policy join, unless the identity was cleared since. */
ZTEST(p2p_logic, test_rejoin_req_starts_a_slow_join)
{
	static const uint8_t dl[] = {P2P_CTRL_REJOIN_REQ, 1, 0};

	paired();
	memset(g_app_config.radio_appkey, 0, sizeof(g_app_config.radio_appkey));
	p2p_test_ctrl_downlink(dl, sizeof(dl));
	zassert_true(app_radio_p2p_is_ready(), "unprovisioned: not re-joining");

	memcpy(g_app_config.radio_appkey, tower_join_kat_app_key,
	       sizeof(g_app_config.radio_appkey));
	p2p_test_ctrl_downlink(dl, sizeof(dl));
	zassert_equal(app_radio_p2p_get_state(), APP_RADIO_STATE_RECONNECT);
	zassert_false(app_radio_p2p_is_ready());
	p2p_test_join_stop();
}

/* An unknown ID is skipped by its length; a malformed list stops the walk. */
ZTEST(p2p_logic, test_ctrl_unknown_skipped_malformed_stops)
{
	static const uint8_t skip[] = {0x77, 2, 0xaa, 0xbb, P2P_CTRL_LINK_CHECK, 4, 1, 2, 3, 4};
	static const uint8_t bad[] = {P2P_CTRL_LINK_CHECK, 5, 1, 2, 3, 4};
	struct app_radio_p2p_info info;

	paired();
	p2p_test_ctrl_downlink(bad, sizeof(bad));
	app_radio_p2p_get_info(&info);
	zassert_false(info.lc_valid, "a value past the list is not applied");

	p2p_test_ctrl_downlink(skip, sizeof(skip));
	app_radio_p2p_get_info(&info);
	zassert_true(info.lc_valid, "applied past the unknown entry");
	zassert_equal(info.lc_gw_count, 4);
}

/* A 0x91 answer can also ride a PENDING downlink. */
ZTEST(p2p_logic, test_ctrl_downlink_in_pending)
{
	static const uint8_t up[] = {P2P_ENV_DATA, P2P_PORT_TELEMETRY, 0x08};
	static const uint8_t dl[] = {P2P_ENV_CTRL, P2P_CTRL_LINK_CHECK, 4, (uint8_t)-90, 3, 8, 2};
	struct app_radio_p2p_info info;

	paired();
	gw.ack = true;
	gw.dl = dl;
	gw.dl_len = sizeof(dl);
	zassert_ok(p2p_test_uplink(up, sizeof(up), true));
	app_radio_p2p_get_info(&info);
	zassert_true(info.lc_valid);
	zassert_equal(info.lc_rssi, -90);
	zassert_equal(info.lc_gw_count, 2);
}

/* ---- frame counter (B9) --------------------------------------------------------- */

ZTEST(p2p_logic, test_fcnt_normal_advance)
{
	uint32_t c;

	p2p_test_set_fcnt(100, 200);
	zassert_ok(p2p_test_fcnt_next(&c));
	zassert_equal(c, 100u);
	zassert_equal(p2p_test_get_fcnt(), 101u);
}

/* At the window edge a durable reserve is required: refuse, and keep refusing
 * until it is durable -- advancing the in-RAM watermark first (the pre-B9 bug)
 * would hand out an unreserved counter on the second call, and a reboot would
 * then repeat a (key, nonce) pair. */
ZTEST(p2p_logic, test_fcnt_fail_closed_on_reserve_failure)
{
	uint32_t c = 0xDEADBEEF;

	test_settings_save_ret = -EIO;
	p2p_test_set_fcnt(200, 200);
	zassert_true(p2p_test_fcnt_next(&c) != 0);
	zassert_equal(p2p_test_get_fcnt(), 200u);
	zassert_equal(c, 0xDEADBEEFu);
	zassert_true(p2p_test_fcnt_next(&c) != 0, "the second call must also refuse");
	zassert_equal(p2p_test_get_fcnt(), 200u);
	zassert_equal(c, 0xDEADBEEFu);
}

ZTEST(p2p_logic, test_fcnt_saturates_no_wrap)
{
	uint32_t c = 0xDEADBEEF;

	p2p_test_set_fcnt(UINT32_MAX, UINT32_MAX);
	zassert_equal(p2p_test_fcnt_next(&c), -EOVERFLOW);
	zassert_equal(p2p_test_get_fcnt(), UINT32_MAX);
}

/* TOWER reserves counter 0: a new session's first frame is counter 1. */
ZTEST(p2p_logic, test_session_counter_skips_zero)
{
	static const uint8_t pt[] = {P2P_ENV_DATA, P2P_PORT_TELEMETRY, 0x08};

	paired();
	p2p_test_set_fcnt(0, 256);
	zassert_ok(p2p_test_uplink(pt, sizeof(pt), false));
	zassert_equal(gw.up_hdr.counter, 1);
}

ZTEST_SUITE(p2p_logic, NULL, NULL, before, NULL, NULL);
