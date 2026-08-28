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

/* ---- Duty-cycle governor (B2) ----------------------------------------- */

ZTEST(p2p_logic, test_duty_starts_full)
{
	struct p2p_duty d;

	p2p_duty_init(&d, 1000);

	/* A full-hour budget worth of air-time can go immediately after init. */
	zassert_equal(p2p_duty_wait_ms(&d, 1000, P2P_DUTY_BUDGET_MS), 0,
		      "a full bucket must afford the whole budget at once");
}

ZTEST(p2p_logic, test_duty_charge_then_block)
{
	struct p2p_duty d;

	p2p_duty_init(&d, 0);

	/* Drain the entire bucket with one big charge. */
	p2p_duty_charge(&d, 0, P2P_DUTY_BUDGET_MS);

	/* Now even a 1 ms frame must wait ~100 ms (10 us/ms refill). */
	int64_t wait = p2p_duty_wait_ms(&d, 0, 1);

	zassert_equal(wait, 100, "1 ms of air after a full drain should need 100 ms, got %lld",
		      wait);
}

ZTEST(p2p_logic, test_duty_refill_accrues)
{
	struct p2p_duty d;

	p2p_duty_init(&d, 0);
	p2p_duty_charge(&d, 0, P2P_DUTY_BUDGET_MS); /* empty */

	/* After 1 s of wall time, 10 ms of air-time budget has accrued. */
	zassert_equal(p2p_duty_wait_ms(&d, 1000, 10), 0,
		      "10 ms air should be affordable after 1 s");
	zassert_true(p2p_duty_wait_ms(&d, 1000, 11) > 0, "11 ms air should not yet be affordable");
}

ZTEST(p2p_logic, test_duty_refill_caps_at_budget)
{
	struct p2p_duty d;

	p2p_duty_init(&d, 0);
	p2p_duty_charge(&d, 0, P2P_DUTY_BUDGET_MS); /* empty */

	/* Idle far longer than a full recharge (10x the hour): budget must cap,
	 * not overflow into a larger-than-full burst allowance. */
	int64_t long_idle = (int64_t)P2P_DUTY_BUDGET_MS * 100 * 10;

	zassert_equal(p2p_duty_wait_ms(&d, long_idle, P2P_DUTY_BUDGET_MS), 0,
		      "capped bucket must afford exactly the full budget");
	zassert_true(p2p_duty_wait_ms(&d, long_idle, P2P_DUTY_BUDGET_MS + 1) > 0,
		     "capped bucket must NOT afford more than the full budget");
}

ZTEST(p2p_logic, test_duty_long_run_stays_within_1pct)
{
	struct p2p_duty d;

	p2p_duty_init(&d, 0);

	/* Empty the initial full bucket so it does not inflate the accounting,
	 * then hammer sends for a simulated hour and confirm the air-time
	 * actually transmitted never exceeds the 1% budget for that window. */
	p2p_duty_charge(&d, 0, P2P_DUTY_BUDGET_MS);

	const int64_t window_ms = 3600LL * 1000; /* one hour */
	const uint32_t air_per_send = 500;       /* a typical telemetry frame */
	int64_t sent_air_ms = 0;

	for (int64_t now = 0; now <= window_ms; now += 1000) {
		/* Try to send as many frames as the bucket currently allows. */
		while (p2p_duty_wait_ms(&d, now, air_per_send) == 0) {
			p2p_duty_charge(&d, now, air_per_send);
			sent_air_ms += air_per_send;
		}
	}

	/* Over one hour at 1% the air budget is P2P_DUTY_BUDGET_MS; allow one
	 * extra frame's slack for the boundary. */
	zassert_true(sent_air_ms <= P2P_DUTY_BUDGET_MS + air_per_send,
		     "sent %lld ms of air in an hour, over the 1%% budget of %d ms", sent_air_ms,
		     P2P_DUTY_BUDGET_MS);
}

ZTEST(p2p_logic, test_duty_burst_after_idle)
{
	struct p2p_duty d;

	p2p_duty_init(&d, 5000); /* full at boot */

	/* Tower-style: a full bucket permits a burst up to the whole budget in
	 * one go (this is the deliberate ~2% worst-case, doc/p2p.md §6). */
	zassert_equal(p2p_duty_wait_ms(&d, 5000, P2P_DUTY_BUDGET_MS), 0,
		      "a full bucket must permit a full-budget burst");
	p2p_duty_charge(&d, 5000, P2P_DUTY_BUDGET_MS);
	zassert_true(p2p_duty_wait_ms(&d, 5000, 1) > 0, "after the burst the bucket must be empty");
}

/* ---- Self-healing rejoin backoff (B3) --------------------------------- */

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

ZTEST_SUITE(p2p_logic, NULL, NULL, NULL, NULL, NULL);
