/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host reproduction of the #348 HIL "STATE edge never fires" finding
 * (project_issue348_alarm_dwell_unify.md, real bug #3): a STATE edge rule
 * (from != to) on hall-left did not fire on the bench even after the poll-
 * resolution fix (bug #1) that made STATE level work. This drives
 * app_alarm_event()/app_alarm_poll() directly against real app_alarm.c, with
 * app_hall_get_data() stubbed so the test controls the GPIO level exactly
 * like the real driver would report it — real k_uptime_get()/k_sleep() time,
 * no fake clock, so the dwell/confirm timing is exercised for real.
 */

#include "app_alarm.h"
#include "app_alarm_rules.h"
#include "app_config.h"
#include "app_hall.h"
#include "app_sensor.h"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

extern struct app_hall_data test_hall;

static void before(void *unused)
{
	ARG_UNUSED(unused);
	app_alarm_rules_clear_all();
	test_hall = (struct app_hall_data){0};
	g_app_sensor_data = (struct app_sensor_data){0};
	g_app_config.interval_report = 0;
	/* app_alarm.c's per-slot runtime latch (m_rt[]) is static file-scope state
	 * that outlives a single ztest case. rt_sync() only resets a slot when its
	 * (source, quantity) changes or the slot was never used — several tests
	 * here reuse the same (source, quantity) across cases (only from/to or
	 * quantity/source combos vary within a family), so without this poll a
	 * leftover latch from a previous test could leak in and falsely look like
	 * "fired immediately". Polling once here, with no rule present yet, drives
	 * rt_sync()'s "rule cleared" branch for every slot, which does reset it. */
	app_alarm_poll();
}

ZTEST_SUITE(alarm_eval, NULL, NULL, before, NULL, NULL);

static void set_hall_left_edge_rule(uint8_t from, uint8_t to, float hst_s)
{
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_HALL_LEFT,
		.quantity = APP_ALARM_Q_STATE,
		.enabled = 1,
		.from_state = from,
		.to_state = to,
		.hst = hst_s,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");
}

/* Control case: STATE LEVEL (from == to) on hall-left, resolved purely via
 * app_alarm_poll() — this is the path bug #1 fixed and HIL-confirmed working.
 * If this fails too, the harness itself (not the edge logic) is suspect. */
ZTEST(alarm_eval, test_state_level_fires_after_confirm_via_poll)
{
	set_hall_left_edge_rule(1, 1, 0.3f);

	test_hall.left_is_active = true; /* already at the target level */
	app_alarm_poll();                /* arms the confirm deadline */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		      "level fired before the dwell elapsed");

	k_sleep(K_MSEC(400));
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		     "level never fired after the dwell elapsed");
}

/* The #348 HIL finding: STATE EDGE (from != to) on hall-left, raw transition
 * delivered via app_alarm_event() (the real GPIO-callback path), resolved via
 * a later app_alarm_poll() (the real ~3 s main-loop cadence) since the level
 * holds steady with no second edge to re-invoke eval_state(). */
ZTEST(alarm_eval, test_state_edge_fires_after_confirm_via_poll)
{
	set_hall_left_edge_rule(0, 1, 0.3f);

	test_hall.left_is_active = false;
	app_alarm_event(APP_ALARM_SRC_HALL_LEFT, false); /* seed prev_state = 0 */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		      "spuriously active before any transition");

	test_hall.left_is_active = true;
	app_alarm_event(APP_ALARM_SRC_HALL_LEFT, true); /* raw 0->1 transition: arms confirm */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		      "edge fired before the confirm dwell elapsed");

	k_sleep(K_MSEC(400)); /* past hst, level still held (no second edge) */
	app_alarm_poll();     /* the real re-check path: read_poll_state() -> eval_state() */
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		     "edge never fired after the confirm dwell elapsed (#348 HIL bug #3)");
}

/* Variant: the raw transition is observed ONLY through app_alarm_poll()'s
 * ~3 s cadence (read_poll_state() -> eval_state()), never through
 * app_alarm_event() — models a missed/debounced GPIO edge callback where the
 * level nonetheless changed by the time the next poll ran. eval_state() does
 * not care which caller delivered `cur`, so this should behave identically to
 * the event-driven case above. */
ZTEST(alarm_eval, test_state_edge_fires_via_poll_only_no_event)
{
	set_hall_left_edge_rule(0, 1, 0.3f);

	test_hall.left_is_active = false;
	app_alarm_poll(); /* seeds prev_state = 0, no transition yet */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		      "spuriously active before any transition");

	test_hall.left_is_active = true;
	app_alarm_poll(); /* poll observes the 0->1 change directly: arms confirm */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		      "edge fired before the confirm dwell elapsed (poll-only path)");

	k_sleep(K_MSEC(400));
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		     "edge never fired after the confirm dwell elapsed (poll-only path)");
}

/* Early-revert: releasing before the confirm dwell elapses must cancel the
 * armed transition outright, not just pause it (the #348 design's core
 * "continuous dwell" guarantee, mirrored from the THRESHOLD side). */
ZTEST(alarm_eval, test_state_edge_early_revert_cancels_confirm)
{
	set_hall_left_edge_rule(0, 1, 0.3f);

	test_hall.left_is_active = false;
	app_alarm_event(APP_ALARM_SRC_HALL_LEFT, false);
	test_hall.left_is_active = true;
	app_alarm_event(APP_ALARM_SRC_HALL_LEFT, true); /* arms confirm */

	k_sleep(K_MSEC(100)); /* well short of the 300 ms dwell */
	test_hall.left_is_active = false;
	app_alarm_event(APP_ALARM_SRC_HALL_LEFT, false); /* reverted: cancel */

	k_sleep(K_MSEC(400)); /* past the ORIGINAL deadline */
	app_alarm_poll();
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		      "reverted edge fired anyway — confirm window was paused, not reset");
}

/* Momentary source (PIR): fires immediately on the pulse (no confirm — the
 * source already reports a discrete event, not a raw level), then holds for
 * `hst` seconds before it can re-arm. Not HIL-tested in the #348 session. */
ZTEST(alarm_eval, test_momentary_pir_fires_immediately_then_holds)
{
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_PIR,
		.quantity = APP_ALARM_Q_STATE,
		.enabled = 1,
		.from_state = 0,
		.to_state = 1,
		.hst = 0.3f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	app_alarm_event(APP_ALARM_SRC_PIR, true); /* pulse: fires on this same call */
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_PIR, APP_ALARM_Q_STATE),
		     "momentary source did not fire immediately");

	app_alarm_event(APP_ALARM_SRC_PIR, true); /* a second pulse within the hold */
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_PIR, APP_ALARM_Q_STATE),
		     "held pulse dropped active early");

	k_sleep(K_MSEC(400)); /* past the hold */
	app_alarm_poll();     /* central oneshot-expiry sweep clears it */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_PIR, APP_ALARM_Q_STATE),
		      "momentary source never re-armed after the hold elapsed");
}

/* RATE (count): fires when the counter delta within one interval_report
 * window reaches `hi`, then holds/re-arm-blocks for `hst` seconds — same
 * hold/re-arm role as a momentary source. Not HIL-tested in the #348
 * session. */
ZTEST(alarm_eval, test_rate_count_fires_after_window_then_holds)
{
	g_app_config.interval_report = 1; /* 1 s tumbling window */
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_HALL_LEFT,
		.quantity = APP_ALARM_Q_COUNT,
		.enabled = 1,
		.hi = 2, /* alarm when the window's delta >= 2 */
		.hst = 0.3f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	g_app_sensor_data.hall_left_count = 0;
	app_alarm_poll(); /* first poll only seeds the baseline + window start */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_COUNT),
		      "spuriously active before any window elapsed");

	g_app_sensor_data.hall_left_count = 3; /* 3 pulses within this window */
	k_sleep(K_MSEC(1100));                 /* past the 1 s window */
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_COUNT),
		     "rate alarm never fired after an over-limit window elapsed");

	k_sleep(K_MSEC(400)); /* past the hold, but still within the SAME 1 s window */
	app_alarm_poll();
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_COUNT),
		      "rate alarm never re-armed after the hold elapsed");
}

/* THRESHOLD (onboard temperature): dwell before activating outside [lo, hi],
 * immediate deactivate, and the early-revert-resets-the-window guarantee —
 * the HIL-confirmed behavior (project_issue348_alarm_dwell_unify.md), now
 * also covered deterministically. */
ZTEST(alarm_eval, test_threshold_dwell_activate_immediate_deactivate)
{
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_ONBOARD,
		.quantity = APP_ALARM_Q_TEMPERATURE,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 30.0f,
		.hst = 0.3f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	g_app_sensor_data.temperature = 35.0f; /* above hi: arms the dwell */
	app_alarm_poll();
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_TEMPERATURE),
		      "threshold fired before the dwell elapsed");

	k_sleep(K_MSEC(400));
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_TEMPERATURE),
		     "threshold never fired after the dwell elapsed");

	g_app_sensor_data.temperature = 20.0f; /* back in band */
	app_alarm_poll();
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_TEMPERATURE),
		      "threshold deactivation was not immediate");
}

ZTEST(alarm_eval, test_threshold_early_revert_resets_window)
{
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_ONBOARD,
		.quantity = APP_ALARM_Q_TEMPERATURE,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 30.0f,
		.hst = 0.3f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	g_app_sensor_data.temperature = 35.0f; /* arm the dwell */
	app_alarm_poll();

	k_sleep(K_MSEC(100));                  /* well short of the 300 ms dwell */
	g_app_sensor_data.temperature = 20.0f; /* reverts: back in band, cancel */
	app_alarm_poll();
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_TEMPERATURE),
		      "revert did not cancel the pending dwell");

	k_sleep(K_MSEC(400)); /* past the ORIGINAL deadline */
	app_alarm_poll();
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_TEMPERATURE),
		      "reverted threshold fired anyway — window was paused, not reset");
}
