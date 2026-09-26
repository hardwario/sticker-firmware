/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for the M-2 stale-uplink watchdog decision (app_lrw_stale.c):
 * a joined station with no telemetry uplink for APP_LRW_STALE_FACTOR report
 * intervals is forced to rejoin, except while the EU868 duty cycle is refusing
 * its sends (F29) — a rejoin there would reset the band credits and bypass the
 * 1 % limit. The hold is bounded so a MAC stuck in "restricted" still rejoins.
 */

#include "app_lrw_stale.h"

#include <zephyr/ztest.h>

#include <errno.h>
#include <stdint.h>

#define I   60u     /* interval_report (s) */
#define IMS 60000LL /* interval_report (ms) */
#define T0  1000000LL

ZTEST_SUITE(lrw_stale, NULL, NULL, NULL, NULL, NULL);

ZTEST(lrw_stale, test_no_clock_no_decision)
{
	struct app_lrw_stale_dc dc = {0};

	zassert_equal(app_lrw_stale_check(T0, 0, &dc, I), APP_LRW_STALE_OK, "no uplink yet");
	zassert_equal(app_lrw_stale_check(T0 + 100 * IMS, T0, &dc, 0), APP_LRW_STALE_OK,
		      "no cadence");
}

ZTEST(lrw_stale, test_fresh_and_stale_without_duty_cycle)
{
	struct app_lrw_stale_dc dc = {0};

	zassert_equal(app_lrw_stale_check(T0 + 4 * IMS, T0, &dc, I), APP_LRW_STALE_OK,
		      "exactly factor x interval is not stale yet");
	zassert_equal(app_lrw_stale_check(T0 + 4 * IMS + 1, T0, &dc, I), APP_LRW_STALE_REJOIN,
		      "mute station without a duty-cycle excuse rejoins (M-2 unchanged)");
}

ZTEST(lrw_stale, test_note_send_tracks_streak)
{
	struct app_lrw_stale_dc dc = {0};

	app_lrw_stale_note_send(&dc, -ECONNREFUSED, T0);
	zassert_equal(dc.since_ms, T0);
	zassert_equal(dc.last_ms, T0);
	app_lrw_stale_note_send(&dc, -ECONNREFUSED, T0 + 15000);
	zassert_equal(dc.since_ms, T0, "streak start kept");
	zassert_equal(dc.last_ms, T0 + 15000);

	/* Other errors (MAC busy, timeout) neither start nor clear the streak. */
	app_lrw_stale_note_send(&dc, -EBUSY, T0 + 30000);
	zassert_equal(dc.since_ms, T0);
	zassert_equal(dc.last_ms, T0 + 15000);

	app_lrw_stale_note_send(&dc, 0, T0 + 45000);
	zassert_equal(dc.since_ms, 0, "a successful send ends the streak");
	zassert_equal(dc.last_ms, 0);

	struct app_lrw_stale_dc fresh = {0};

	app_lrw_stale_note_send(&fresh, -EBUSY, T0);
	zassert_equal(fresh.since_ms, 0, "a non-duty-cycle error does not start a streak");
}

/* HIL 2026-09-25 (0413, DR0, 60 s): the band credits ran out at 17:57Z, every
 * send was refused, and M-2 forced a rejoin 4 intervals later — which reset the
 * credits. With refusals recent, the watchdog must hold instead. */
ZTEST(lrw_stale, test_duty_cycle_refusals_hold)
{
	struct app_lrw_stale_dc dc = {0};
	int64_t last_ok = T0;

	/* Refused every 15 s from one interval after the last good uplink. */
	for (int64_t t = T0 + IMS; t <= T0 + 30 * IMS; t += 15000) {
		app_lrw_stale_note_send(&dc, -ECONNREFUSED, t);
		zassert_not_equal(app_lrw_stale_check(t, last_ok, &dc, I), APP_LRW_STALE_REJOIN,
				  "rejoin at +%lld ms while throttled", t - T0);
	}
	zassert_equal(app_lrw_stale_check(T0 + 30 * IMS, last_ok, &dc, I), APP_LRW_STALE_HOLD_DC);

	/* Credits back: the next send succeeds and M-2 is quiet again. */
	app_lrw_stale_note_send(&dc, 0, T0 + 31 * IMS);
	last_ok = T0 + 31 * IMS;
	zassert_equal(app_lrw_stale_check(T0 + 32 * IMS, last_ok, &dc, I), APP_LRW_STALE_OK);
}

/* A refusal long ago is no excuse: if the station then goes mute for another
 * reason (no attempts at all), M-2 must still fire. */
ZTEST(lrw_stale, test_old_refusal_does_not_hold)
{
	struct app_lrw_stale_dc dc = {0};

	app_lrw_stale_note_send(&dc, -ECONNREFUSED, T0 + IMS);
	int64_t now = T0 + IMS + IMS + APP_LRW_STALE_DC_RECENT_MARGIN_MS + 1;

	zassert_true(now - T0 > 4 * IMS, "stale by then");
	zassert_equal(app_lrw_stale_check(now, T0, &dc, I), APP_LRW_STALE_REJOIN);
}

/* The hold is bounded by the duty-cycle window: a MAC that keeps refusing past
 * it is not waiting for credits any more, so M-2 rejoins. */
ZTEST(lrw_stale, test_hold_bounded_by_window)
{
	struct app_lrw_stale_dc dc = {0};
	int64_t t;

	for (t = T0 + IMS; t < T0 + IMS + APP_LRW_STALE_DC_HOLD_MAX_MS; t += 15000) {
		app_lrw_stale_note_send(&dc, -ECONNREFUSED, t);
	}
	app_lrw_stale_note_send(&dc, -ECONNREFUSED, t);
	zassert_equal(app_lrw_stale_check(t, T0, &dc, I), APP_LRW_STALE_REJOIN,
		      "refused for longer than the duty-cycle window");
}

/* At a 900 s interval the telemetry retry chain (8 x 15 s) ends long before the
 * next report; a refusal from the previous cycle still counts as recent. */
ZTEST(lrw_stale, test_long_interval_recent_window)
{
	struct app_lrw_stale_dc dc = {0};
	const uint32_t I900 = 900;
	const int64_t I900MS = 900000LL;

	app_lrw_stale_note_send(&dc, -ECONNREFUSED, T0 + 3 * I900MS);
	app_lrw_stale_note_send(&dc, -ECONNREFUSED, T0 + 4 * I900MS);
	zassert_equal(app_lrw_stale_check(T0 + 4 * I900MS + 600000, T0, &dc, I900),
		      APP_LRW_STALE_HOLD_DC);
}
