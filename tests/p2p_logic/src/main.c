/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native unit tests for app_p2p.c's pure decision logic: LoRa time-on-air, the
 * CCM nonce layout, the data-plane frame codec, and the token-bucket duty-cycle
 * governor (doc/plan/408 §7 Steps 1-2). app_p2p.c is compiled directly (with a
 * no-op fake LoRa device, src/emul_lora.c, and thin stubs, src/stubs.c) and its
 * internal helpers are reached via the CONFIG_ZTEST hooks in app_p2p.h. Crypto
 * known-answer vectors live in tests/ccm; this suite is framing/timing/duty.
 */

#include "app_ccm.h"
#include "app_p2p.h"

#include <zephyr/ztest.h>
#include <zephyr/sys/byteorder.h>

#include <string.h>

/* ---- p2p_frame_toa_ms ------------------------------------------------- */

/* Documented reference point (app_p2p.c comment, doc/plan/408 §3): a full
 * 255 B frame at SF10/BW125 is ~2.3 s of air. */
ZTEST(p2p_logic, test_toa_sf10_max_frame_reference)
{
	uint32_t toa = p2p_toa_ms(10, 255);

	zassert_between_inclusive(toa, 2200, 2400,
				  "SF10/255 B ToA %u ms outside the documented ~2296 ms", toa);
}

ZTEST(p2p_logic, test_toa_monotonic_in_length)
{
	uint32_t prev = p2p_toa_ms(10, 15); /* header+tag only */

	for (uint16_t len = 16; len <= 255; len++) {
		uint32_t cur = p2p_toa_ms(10, (uint8_t)len);

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

ZTEST(p2p_logic, test_toa_small_frame_bounded)
{
	/* A short frame is well under the max frame's air time. */
	uint32_t small = p2p_toa_ms(10, 15);
	uint32_t big = p2p_toa_ms(10, 255);

	zassert_true(small > 0, "ToA must be positive");
	zassert_true(small < big, "small frame ToA %u not < max frame ToA %u", small, big);
}

/* ---- p2p_build_nonce -------------------------------------------------- */

ZTEST(p2p_logic, test_nonce_layout)
{
	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, 0x01020304u, 0xAABBu, 0x02 /* TELEMETRY */, P2P_DIR_TX);

	/* counter(4 BE) | dev_addr(2 BE) | frame_type(1) | dir(1) | zero-pad. */
	zassert_equal(sys_get_be32(&nonce[0]), 0x01020304u, "counter mis-encoded");
	zassert_equal(sys_get_be16(&nonce[4]), 0xAABBu, "dev_addr mis-encoded");
	zassert_equal(nonce[6], 0x02, "frame_type mis-encoded");
	zassert_equal(nonce[7], P2P_DIR_TX, "direction mis-encoded");
	for (size_t i = 8; i < P2P_NONCE_LEN; i++) {
		zassert_equal(nonce[i], 0, "nonce byte %zu not zero-padded", i);
	}
}

ZTEST(p2p_logic, test_nonce_direction_separates_keystream)
{
	uint8_t tx[P2P_NONCE_LEN];
	uint8_t rx[P2P_NONCE_LEN];

	build_nonce(tx, 5, 7, 0xFA, P2P_DIR_TX);
	build_nonce(rx, 5, 7, 0xFA, P2P_DIR_RX);

	zassert_true(memcmp(tx, rx, P2P_NONCE_LEN) != 0,
		     "TX and RX nonce identical under the same counter — keystream reuse");
	zassert_equal(tx[7], P2P_DIR_TX, "TX dir byte wrong");
	zassert_equal(rx[7], P2P_DIR_RX, "RX dir byte wrong");
}

/* ---- p2p_build_frame -------------------------------------------------- */

static const uint8_t k_session_key[P2P_KEY_LEN] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
						   0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

ZTEST(p2p_logic, test_build_frame_roundtrip)
{
	const uint32_t net_id = 0x00000001u;
	const uint16_t dev_addr = 0x0042u;
	const uint8_t frame_type = 0x02; /* TELEMETRY */
	const uint32_t counter = 12345;
	const uint8_t body[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
	const size_t body_len = sizeof(body);

	uint8_t frame[P2P_FRAME_MAX];

	zassert_ok(build_frame_keyed(net_id, dev_addr, k_session_key, frame_type, body, body_len,
				     counter, frame),
		   "build_frame failed");

	/* Cleartext header is the AAD, in the clear. */
	zassert_equal(sys_get_be32(&frame[0]), net_id, "net_id header wrong");
	zassert_equal(sys_get_be16(&frame[4]), dev_addr, "dev_addr header wrong");
	zassert_equal(frame[6], frame_type, "frame_type header wrong");
	zassert_equal(sys_get_be32(&frame[7]), counter, "counter header wrong");

	/* Decrypt the body with an independently built RX-direction nonce and
	 * the header as AAD — proves the frame is a valid CCM sealing. */
	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, counter, dev_addr, frame_type, P2P_DIR_TX);

	uint8_t pt[sizeof(body)];

	zassert_ok(app_ccm_auth_decrypt(k_session_key, nonce, P2P_NONCE_LEN, frame, P2P_HDR_LEN,
					&frame[P2P_HDR_LEN], body_len,
					&frame[P2P_HDR_LEN + body_len], P2P_TAG_LEN, pt),
		   "decrypt/verify of a freshly built frame failed");
	zassert_mem_equal(pt, body, body_len, "recovered body mismatch");
}

ZTEST(p2p_logic, test_build_frame_tag_detects_tamper)
{
	const uint32_t counter = 7;
	const uint16_t dev_addr = 0x0042u;
	const uint8_t frame_type = 0x03; /* ALARM */
	const uint8_t body[] = {0xAA, 0xBB, 0xCC};
	const size_t body_len = sizeof(body);

	uint8_t frame[P2P_FRAME_MAX];

	zassert_ok(build_frame_keyed(1, dev_addr, k_session_key, frame_type, body, body_len,
				     counter, frame),
		   "build_frame failed");

	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, counter, dev_addr, frame_type, P2P_DIR_TX);

	uint8_t pt[sizeof(body)];

	/* Flip one ciphertext byte -> tag must reject. */
	frame[P2P_HDR_LEN] ^= 0x01;
	zassert_equal(app_ccm_auth_decrypt(k_session_key, nonce, P2P_NONCE_LEN, frame, P2P_HDR_LEN,
					   &frame[P2P_HDR_LEN], body_len,
					   &frame[P2P_HDR_LEN + body_len], P2P_TAG_LEN, pt),
		      -EBADMSG, "tampered ciphertext accepted");
	frame[P2P_HDR_LEN] ^= 0x01; /* restore */

	/* Corrupt the header (AAD) -> tag must reject. */
	frame[0] ^= 0x80;
	zassert_equal(app_ccm_auth_decrypt(k_session_key, nonce, P2P_NONCE_LEN, frame, P2P_HDR_LEN,
					   &frame[P2P_HDR_LEN], body_len,
					   &frame[P2P_HDR_LEN + body_len], P2P_TAG_LEN, pt),
		      -EBADMSG, "tampered AAD accepted");
}

/* Detach (0xFD) and RejoinRequest (0xFE) are empty-bodied on the wire, so
 * recv_ack()'s link-control branch (§5.4) authenticates a zero-length CCM
 * message whose tag covers only the nonce and the 11 B header AAD. Nothing
 * else in the transport exercises that shape -- pin it here, both directions
 * of the verdict, since the central relies on it. */
ZTEST(p2p_logic, test_build_frame_empty_body_roundtrip)
{
	const uint32_t net_id = 0xB595CF19u;
	const uint16_t dev_addr = 0x0001u;
	const uint32_t counter = 4242;

	for (uint8_t i = 0; i < 2; i++) {
		const uint8_t frame_type = i ? APP_P2P_FRAME_REJOIN_REQUEST : APP_P2P_FRAME_DETACH;
		uint8_t frame[P2P_HDR_LEN + P2P_TAG_LEN]; /* 15 B, no ciphertext */

		zassert_ok(build_frame_keyed(net_id, dev_addr, k_session_key, frame_type, NULL, 0,
					     counter, frame),
			   "empty-body build failed for type 0x%02x", frame_type);

		zassert_equal(sys_get_be32(&frame[0]), net_id, "net_id header wrong");
		zassert_equal(sys_get_be16(&frame[4]), dev_addr, "dev_addr header wrong");
		zassert_equal(frame[6], frame_type, "frame_type header wrong");
		zassert_equal(sys_get_be32(&frame[7]), counter, "counter header wrong");

		uint8_t nonce[P2P_NONCE_LEN];
		uint8_t empty[1];

		build_nonce(nonce, counter, dev_addr, frame_type, P2P_DIR_TX);

		zassert_ok(app_ccm_auth_decrypt(k_session_key, nonce, P2P_NONCE_LEN, frame,
						P2P_HDR_LEN, &frame[P2P_HDR_LEN], 0,
						&frame[P2P_HDR_LEN], P2P_TAG_LEN, empty),
			   "empty-body verify failed for type 0x%02x", frame_type);

		/* A flipped tag byte must fail: with no ciphertext the tag is
		 * the only thing standing between the node and a forged
		 * unpair. */
		frame[P2P_HDR_LEN] ^= 0x01;
		zassert_equal(app_ccm_auth_decrypt(k_session_key, nonce, P2P_NONCE_LEN, frame,
						   P2P_HDR_LEN, &frame[P2P_HDR_LEN], 0,
						   &frame[P2P_HDR_LEN], P2P_TAG_LEN, empty),
			      -EBADMSG, "tampered empty-body tag accepted");
		frame[P2P_HDR_LEN] ^= 0x01;

		/* Same for the header AAD -- e.g. retargeting a captured
		 * Detach at another dev_addr. */
		frame[4] ^= 0x01;
		zassert_equal(app_ccm_auth_decrypt(k_session_key, nonce, P2P_NONCE_LEN, frame,
						   P2P_HDR_LEN, &frame[P2P_HDR_LEN], 0,
						   &frame[P2P_HDR_LEN], P2P_TAG_LEN, empty),
			      -EBADMSG, "tampered empty-body AAD accepted");
	}
}

/* The frame-type numbers are a cross-repo wire constant (the central's
 * src/p2p/frame.rs::frame_type and app/decoder/p2p.js both hard-code them), so
 * a renumbering here has to fail loudly rather than desync three code bases. */
ZTEST(p2p_logic, test_frame_type_constants)
{
	zassert_equal(APP_P2P_FRAME_TELEMETRY, 0x02, "telemetry");
	zassert_equal(APP_P2P_FRAME_ALARM, 0x03, "alarm");
	zassert_equal(APP_P2P_FRAME_RESPONSE, 0x55, "response");
	zassert_equal(APP_P2P_FRAME_COMMAND, 0x56, "command");
	zassert_equal(APP_P2P_FRAME_JOIN_REQUEST, 0xF0, "join request");
	zassert_equal(APP_P2P_FRAME_JOIN_ACCEPT, 0xF1, "join accept");
	zassert_equal(APP_P2P_FRAME_ACK, 0xFA, "ack");
	zassert_equal(APP_P2P_FRAME_DETACH, 0xFD, "detach");
	zassert_equal(APP_P2P_FRAME_REJOIN_REQUEST, 0xFE, "rejoin request");
}

ZTEST(p2p_logic, test_build_frame_max_body)
{
	uint8_t body[P2P_MAX_BODY];

	for (size_t i = 0; i < sizeof(body); i++) {
		body[i] = (uint8_t)i;
	}

	uint8_t frame[P2P_FRAME_MAX];

	zassert_ok(build_frame_keyed(1, 2, k_session_key, 0x02, body, sizeof(body), 99, frame),
		   "build_frame failed at max body");

	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, 99, 2, 0x02, P2P_DIR_TX);

	uint8_t pt[P2P_MAX_BODY];

	zassert_ok(app_ccm_auth_decrypt(k_session_key, nonce, P2P_NONCE_LEN, frame, P2P_HDR_LEN,
					&frame[P2P_HDR_LEN], sizeof(body),
					&frame[P2P_HDR_LEN + sizeof(body)], P2P_TAG_LEN, pt),
		   "decrypt of max-body frame failed");
	zassert_mem_equal(pt, body, sizeof(body), "max-body recovered mismatch");
}

/* ---- Exact sliding-hour duty ledger (B2, decision D1) ----------------- */

/* The property under test is stronger than the token bucket's: not "the
 * long-run average is 1%" but "EVERY sliding one-hour window sums to <= 1%".
 * These tests drive the ledger on a virtual clock, so an hour costs no time. */

ZTEST(p2p_logic, test_duty_empty_ledger_admits)
{
	struct p2p_duty d;

	p2p_duty_init(&d);

	/* Boot is never blocked, and the whole allowance is available at once. */
	zassert_equal(p2p_duty_wait_ms(&d, 0, 1), 0, "an empty ledger must admit a small frame");
	zassert_equal(p2p_duty_wait_ms(&d, 0, P2P_DUTY_BUDGET_MS), 0,
		      "an empty ledger must admit the whole hourly allowance");
}

ZTEST(p2p_logic, test_duty_sum_enforced)
{
	struct p2p_duty d;
	const uint32_t air = P2P_DUTY_BUDGET_MS / 4; /* 9 s: four fill the hour */

	p2p_duty_init(&d);

	for (int i = 0; i < 4; i++) {
		int64_t now = i * 1000;

		zassert_equal(p2p_duty_wait_ms(&d, now, air), 0, "frame %d must be admitted", i);
		p2p_duty_charge(&d, now, air);
	}

	/* The allowance is exactly spent -- not one further millisecond of air. */
	zassert_true(p2p_duty_wait_ms(&d, 4000, 1) > 0,
		     "1 ms of air must be refused once the hour's allowance is spent");
	zassert_equal(p2p_duty_wait_ms(&d, 4000, 0), 0, "a zero-length frame is always affordable");
}

ZTEST(p2p_logic, test_duty_expiry_after_hour)
{
	struct p2p_duty d;

	p2p_duty_init(&d);
	p2p_duty_charge(&d, 0, P2P_DUTY_BUDGET_MS); /* spend it all at t=0 */

	zassert_true(p2p_duty_wait_ms(&d, 1000, 1) > 0, "still blocked one second in");

	/* One ms before the entry leaves the window: still blocked, and the
	 * reported wait is exactly the time remaining. */
	int64_t wait = p2p_duty_wait_ms(&d, P2P_DUTY_WINDOW_MS - 1, 1);

	zassert_equal(wait, 1, "wait should be 1 ms at the window edge, got %lld", wait);

	/* The instant it does, the full allowance is available again. */
	zassert_equal(p2p_duty_wait_ms(&d, P2P_DUTY_WINDOW_MS, P2P_DUTY_BUDGET_MS), 0,
		      "the allowance must return when the entry leaves the window");
}

/* The ring is finite, so a node transmitting more frames per hour than it has
 * entries runs out of slots before it runs out of air-time budget. That is
 * safe -- it can only delay a frame, never permit one the budget forbids --
 * and this pins the direction of that conservatism. */
ZTEST(p2p_logic, test_duty_ring_full_is_conservative)
{
	struct p2p_duty d;

	p2p_duty_init(&d);

	/* Fill every slot with a frame far too short to trouble the budget. */
	for (int i = 0; i < P2P_DUTY_LEDGER_ENTRIES; i++) {
		int64_t now = i * 10;

		zassert_equal(p2p_duty_wait_ms(&d, now, 1), 0, "tiny frame %d must be admitted", i);
		p2p_duty_charge(&d, now, 1);
	}

	/* Budget is barely touched, but there is no slot to record another
	 * frame in, so the ledger waits for the oldest to expire rather than
	 * forget a transmission it has already counted. */
	int64_t wait = p2p_duty_wait_ms(&d, 1000, 1);

	zassert_true(wait > 0, "a full ring must block even with budget to spare");
	zassert_equal(wait, P2P_DUTY_WINDOW_MS - 1000,
		      "the wait must be until the OLDEST entry expires, got %lld", wait);

	/* And once it does, exactly one slot frees up. */
	zassert_equal(p2p_duty_wait_ms(&d, P2P_DUTY_WINDOW_MS, 1), 0,
		      "a freed slot must admit the next frame");
}

/* Deterministic pseudo-random frame sizes: a failure has to be reproducible. */
static uint32_t duty_rand(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/* Static, not on the stack: 24 simulated hours of sends is a few thousand
 * records and this runs on native_sim. */
static struct {
	uint32_t end_ms;
	uint32_t air_ms;
} m_sent[4096];

ZTEST(p2p_logic, test_duty_sliding_hour_never_exceeds_1pct)
{
	struct p2p_duty d;
	uint32_t state = 0xC0FFEEu;
	size_t n = 0;

	p2p_duty_init(&d);

	/* Hammer the ledger for 24 simulated hours, always trying to send, with
	 * frame air-times spanning the real SF10 range (330..2296 ms). */
	for (int64_t now = 0; now <= 24 * (int64_t)P2P_DUTY_WINDOW_MS; now += 1000) {
		uint32_t air = 330 + (duty_rand(&state) % 1967);

		while (p2p_duty_wait_ms(&d, now, air) == 0) {
			p2p_duty_charge(&d, now, air);
			zassert_true(n < ARRAY_SIZE(m_sent), "test record overflow");
			m_sent[n].end_ms = (uint32_t)now;
			m_sent[n].air_ms = air;
			n++;
			air = 330 + (duty_rand(&state) % 1967);
		}
	}

	zassert_true(n > 100, "the simulation should have sent plenty, got %zu", n);

	/* The guarantee, checked directly against the record rather than the
	 * ledger's own arithmetic: for every transmission, the air radiated in
	 * the hour ending at it -- itself included -- is within the allowance. */
	for (size_t j = 0; j < n; j++) {
		uint32_t sum = 0;

		for (size_t i = 0; i <= j; i++) {
			if (m_sent[j].end_ms - m_sent[i].end_ms < P2P_DUTY_WINDOW_MS) {
				sum += m_sent[i].air_ms;
			}
		}
		zassert_true(sum <= P2P_DUTY_BUDGET_MS,
			     "sliding hour ending at send %zu (t=%u) radiated %u ms, over the "
			     "%d ms allowance",
			     j, m_sent[j].end_ms, sum, P2P_DUTY_BUDGET_MS);
	}
}

/* Uptime is truncated to 32 bits in the ledger, so entries have to survive the
 * ~49.7-day wrap. A `now >= end_ms + WINDOW` formulation breaks here; the
 * unsigned-difference one does not. */
ZTEST(p2p_logic, test_duty_wrap_safe)
{
	struct p2p_duty d;
	const int64_t t = 0xFFFFFF00LL; /* 256 ms before the u32 wrap */

	p2p_duty_init(&d);
	p2p_duty_charge(&d, t, P2P_DUTY_BUDGET_MS);

	/* Straddling the wrap, still inside the window: must stay blocked. */
	zassert_true(p2p_duty_wait_ms(&d, t + 1000, 1) > 0,
		     "an entry must still count after the uptime counter wraps");

	int64_t wait = p2p_duty_wait_ms(&d, t + 1000, 1);

	zassert_equal(wait, P2P_DUTY_WINDOW_MS - 1000, "wait wrong across the wrap: %lld", wait);

	/* And expire correctly on the far side of it. */
	zassert_equal(p2p_duty_wait_ms(&d, t + P2P_DUTY_WINDOW_MS, P2P_DUTY_BUDGET_MS), 0,
		      "the entry must expire on schedule across the wrap");
}

/* ---- JoinAccept reserved(4) radio assignment (D3) --------------------- */

/* Every unsupported or out-of-range field is warned about and ignored, never
 * refused: a JoinAccept is authenticated and otherwise valid, and declining to
 * pair over a byte this release cannot honour would strand the node. */

ZTEST(p2p_logic, test_join_accept_reserved_all_zero_is_no_assignment)
{
	const uint8_t reserved[4] = {0, 0, 0, 0};
	struct p2p_radio_assign a;

	p2p_parse_join_accept_reserved(reserved, &a);

	zassert_false(a.tx_power_assigned, "all-zero reserved must assign nothing");
	zassert_equal(a.tx_power_dbm, 0, "no assignment must leave the power at 0");
	zassert_equal(a.sf_hint, 0, "no SF hint expected");
}

ZTEST(p2p_logic, test_join_accept_reserved_assigns_tx_power)
{
	struct p2p_radio_assign a;

	/* Both ends of the configured envelope, plus a middling value. */
	const uint8_t powers[] = {P2P_TX_POWER_MIN_DBM, 8, 14, P2P_TX_POWER_MAX_DBM};

	for (size_t i = 0; i < ARRAY_SIZE(powers); i++) {
		const uint8_t reserved[4] = {0, 0, powers[i], 0};

		p2p_parse_join_accept_reserved(reserved, &a);
		zassert_true(a.tx_power_assigned, "%u dBm must be accepted", powers[i]);
		zassert_equal(a.tx_power_dbm, (int8_t)powers[i], "wrong power recorded");
	}
}

ZTEST(p2p_logic, test_join_accept_reserved_rejects_out_of_range_tx_power)
{
	struct p2p_radio_assign a;
	/* Just outside both bounds, and the 0xFF a corrupt/garbage byte gives. */
	const uint8_t bad[] = {1,   P2P_TX_POWER_MIN_DBM - 1, P2P_TX_POWER_MAX_DBM + 1, 23, 100,
			       0xFF};

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		const uint8_t reserved[4] = {0, 0, bad[i], 0};

		p2p_parse_join_accept_reserved(reserved, &a);
		zassert_false(a.tx_power_assigned, "%u dBm must be refused, not applied", bad[i]);
		zassert_equal(a.tx_power_dbm, 0, "a refused power must not leak through");
	}
}

ZTEST(p2p_logic, test_join_accept_reserved_ignores_channel_but_keeps_power)
{
	/* A nonzero channel is unsupported (one physical channel), but it must
	 * not cost the node an otherwise-valid power assignment. */
	const uint8_t reserved[4] = {3, 0, 8, 0};
	struct p2p_radio_assign a;

	p2p_parse_join_accept_reserved(reserved, &a);

	zassert_true(a.tx_power_assigned, "an unsupported channel must not veto the power");
	zassert_equal(a.tx_power_dbm, 8, "wrong power recorded");
}

ZTEST(p2p_logic, test_join_accept_reserved_records_sf_hint)
{
	/* SF is judged by the caller (only it knows the configured SF), so the
	 * parser must pass the raw byte through untouched. */
	const uint8_t reserved[4] = {0, 12, 0, 0};
	struct p2p_radio_assign a;

	p2p_parse_join_accept_reserved(reserved, &a);

	zassert_equal(a.sf_hint, 12, "SF hint must be reported verbatim");
	zassert_false(a.tx_power_assigned, "an SF hint alone assigns no power");
}

ZTEST(p2p_logic, test_join_accept_reserved_ignores_unknown_flags)
{
	const uint8_t reserved[4] = {0, 0, 14, 0xA5};
	struct p2p_radio_assign a;

	p2p_parse_join_accept_reserved(reserved, &a);

	zassert_true(a.tx_power_assigned, "unknown flags must not veto the power");
	zassert_equal(a.tx_power_dbm, 14, "wrong power recorded");
}

ZTEST(p2p_logic, test_rejoin_backoff_doubles_then_caps)
{
	/* base, 2x, 4x, ... capped at 1 h. */
	zassert_equal(p2p_rejoin_backoff_ms(0), 60000u, "attempt 0 should be the 60 s base");
	zassert_equal(p2p_rejoin_backoff_ms(1), 120000u, "attempt 1 should double to 120 s");
	zassert_equal(p2p_rejoin_backoff_ms(2), 240000u, "attempt 2 should be 240 s");
	zassert_equal(p2p_rejoin_backoff_ms(3), 480000u, "attempt 3 should be 480 s");

	/* Monotonic non-decreasing, and never above the 1 h cap, for any attempt. */
	uint32_t prev = 0;

	for (int a = 0; a <= 255; a++) {
		uint32_t ms = p2p_rejoin_backoff_ms((uint8_t)a);

		zassert_true(ms >= prev, "backoff not monotonic at attempt %d (%u < %u)", a, ms,
			     prev);
		zassert_true(ms <= 3600000u, "backoff %u at attempt %d exceeds the 1 h cap", ms, a);
		prev = ms;
	}
	zassert_equal(p2p_rejoin_backoff_ms(255), 3600000u, "a large attempt must saturate at 1 h");
}

/* ---- Frame-counter fail-closed / saturation (B9) ---------------------- */

ZTEST(p2p_logic, test_fcnt_normal_advance)
{
	uint32_t c;

	/* Within the reserved window: hands out the value and advances, no reserve
	 * needed (so no dependency on the settings backend). */
	p2p_test_set_fcnt(100, 200);
	zassert_ok(p2p_test_fcnt_next(&c), "fcnt_next should succeed within the window");
	zassert_equal(c, 100u, "counter value wrong");
	zassert_equal(p2p_test_get_fcnt(), 101u, "counter not advanced");
}

ZTEST(p2p_logic, test_fcnt_fail_closed_on_reserve_failure)
{
	uint32_t c = 0xDEADBEEF;

	/* At the window edge a durable reserve is required; with CONFIG_SETTINGS_NONE
	 * the save fails, so fcnt_next must refuse rather than hand out an
	 * unreserved counter -- and must NOT advance the counter. */
	p2p_test_set_fcnt(200, 200);
	zassert_true(p2p_test_fcnt_next(&c) != 0,
		     "fcnt_next must fail closed when the reservation can't be persisted");
	zassert_equal(p2p_test_get_fcnt(), 200u, "counter advanced despite a failed reservation");
}

ZTEST(p2p_logic, test_fcnt_saturates_no_wrap)
{
	uint32_t c = 0xDEADBEEF;

	/* At the ceiling the counter must refuse rather than wrap (a wrap repeats
	 * every (key, nonce) -- a full CCM break). */
	p2p_test_set_fcnt(UINT32_MAX, UINT32_MAX);
	zassert_equal(p2p_test_fcnt_next(&c), -EOVERFLOW,
		      "exhausted counter must return -EOVERFLOW");
	zassert_equal(p2p_test_get_fcnt(), UINT32_MAX, "counter must not wrap past UINT32_MAX");
}

/* ---- Ack body parse (B1 rssi/snr + B5 time) --------------------------- */

ZTEST(p2p_logic, test_ack_body_base)
{
	uint8_t body[P2P_ACK_BODY_BASE_LEN] = {P2P_ACK_FLAG_PENDING, (uint8_t)(int8_t)-57,
					       (uint8_t)(int8_t)9};
	struct p2p_ack_info info;

	zassert_true(p2p_parse_ack_body(body, sizeof(body), &info), "base body must parse");
	zassert_equal(info.flags, P2P_ACK_FLAG_PENDING, "flags wrong");
	zassert_equal(info.rssi, -57, "rssi wrong (signed)");
	zassert_equal(info.snr, 9, "snr wrong");
	zassert_false(info.time_present, "no time tail in a base body");
}

ZTEST(p2p_logic, test_ack_body_with_time)
{
	/* The 7 B time form, spelled out rather than as P2P_ACK_BODY_MAX_LEN:
	 * D2 grew the max to 8 (base + length + time), and this test is about
	 * the shape with no length byte. */
	uint8_t body[P2P_ACK_BODY_BASE_LEN + P2P_ACK_TIME_LEN] = {0};

	body[0] = P2P_ACK_FLAG_TIME;
	body[1] = (uint8_t)(int8_t)-110; /* rssi */
	body[2] = (uint8_t)(int8_t)-3;   /* snr */
	sys_put_be32(1735689600u, &body[3]);
	struct p2p_ack_info info;

	zassert_true(p2p_parse_ack_body(body, sizeof(body), &info), "7 B body must parse");
	zassert_equal(info.rssi, -110, "rssi wrong");
	zassert_equal(info.snr, -3, "snr wrong");
	zassert_true(info.time_present, "time tail must be reported present");
	zassert_equal(info.unix_time, 1735689600u, "unix time decoded wrong");
}

ZTEST(p2p_logic, test_ack_body_time_bit_without_tail_is_ignored)
{
	/* 3-byte body but the time bit set: no tail bytes, so time must NOT be
	 * reported present (length is authoritative, not the flag). */
	uint8_t body[P2P_ACK_BODY_BASE_LEN] = {P2P_ACK_FLAG_TIME, 0, 0};
	struct p2p_ack_info info;

	zassert_true(p2p_parse_ack_body(body, sizeof(body), &info), "base body must parse");
	zassert_false(info.time_present, "time flag with no tail must be ignored");
}

/* D2: the announcing Ack states the pending 0x56's on-air length so the node
 * can size its RX1 window exactly. 4 B = base + length. */
ZTEST(p2p_logic, test_ack_body_pending_len)
{
	uint8_t body[4] = {P2P_ACK_FLAG_PENDING, (uint8_t)(int8_t)-71, (uint8_t)(int8_t)7, 17};
	struct p2p_ack_info info;

	zassert_true(p2p_parse_ack_body(body, sizeof(body), &info), "4 B body must parse");
	zassert_equal(info.rssi, -71, "rssi wrong");
	zassert_equal(info.snr, 7, "snr wrong");
	zassert_true(info.pending_len_present, "length byte must be reported present");
	zassert_equal(info.pending_frame_len, 17, "a 2 B command is 11+2+4 = 17 B on air");
	zassert_false(info.time_present, "no time tail in a 4 B body");
}

/* 8 B = base + length + time: the length byte precedes the tail, so a parser
 * that read the tail from a fixed offset 3 would decode garbage. */
ZTEST(p2p_logic, test_ack_body_pending_len_with_time)
{
	uint8_t body[P2P_ACK_BODY_MAX_LEN] = {0};
	struct p2p_ack_info info;

	body[0] = P2P_ACK_FLAG_PENDING | P2P_ACK_FLAG_TIME;
	body[1] = (uint8_t)(int8_t)-110;
	body[2] = (uint8_t)(int8_t)-3;
	body[3] = 55;
	sys_put_be32(1735689600u, &body[4]);

	zassert_true(p2p_parse_ack_body(body, sizeof(body), &info), "8 B body must parse");
	zassert_equal(info.rssi, -110, "rssi wrong");
	zassert_equal(info.snr, -3, "snr wrong");
	zassert_true(info.pending_len_present, "length byte must be present");
	zassert_equal(info.pending_frame_len, 55, "length byte wrong");
	zassert_true(info.time_present, "time tail must be present");
	zassert_equal(info.unix_time, 1735689600u, "time tail decoded from the wrong offset");
}

/* Transitional tolerance: a central still emitting the pre-D2 body with bit 0
 * set means "pending, length unknown" -- it must parse, with the caller left
 * to fall back to the 255 B worst-case window. Both legacy shapes.
 */
ZTEST(p2p_logic, test_ack_body_legacy_pending_without_len_still_parses)
{
	struct p2p_ack_info info;
	uint8_t base[P2P_ACK_BODY_BASE_LEN] = {P2P_ACK_FLAG_PENDING, 0, 0};

	zassert_true(p2p_parse_ack_body(base, sizeof(base), &info), "legacy 3 B must parse");
	zassert_equal(info.flags & P2P_ACK_FLAG_PENDING, P2P_ACK_FLAG_PENDING, "pending lost");
	zassert_false(info.pending_len_present, "3 B body cannot carry a length");
	zassert_equal(info.pending_frame_len, 0, "length must read 0 when absent");

	uint8_t timed[P2P_ACK_BODY_BASE_LEN + P2P_ACK_TIME_LEN] = {0};

	timed[0] = P2P_ACK_FLAG_PENDING | P2P_ACK_FLAG_TIME;
	sys_put_be32(1735689600u, &timed[3]);

	zassert_true(p2p_parse_ack_body(timed, sizeof(timed), &info), "legacy 7 B must parse");
	zassert_false(info.pending_len_present, "7 B body cannot carry a length");
	zassert_true(info.time_present, "the 7 B tail is still a time tail");
	zassert_equal(info.unix_time, 1735689600u, "legacy time tail decoded wrong");
}

ZTEST(p2p_logic, test_ack_body_bad_length_rejected)
{
	uint8_t body[16] = {P2P_ACK_FLAG_PENDING | P2P_ACK_FLAG_TIME, 0, 0, 17, 0, 0, 0, 0};
	struct p2p_ack_info info;

	/* Only 3, 4, 7 and 8 are valid shapes. */
	zassert_false(p2p_parse_ack_body(body, 0, &info), "0 B body must be rejected");
	zassert_false(p2p_parse_ack_body(body, 1, &info), "1 B body must be rejected");
	zassert_false(p2p_parse_ack_body(body, 2, &info), "2 B body must be rejected");
	zassert_false(p2p_parse_ack_body(body, 5, &info), "5 B body must be rejected");
	zassert_false(p2p_parse_ack_body(body, 6, &info), "6 B body must be rejected");
	zassert_false(p2p_parse_ack_body(body, 9, &info), "9 B body must be rejected");
	zassert_false(p2p_parse_ack_body(body, P2P_ACK_BODY_MAX_LEN + 1, &info),
		      "over-long body must be rejected");

	/* A length byte is only meaningful when a downlink is pending: the 4 and
	 * 8 B shapes require bit 0, or the byte is unexplained. */
	uint8_t no_pending[P2P_ACK_BODY_MAX_LEN] = {P2P_ACK_FLAG_TIME, 0, 0, 17, 0, 0, 0, 0};

	zassert_false(p2p_parse_ack_body(no_pending, 4, &info),
		      "4 B without the pending bit must be rejected");
	zassert_false(p2p_parse_ack_body(no_pending, 8, &info),
		      "8 B without the pending bit must be rejected");
}

ZTEST_SUITE(p2p_logic, NULL, NULL, NULL, NULL, NULL);
