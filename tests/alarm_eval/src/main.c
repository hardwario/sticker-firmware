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
#include "app_buzzer.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_hall.h"
#include "app_radio.h"
#include "app_sensor.h"
#include "app_sensor_types.h"
#include "app_w1_slots.h"

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

extern struct app_hall_data test_hall;
extern int g_buzzer_play_calls;
extern uint32_t g_buzzer_play_last_kind;
extern uint16_t g_buzzer_play_last_repeat_s;
extern size_t test_alarm_max_events;
extern int32_t test_radio_data_hold_ms;
extern uint32_t test_radio_alarm_free;
extern size_t test_alarm_frames;
extern bool test_w1_configured[APP_W1_SLOT_COUNT];
extern enum app_w1_slot_state test_w1_state[APP_W1_SLOT_COUNT];
extern uint8_t test_w1_expected[APP_W1_SLOT_COUNT];
extern uint8_t test_w1_detected[APP_W1_SLOT_COUNT];

static void before(void *unused)
{
	ARG_UNUSED(unused);
	app_alarm_rules_clear_all();
	test_hall = (struct app_hall_data){0};
	g_app_sensor_data = (struct app_sensor_data){0};
	g_app_config.interval_report = 0;
	g_app_config.cap_buzzer = false;
	g_app_config.alarm_buzzer_mode = APP_CONFIG_ALARM_BUZZER_MODE_OFF;
	g_buzzer_play_calls = 0;
	g_buzzer_play_last_kind = 0;
	g_buzzer_play_last_repeat_s = 0;
	test_alarm_max_events = SIZE_MAX;
	test_radio_data_hold_ms = 0;
	test_radio_alarm_free = APP_RADIO_TX_QUEUE_DEPTH;
	g_app_config.cap_w1_sensors = false;
	/* Motherboard rules are inert while their channel's capability is off
	 * (#430); the cases here exercise the evaluator, so enable them all. */
	g_app_config.cap_sht = true;
	g_app_config.cap_barometer = true;
	g_app_config.cap_hall_left = true;
	g_app_config.cap_pir_detector = true;
	g_app_config.sensor1_type = 0;
	g_app_config.sensor2_type = 0;
	memset(test_w1_configured, 0, sizeof(test_w1_configured));
	memset(test_w1_state, 0, sizeof(test_w1_state));
	memset(test_w1_expected, 0, sizeof(test_w1_expected));
	memset(test_w1_detected, 0, sizeof(test_w1_detected));
	/* app_alarm.c's per-slot runtime latch (m_rt[]) is static file-scope state
	 * that outlives a single ztest case. rt_sync() only resets a slot when its
	 * rule changes or the slot was never used — several tests
	 * here reuse the same (slot, channel) across cases (only from/to or
	 * quantity/source combos vary within a family), so without this poll a
	 * leftover latch from a previous test could leak in and falsely look like
	 * "fired immediately". Polling once here, with no rule present yet, drives
	 * rt_sync()'s "rule cleared" branch for every slot, which does reset it. */
	app_alarm_poll();
}

ZTEST_SUITE(alarm_eval, NULL, NULL, before, NULL, NULL);

/* True if an alarm on (slot, channel) is latched (#430: replaces the old
 * slot/channel lookup; any alarm type). */
static bool is_active(uint8_t slot, uint8_t channel)
{
	struct app_alarm_active act[APP_ALARM_RULE_COUNT + APP_ALARM_WATCHDOG_MAX];
	size_t n = app_alarm_active_snapshot(act, ARRAY_SIZE(act));

	for (size_t i = 0; i < n; i++) {
		if (act[i].slot == slot && act[i].channel == channel) {
			return true;
		}
	}
	return false;
}

static void set_hall_left_edge_rule(uint8_t from, uint8_t to, float dwell_s)
{
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.from_state = from,
		.to_state = to,
		.dwell = dwell_s,
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
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "level fired before the dwell elapsed");

	k_sleep(K_MSEC(400));
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
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
	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE, false); /* seed prev_state = 0 */
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "spuriously active before any transition");

	test_hall.left_is_active = true;
	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE, true); /* raw 0->1 transition: arms confirm */
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "edge fired before the confirm dwell elapsed");

	k_sleep(K_MSEC(400)); /* past dwell, level still held (no second edge) */
	app_alarm_poll();     /* the real re-check path: read_poll_state() -> eval_state() */
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		     "edge never fired after the confirm dwell elapsed (#348 HIL bug #3)");
}

/* Reverse direction: STATE EDGE armed on a 1->0 transition (alarm-on-removal,
 * e.g. "magnet taken away"), not just the 0->1 case above. eval_state()'s
 * comparisons are symmetric in from_state/to_state, but this was only
 * confirmed on real hardware after an HIL retest scare (project_
 * issue348_alarm_dwell_unify.md, 2026-08-13 session) — worth a dedicated
 * regression test rather than relying on the 0->1 case as a stand-in. */
ZTEST(alarm_eval, test_state_edge_reverse_direction_fires_after_confirm)
{
	set_hall_left_edge_rule(1, 0, 0.3f);

	test_hall.left_is_active = true;
	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE, true); /* seed prev_state = 1 */
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "spuriously active before any transition");

	test_hall.left_is_active = false;
	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE, false); /* raw 1->0 transition: arms confirm */
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "edge fired before the confirm dwell elapsed");

	k_sleep(K_MSEC(400)); /* past dwell, level still held (no second edge) */
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		     "reverse-direction (1->0) edge never fired after the confirm dwell elapsed");

	k_sleep(K_MSEC(400)); /* past the hold */
	app_alarm_poll();
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "reverse-direction edge never re-armed after the hold elapsed");
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
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "spuriously active before any transition");

	test_hall.left_is_active = true;
	app_alarm_poll(); /* poll observes the 0->1 change directly: arms confirm */
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "edge fired before the confirm dwell elapsed (poll-only path)");

	k_sleep(K_MSEC(400));
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		     "edge never fired after the confirm dwell elapsed (poll-only path)");
}

/* Early-revert: releasing before the confirm dwell elapses must cancel the
 * armed transition outright, not just pause it (the #348 design's core
 * "continuous dwell" guarantee, mirrored from the THRESHOLD side). */
ZTEST(alarm_eval, test_state_edge_early_revert_cancels_confirm)
{
	set_hall_left_edge_rule(0, 1, 0.3f);

	test_hall.left_is_active = false;
	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE, false);
	test_hall.left_is_active = true;
	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE, true); /* arms confirm */

	k_sleep(K_MSEC(100)); /* well short of the 300 ms dwell */
	test_hall.left_is_active = false;
	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE, false); /* reverted: cancel */

	k_sleep(K_MSEC(400)); /* past the ORIGINAL deadline */
	app_alarm_poll();
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "reverted edge fired anyway — confirm window was paused, not reset");
}

/* Momentary source (PIR): fires immediately on the pulse (no confirm — the
 * source already reports a discrete event, not a raw level), then holds for
 * `dwell` seconds before it can re-arm. Not HIL-tested in the #348 session. */
ZTEST(alarm_eval, test_momentary_pir_fires_immediately_then_holds)
{
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.from_state = 0,
		.to_state = 1,
		.dwell = 0.3f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION, true); /* pulse: fires on this same call */
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION),
		     "momentary source did not fire immediately");

	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION, true); /* a second pulse within the hold */
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION),
		     "held pulse dropped active early");

	k_sleep(K_MSEC(400)); /* past the hold */
	app_alarm_poll();     /* central oneshot-expiry sweep clears it */
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION),
		      "momentary source never re-armed after the hold elapsed");
}

/* RATE (count): fires when the counter delta within one interval_report
 * window reaches `hi`, then holds/re-arm-blocks for `dwell` seconds — same
 * hold/re-arm role as a momentary source. Not HIL-tested in the #348
 * session. */
ZTEST(alarm_eval, test_rate_count_fires_after_window_then_holds)
{
	g_app_config.interval_report = 1; /* 1 s tumbling window */
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.hi = 2, /* alarm when the window's delta >= 2 */
		.dwell = 0.3f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	APP_SENSOR_MB_U(&g_app_sensor_data, HALL_LEFT_COUNT) = 0;
	app_alarm_poll(); /* first poll only seeds the baseline + window start */
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT),
		      "spuriously active before any window elapsed");

	APP_SENSOR_MB_U(&g_app_sensor_data, HALL_LEFT_COUNT) = 3; /* 3 pulses within this window */
	k_sleep(K_MSEC(1100));                                    /* past the 1 s window */
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT),
		     "rate alarm never fired after an over-limit window elapsed");

	k_sleep(K_MSEC(400)); /* past the hold, but still within the SAME 1 s window */
	app_alarm_poll();
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT),
		      "rate alarm never re-armed after the hold elapsed");
}

/* THRESHOLD (onboard temperature): dwell before activating outside [lo, hi],
 * immediate deactivate, and the early-revert-resets-the-window guarantee —
 * the HIL-confirmed behavior (project_issue348_alarm_dwell_unify.md), now
 * also covered deterministically. */
ZTEST(alarm_eval, test_threshold_dwell_activate_immediate_deactivate)
{
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 30.0f,
		.dwell = 0.3f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) = 35.0f; /* above hi: arms the dwell */
	app_alarm_poll();
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE),
		      "threshold fired before the dwell elapsed");

	k_sleep(K_MSEC(400));
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE),
		     "threshold never fired after the dwell elapsed");

	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) = 20.0f; /* back in band */
	app_alarm_poll();
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE),
		      "threshold deactivation was not immediate");
}

ZTEST(alarm_eval, test_threshold_early_revert_resets_window)
{
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 30.0f,
		.dwell = 0.3f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) = 35.0f; /* arm the dwell */
	app_alarm_poll();

	k_sleep(K_MSEC(100)); /* well short of the 300 ms dwell */
	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) =
		20.0f; /* reverts: back in band, cancel */
	app_alarm_poll();
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE),
		      "revert did not cancel the pending dwell");

	k_sleep(K_MSEC(400)); /* past the ORIGINAL deadline */
	app_alarm_poll();
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE),
		      "reverted threshold fired anyway — window was paused, not reset");
}

/* Bug A regression (today's PR#349 same-day finding): rt_sync() must reset the
 * per-slot runtime latch (m_rt[]) on ANY rule edit, not just a slot/channel
 * change. Before the fix, rt_sync() compared only slot+channel, so a live
 * edit that keeps those the same but changes e.g. dwell left a stale
 * confirm_deadline/active latch from the OLD rule in place.
 *
 * Demonstrated here with a STATE level rule on hall-left: arm a confirm
 * deadline under a LONG dwell (2.0 s), then edit the SAME slot (same
 * slot+channel) to a SHORT dwell (0.1 s) before the long one would ever
 * elapse. With the pre-fix rt_sync(), the stale long-dwell deadline keeps
 * gating firing (since the edit doesn't touch slot/channel), so the alarm
 * would still be pending well past when the NEW, shorter dwell says it should
 * already have fired. With the fix, rt_sync() detects the dwell field changed
 * (full-struct compare) and resets the latch, so the rule re-arms fresh under
 * the new dwell and fires on schedule. */
ZTEST(alarm_eval, test_rt_sync_resets_latch_on_same_source_rule_edit)
{
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.from_state = 1,
		.to_state = 1, /* level */
		.dwell = 2.0f, /* long: must NOT have elapsed by the time we check below */
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	test_hall.left_is_active = true; /* already at the target level */
	app_alarm_poll();                /* arms the (long) confirm deadline */
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		      "level fired before any dwell elapsed");

	/* Edit the SAME slot: same slot+channel+from/to, only dwell changes
	 * (2.0 s -> 0.1 s). Bug A: rt_sync() only reset the latch on a
	 * slot/channel change, so the stale 2 s deadline armed above would
	 * otherwise still gate firing below instead of the new 0.1 s one. */
	r.dwell = 0.1f;
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule edit rejected");
	app_alarm_poll(); /* observes the edit; with the fix, re-arms under the NEW (short) dwell */

	k_sleep(K_MSEC(250)); /* past the NEW 0.1 s dwell, nowhere near the stale 2 s one */
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE),
		     "stale latch from before the rule edit blocked firing under the new dwell "
		     "(rt_sync did not reset on a same-slot/channel edit)");
}

/* Bug B regression (today's PR#349 same-day finding): eval_count()'s one-shot-
 * then-hold guarantee ("fires and then holds for dwell seconds ... before it
 * can fire again", app_alarm_rules.h) had no `!rt->active` gate on the firing
 * condition, so a counter that stays over the per-window rate limit across
 * MULTIPLE consecutive interval_report windows kept re-triggering every
 * window and re-extending oneshot_expiry indefinitely, instead of holding for
 * a single `dwell` period from the FIRST fire and then re-arming.
 *
 * Demonstrated by choosing dwell (1.5 s) longer than interval_report (1 s), so
 * a second over-threshold window lands while the first fire's hold is still
 * running: with the bug, that second window re-extends the hold so it never
 * naturally expires as long as the condition keeps holding; fixed, the second
 * window is suppressed (no re-extension) and the ORIGINAL hold expires on
 * schedule — observed via an intermediate poll (not itself a window
 * boundary) that only exercises the central oneshot-expiry sweep. A third,
 * later window then re-arms and fires again, proving the hold isn't just
 * suppressing forever but genuinely re-arms. */
ZTEST(alarm_eval, test_rate_count_one_shot_then_hold_across_multiple_windows)
{
	g_app_config.interval_report = 1; /* 1 s tumbling window */
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.hi = 2,       /* alarm when the window's delta >= 2 */
		.dwell = 1.5f, /* hold LONGER than the 1 s window, so it spans window 2 */
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	APP_SENSOR_MB_U(&g_app_sensor_data, HALL_LEFT_COUNT) = 0;
	app_alarm_poll(); /* seeds the baseline + window start, no rule evaluated yet */
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT),
		      "spuriously active before any window elapsed");

	/* Window 1: over-threshold delta (3 >= hi 2) -> first fire. */
	APP_SENSOR_MB_U(&g_app_sensor_data, HALL_LEFT_COUNT) = 3;
	k_sleep(K_MSEC(1100));
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT),
		     "rate alarm never fired after the first over-limit window elapsed");

	/* Window 2: STILL over-threshold (another delta of 3) while window 1's
	 * 1.5 s hold is still running (only ~1.1 s elapsed). Bug: this
	 * unconditionally re-fires and pushes oneshot_expiry another 1.5 s out.
	 * Fixed: the `!rt->active` gate suppresses this re-fire, so the hold set
	 * by window 1 is left untouched. */
	APP_SENSOR_MB_U(&g_app_sensor_data, HALL_LEFT_COUNT) = 6;
	k_sleep(K_MSEC(1100));
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT),
		     "alarm dropped active mid-hold (unexpected)");

	/* Intermediate poll, deliberately NOT at a window boundary (only ~0.5 s
	 * into the new 1 s window, so eval_count's own window-hold check returns
	 * early and does not touch the latch) — this isolates app_alarm_poll()'s
	 * central oneshot-expiry sweep, which fires purely off oneshot_expiry vs.
	 * now. Only the FIXED code's un-extended window-1 expiry (~2.6 s from
	 * start) has elapsed by here (~2.7 s from start); the buggy code's
	 * window-2-extended expiry (~3.7 s from start) has not. */
	k_sleep(K_MSEC(500));
	app_alarm_poll();
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT),
		      "hold from window 1 never expired — a still-over-threshold window 2 "
		      "re-extended it instead of being suppressed by the one-shot-then-hold gate");

	/* Window 3: still over-threshold -> having genuinely gone inactive above,
	 * the rule must re-arm and fire again (not just suppress forever). */
	APP_SENSOR_MB_U(&g_app_sensor_data, HALL_LEFT_COUNT) = 9;
	k_sleep(K_MSEC(600)); /* 500 + 600 = 1.1 s since window 2's re-baseline: window elapsed */
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT),
		     "rate alarm never re-armed after the hold genuinely expired");
}

/* Deactivate edge on rule removal/disable/edit (2026-08-18 final-review fix):
 * rt_sync() resets an ACTIVE latch when its rule is cleared, disabled, or
 * edited — and must emit the matching fPort-3 deactivate edge itself, because
 * eval_threshold()/eval_state()'s own !rule->enabled deactivate branches run
 * only AFTER rt_sync() already zeroed rt->active for exactly that transition
 * (they can never see the pre-reset latch). Without the emission, a backend
 * pairing activate/deactivate edges is left with a dangling activate. */

extern struct app_cmd_alarm_event test_alarm_events[16];
extern size_t test_alarm_event_count;

static void activate_threshold_slot0(void)
{
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 30.0f,
		.dwell = 0.0f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) = 35.0f; /* above hi, dwell 0: fires now */
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE),
		     "threshold did not activate");
}

static const struct app_cmd_alarm_event *last_event(void)
{
	zassert_true(test_alarm_event_count > 0, "no alarm event captured");
	return &test_alarm_events[test_alarm_event_count - 1];
}

/* #409 3b: at the 11 B budget tier not even one AlarmEvent fits a frame. The
 * flush must skip the fPort 3 detail cleanly (no partial/garbage frame) while
 * the alarm itself stays latched, so its state still reaches the LNS through
 * the telemetry system_flags alarm bits (app_alarm_status_flags()). */
ZTEST(alarm_eval, test_alarm_detail_skipped_when_no_event_fits)
{
	g_app_config.alarm_limit = 0; /* flush synchronously inside poll */
	test_alarm_event_count = 0;
	test_alarm_max_events = 0;

	activate_threshold_slot0();

	zassert_equal(test_alarm_event_count, 0, "no event may be emitted, got %zu",
		      test_alarm_event_count);
	zassert_true(app_alarm_status_flags() & APP_DEVICE_STATUS_ALARM_ANY,
		     "alarm state must stay visible for the telemetry mirror");
	test_alarm_max_events = SIZE_MAX;
}

/* Boot/join order (2026-09-27): Info -> settings-info -> data. While the radio
 * holds data (link down, or its announce still going out) the alarm batch
 * waits and collects later edges; app_alarm_flush_held() sends it once the
 * announce is out. */

ZTEST(alarm_eval, test_alarm_batch_waits_for_the_boot_announce)
{
	g_app_config.alarm_limit = 0; /* would flush synchronously inside poll */
	test_alarm_event_count = 0;
	test_radio_data_hold_ms = -1; /* link down / announce pending */

	activate_threshold_slot0();
	zassert_equal(test_alarm_event_count, 0, "no alarm frame before the announce, got %zu",
		      test_alarm_event_count);

	/* A second edge while held joins the same batch instead of replacing it. */
	zassert_equal(app_alarm_rules_clear(0), 0, "rule clear rejected");
	app_alarm_poll();
	zassert_equal(test_alarm_event_count, 0, "still held");

	test_radio_data_hold_ms = 0; /* the announce is out */
	app_alarm_flush_held();
	k_sleep(K_MSEC(20)); /* the flush runs on the system work queue */

	zassert_equal(test_alarm_event_count, 2, "both held edges sent, got %zu",
		      test_alarm_event_count);
	zassert_equal(test_alarm_events[0].edge, 0, "activate edge first");
	zassert_equal(test_alarm_events[1].edge, 1, "then the deactivate edge");

	/* Nothing held: a no-op. */
	app_alarm_flush_held();
	k_sleep(K_MSEC(20));
	zassert_equal(test_alarm_event_count, 2, "no extra frame");
}

/* #462: a burst must not overflow the radio's alarm queue. With the queue full
 * the batch waits held, later edges join it, and the frame app_radio takes next
 * releases it: both edges go in one frame, none is dropped. */
ZTEST(alarm_eval, test_alarm_burst_waits_for_queue_room)
{
	g_app_config.alarm_limit = 0; /* every edge would be a frame of its own */
	test_radio_alarm_free = 0;    /* four frames still queued */

	activate_threshold_slot0();
	test_alarm_frames = 0;
	test_alarm_event_count = 0;
	zassert_equal(app_alarm_rules_clear(0), 0, "rule clear rejected");
	app_alarm_poll();
	zassert_equal(test_alarm_frames, 0, "no frame into a full queue");

	test_radio_alarm_free = 1; /* app_radio took a frame */
	app_alarm_flush_held();
	k_sleep(K_MSEC(20));
	zassert_equal(test_alarm_frames, 1, "one frame, got %zu", test_alarm_frames);
	zassert_equal(test_alarm_event_count, 2, "both edges in it, got %zu",
		      test_alarm_event_count);
	zassert_equal(test_alarm_events[0].edge, 0, "the held activate edge first");
	zassert_equal(test_alarm_events[1].edge, 1, "then the deactivate edge");
}

/* #462: a batch waits until all its pages fit, so its pages stay together. */
ZTEST(alarm_eval, test_alarm_batch_waits_until_all_pages_fit)
{
	g_app_config.alarm_limit = 0;
	test_alarm_max_events = 1;     /* one event per page */
	test_radio_data_hold_ms = -1;  /* collect two edges first */
	activate_threshold_slot0();
	zassert_equal(app_alarm_rules_clear(0), 0, "rule clear rejected");
	app_alarm_poll();

	test_alarm_frames = 0;
	test_alarm_event_count = 0;
	test_radio_data_hold_ms = 0;
	test_radio_alarm_free = 1;
	app_alarm_flush_held();
	k_sleep(K_MSEC(20));
	zassert_equal(test_alarm_frames, 0, "2 pages, 1 free slot: waits");

	test_radio_alarm_free = 2;
	app_alarm_flush_held();
	k_sleep(K_MSEC(20));
	zassert_equal(test_alarm_frames, 2, "both pages, got %zu", test_alarm_frames);
	zassert_equal(test_alarm_event_count, 2);
}

/* #462: an empty queue takes a batch of more pages than it holds: no queued
 * frame is left whose dequeue would release it. */
ZTEST(alarm_eval, test_empty_queue_takes_an_oversized_batch)
{
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 30.0f,
	};

	g_app_config.alarm_limit = 0;
	test_alarm_max_events = 1;
	test_radio_data_hold_ms = -1;
	for (uint8_t i = 0; i <= APP_RADIO_TX_QUEUE_DEPTH; i++) {
		zassert_equal(app_alarm_rules_set(i, &r), 0, "rule %u rejected", i);
	}
	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) = 35.0f;
	app_alarm_poll();

	test_alarm_frames = 0;
	test_radio_data_hold_ms = 0;
	app_alarm_flush_held();
	k_sleep(K_MSEC(20));
	zassert_equal(test_alarm_frames, APP_RADIO_TX_QUEUE_DEPTH + 1, "every page, got %zu",
		      test_alarm_frames);
}

/* #462: before a post-command action (a reboot) app_radio asks for a batch
 * still open in its alarm-limit window: it goes now, not at the window's end. */
ZTEST(alarm_eval, test_flush_pending_sends_a_collecting_window)
{
	g_app_config.alarm_limit = 10;
	zassert_false(app_alarm_flush_pending(), "nothing waits yet");

	activate_threshold_slot0();
	test_alarm_frames = 0;
	k_sleep(K_MSEC(20));
	zassert_equal(test_alarm_frames, 0, "the window is still open");

	zassert_true(app_alarm_flush_pending(), "a batch waits");
	k_sleep(K_MSEC(20));
	zassert_equal(test_alarm_frames, 1, "sent before the window's end");
	zassert_false(app_alarm_flush_pending(), "nothing waits any more");
}

/* #462: with the link down a waiting batch cannot go; the action must not wait
 * for it. */
ZTEST(alarm_eval, test_flush_pending_ignores_a_batch_held_for_the_link)
{
	g_app_config.alarm_limit = 0;
	test_radio_data_hold_ms = -1;
	activate_threshold_slot0();

	test_alarm_frames = 0;
	zassert_false(app_alarm_flush_pending(), "the link is down");
	k_sleep(K_MSEC(20));
	zassert_equal(test_alarm_frames, 0);

	test_radio_data_hold_ms = 0; /* leave no held batch for the next case */
	app_alarm_flush_held();
	k_sleep(K_MSEC(20));
	zassert_equal(test_alarm_frames, 1);
}

ZTEST(alarm_eval, test_clearing_active_rule_emits_deactivate_edge)
{
	g_app_config.alarm_limit = 0; /* flush synchronously inside poll */
	activate_threshold_slot0();

	test_alarm_event_count = 0;
	zassert_equal(app_alarm_rules_clear(0), 0, "rule clear rejected");
	zassert_false(app_alarm_poll(), "cleared rule still reported active");

	const struct app_cmd_alarm_event *ev = last_event();
	zassert_equal(ev->edge, 1, "expected deactivate edge (1), got %u", ev->edge);
	zassert_equal(ev->rule, 0, "deactivate edge on wrong rule %u", ev->rule);
	zassert_equal(ev->slot, APP_ALARM_SLOT_MB, "wrong slot %u", ev->slot);
	zassert_equal(ev->channel, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE, "wrong channel %u",
		      ev->channel);
}

ZTEST(alarm_eval, test_disabling_active_rule_emits_deactivate_edge)
{
	g_app_config.alarm_limit = 0;
	activate_threshold_slot0();

	struct app_alarm_rule r;
	zassert_true(app_alarm_rules_get(0, &r), "rule readback failed");
	r.enabled = 0;
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule disable rejected");

	test_alarm_event_count = 0;
	zassert_false(app_alarm_poll(), "disabled rule still reported active");

	const struct app_cmd_alarm_event *ev = last_event();
	zassert_equal(ev->edge, 1, "expected deactivate edge (1), got %u", ev->edge);
	zassert_equal(ev->rule, 0, "deactivate edge on wrong rule %u", ev->rule);
}

ZTEST(alarm_eval, test_editing_active_rule_emits_deactivate_edge_then_rearms)
{
	g_app_config.alarm_limit = 0;
	activate_threshold_slot0();

	/* In-place edit (same slot+channel, new band): X9's HIL-confirmed
	 * latch reset — now paired with the deactivate edge for the old latch. */
	struct app_alarm_rule r;
	zassert_true(app_alarm_rules_get(0, &r), "rule readback failed");
	r.hi = 40.0f;
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule edit rejected");

	test_alarm_event_count = 0;
	app_alarm_poll();
	zassert_true(test_alarm_event_count > 0, "no deactivate edge on rule edit");
	zassert_equal(test_alarm_events[0].edge, 1, "expected deactivate edge first, got %u",
		      test_alarm_events[0].edge);
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE),
		      "latch survived the edit (35.0 is inside the new 0..40 band)");

	/* The edited rule must still evaluate and re-fire cleanly. */
	test_alarm_event_count = 0;
	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) = 45.0f; /* above the NEW hi */
	app_alarm_poll();
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE),
		     "edited rule never re-fired");
	zassert_true(test_alarm_event_count > 0, "no activate edge after re-fire");
	zassert_equal(last_event()->edge, 0, "expected activate edge (0), got %u",
		      last_event()->edge);
}

/* ---- #397: alarm-event -> buzzer melody trigger/stop plumbing -----------
 *
 * alarm_buzzer_sync() (app_alarm.c) drives the buzzer as a side effect of
 * app_alarm_poll()'s per-alarm active bitmask. app_buzzer_play_repeating()
 * itself is stubbed (stubs.c) so these assert on the call pattern rather
 * than real GPIO/thread behavior — that side is covered by tests/buzzer's
 * real app_buzzer.c + gpio_emul. */

ZTEST(alarm_eval, test_buzzer_suppressed_without_cap_buzzer)
{
	g_app_config.cap_buzzer = false;
	g_app_config.alarm_buzzer_mode = APP_CONFIG_ALARM_BUZZER_MODE_NORMAL;

	activate_threshold_slot0();

	zassert_equal(g_buzzer_play_calls, 0, "buzzer played without cap_buzzer");
}

ZTEST(alarm_eval, test_buzzer_suppressed_when_mode_off)
{
	g_app_config.cap_buzzer = true;
	g_app_config.alarm_buzzer_mode = APP_CONFIG_ALARM_BUZZER_MODE_OFF;

	activate_threshold_slot0();

	zassert_equal(g_buzzer_play_calls, 0, "buzzer played with alarm_buzzer_mode = off");
}

ZTEST(alarm_eval, test_buzzer_plays_alarm_melody_on_activation)
{
	g_app_config.cap_buzzer = true;
	g_app_config.alarm_buzzer_mode = APP_CONFIG_ALARM_BUZZER_MODE_NORMAL;

	activate_threshold_slot0();

	zassert_equal(g_buzzer_play_calls, 1, "activation must trigger exactly one buzzer call");
	zassert_equal(g_buzzer_play_last_kind, APP_BUZZER_KIND_ALARM, "wrong melody kind");
	zassert_equal(g_buzzer_play_last_repeat_s, 30, "wrong repeat interval");
}

ZTEST(alarm_eval, test_buzzer_mode_repeat_intervals)
{
	/* Each non-off mode maps to its own melody-engine repeat interval;
	 * reserved modes fall back to normal's. Walk them all on the same
	 * activation edge (deactivate + reactivate between modes). */
	static const struct {
		enum app_config_alarm_buzzer_mode mode;
		uint16_t repeat_s;
	} cases[] = {
		{APP_CONFIG_ALARM_BUZZER_MODE_ONCE, 0},
		{APP_CONFIG_ALARM_BUZZER_MODE_SLOW, 120},
		{APP_CONFIG_ALARM_BUZZER_MODE_NORMAL, 30},
		{APP_CONFIG_ALARM_BUZZER_MODE_FAST, 10},
		{APP_CONFIG_ALARM_BUZZER_MODE_CONTINUOUS, 1},
		{APP_CONFIG_ALARM_BUZZER_MODE_RESERVED_6, 30},
		{APP_CONFIG_ALARM_BUZZER_MODE_RESERVED_7, 30},
	};

	g_app_config.cap_buzzer = true;

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		g_app_config.alarm_buzzer_mode = cases[i].mode;
		g_buzzer_play_calls = 0;

		activate_threshold_slot0();
		zassert_equal(g_buzzer_play_calls, 1, "mode %d: expected one melody call",
			      cases[i].mode);
		zassert_equal(g_buzzer_play_last_kind, APP_BUZZER_KIND_ALARM,
			      "mode %d: wrong melody kind", cases[i].mode);
		zassert_equal(g_buzzer_play_last_repeat_s, cases[i].repeat_s,
			      "mode %d: wrong repeat interval", cases[i].mode);

		APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) =
			20.0f; /* clear for the next round */
		app_alarm_poll();
	}
}

ZTEST(alarm_eval, test_buzzer_does_not_retrigger_while_still_active)
{
	g_app_config.cap_buzzer = true;
	g_app_config.alarm_buzzer_mode = APP_CONFIG_ALARM_BUZZER_MODE_NORMAL;

	activate_threshold_slot0();
	zassert_equal(g_buzzer_play_calls, 1, "activation must trigger exactly one buzzer call");

	g_buzzer_play_calls = 0;
	zassert_true(app_alarm_poll(), "alarm unexpectedly cleared");
	zassert_true(app_alarm_poll(), "alarm unexpectedly cleared");
	zassert_equal(g_buzzer_play_calls, 0,
		      "buzzer re-triggered on a poll with no activation edge");
}

ZTEST(alarm_eval, test_buzzer_replays_on_new_alarm_while_another_active)
{
	/* A SECOND alarm activating while the first is still active must replay
	 * the melody immediately (per-alarm bitmask edge, not the aggregate
	 * bool) — in every non-off mode, here demonstrated with `once`, whose
	 * repeat_s=0 means the replay could not come from the engine's own
	 * repeat cycle. */
	g_app_config.cap_buzzer = true;
	g_app_config.alarm_buzzer_mode = APP_CONFIG_ALARM_BUZZER_MODE_ONCE;

	activate_threshold_slot0(); /* alarm #1: onboard temperature, slot 0 */
	zassert_equal(g_buzzer_play_calls, 1, "first activation must play once");

	/* Alarm #2 in slot 1: onboard humidity, dwell 0, fires on this poll. */
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_HUMIDITY,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 60.0f,
		.dwell = 0.0f,
	};
	zassert_equal(app_alarm_rules_set(1, &r), 0, "rule setup rejected");
	APP_SENSOR_MB_F(&g_app_sensor_data, HUMIDITY) = 90.0f; /* above hi */

	g_buzzer_play_calls = 0;
	zassert_true(app_alarm_poll(), "aggregate unexpectedly cleared");
	zassert_equal(g_buzzer_play_calls, 1, "a NEW alarm while another is active must replay");
	zassert_equal(g_buzzer_play_last_kind, APP_BUZZER_KIND_ALARM, "wrong melody kind");

	/* Alarm #2 clearing while #1 stays active: no new melody, no stop. */
	APP_SENSOR_MB_F(&g_app_sensor_data, HUMIDITY) = 40.0f;
	g_buzzer_play_calls = 0;
	zassert_true(app_alarm_poll(), "alarm #1 should still be active");
	zassert_equal(g_buzzer_play_calls, 0, "partial deactivation must not touch the buzzer");
}

ZTEST(alarm_eval, test_active_mask_and_activation_seq)
{
	/* #397: the per-alarm active mask + monotonic activation sequence are
	 * readable anywhere via app_alarm_active_mask()/app_alarm_activation_seq()
	 * — the same central edge the buzzer replays on. seq is a static that
	 * persists across test cases, so compare relatively, never absolutely. */
	zassert_equal(app_alarm_active_mask(), 0, "mask not empty at test start");
	uint32_t seq0 = app_alarm_activation_seq();

	activate_threshold_slot0(); /* slot 0 latches */
	zassert_equal(app_alarm_active_mask(), BIT(0), "slot 0 bit not set");
	uint32_t seq1 = app_alarm_activation_seq();
	zassert_true(seq1 != seq0, "activation did not bump the sequence");

	/* Polls with no change must bump nothing. */
	app_alarm_poll();
	app_alarm_poll();
	zassert_equal(app_alarm_activation_seq(), seq1, "seq bumped without a new activation");
	zassert_equal(app_alarm_active_mask(), BIT(0), "mask changed without a state change");

	/* A second alarm (slot 1, onboard humidity) sets its own bit + bumps seq. */
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_HUMIDITY,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 60.0f,
		.dwell = 0.0f,
	};
	zassert_equal(app_alarm_rules_set(1, &r), 0, "rule setup rejected");
	APP_SENSOR_MB_F(&g_app_sensor_data, HUMIDITY) = 90.0f;
	app_alarm_poll();
	zassert_equal(app_alarm_active_mask(), BIT(0) | BIT(1), "slot 1 bit not added");
	uint32_t seq2 = app_alarm_activation_seq();
	zassert_true(seq2 != seq1, "second activation did not bump the sequence");

	/* Deactivations clear bits but never bump the sequence. */
	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) = 20.0f;
	APP_SENSOR_MB_F(&g_app_sensor_data, HUMIDITY) = 40.0f;
	zassert_false(app_alarm_poll(), "alarms did not clear");
	zassert_equal(app_alarm_active_mask(), 0, "mask not empty after deactivation");
	zassert_equal(app_alarm_activation_seq(), seq2, "deactivation bumped the sequence");
}

ZTEST(alarm_eval, test_buzzer_stops_on_deactivation)
{
	g_app_config.cap_buzzer = true;
	g_app_config.alarm_buzzer_mode = APP_CONFIG_ALARM_BUZZER_MODE_NORMAL;

	activate_threshold_slot0();
	g_buzzer_play_calls = 0;

	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) = 20.0f; /* back in band: deactivates */
	zassert_false(app_alarm_poll(), "alarm did not clear");

	zassert_equal(g_buzzer_play_calls, 1, "deactivation must trigger exactly one buzzer call");
	zassert_equal(g_buzzer_play_last_kind, APP_BUZZER_KIND_STOP,
		      "expected a stop, not a melody");
}

/* #430: 1-Wire slot rules read the slot's channel vector. A machine-probe slot
 * serves humidity from its own channel; with a dallas in the slot the
 * machine-probe rule is stale and stays inert even if a stray value sits at
 * that index. */
static bool fire_slot1_humidity_rule(uint8_t type)
{
	struct app_alarm_rule r = {
		.slot = 1,
		.channel = APP_SENSOR_CH_MACHINE_PROBE_HUMIDITY,
		.sensor_type = APP_SENSOR_TYPE_MACHINE_PROBE,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 60.0f,
		.dwell = 0.0f,
	};

	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");
	g_app_config.sensor1_type = type;
	app_sensor_w1_clear(&g_app_sensor_data.w1[0], type);
	g_app_sensor_data.w1[0].present = true;
	g_app_sensor_data.w1[0].v[APP_SENSOR_CH_MACHINE_PROBE_HUMIDITY].f = 90.0f;
	return app_alarm_poll();
}

ZTEST(alarm_eval, test_w1_machine_probe_humidity_channel_fires)
{
	zassert_true(fire_slot1_humidity_rule(APP_SENSOR_TYPE_MACHINE_PROBE),
		     "machine-probe humidity above hi must fire");
	zassert_true(is_active(1, APP_SENSOR_CH_MACHINE_PROBE_HUMIDITY));
}

ZTEST(alarm_eval, test_w1_dallas_has_no_humidity_channel)
{
	zassert_false(fire_slot1_humidity_rule(APP_SENSOR_TYPE_DALLAS),
		      "a dallas slot must not serve humidity");
	zassert_false(is_active(1, APP_SENSOR_CH_MACHINE_PROBE_HUMIDITY));
}

ZTEST(alarm_eval, test_w1_dallas_temperature_channel_fires)
{
	struct app_alarm_rule r = {
		.slot = 2,
		.channel = APP_SENSOR_CH_DALLAS_TEMPERATURE,
		.sensor_type = APP_SENSOR_TYPE_DALLAS,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 30.0f,
		.dwell = 0.0f,
	};

	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");
	g_app_config.sensor2_type = APP_SENSOR_TYPE_DALLAS;
	app_sensor_w1_clear(&g_app_sensor_data.w1[1], APP_SENSOR_TYPE_DALLAS);
	g_app_sensor_data.w1[1].present = true;
	g_app_sensor_data.w1[1].v[APP_SENSOR_CH_DALLAS_TEMPERATURE].f = 35.0f;
	zassert_true(app_alarm_poll(), "dallas temperature above hi must fire");
}

/* The pressure channel is hPa (#430); the rule thresholds are hPa too, so no
 * unit conversion sits between them any more (it used to be kPa x10). */
ZTEST(alarm_eval, test_onboard_pressure_is_hpa)
{
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_PRESSURE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.lo = 950.0f,
		.hi = 1050.0f,
		.dwell = 0.0f,
	};

	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");
	APP_SENSOR_MB_F(&g_app_sensor_data, PRESSURE) = 1013.0f;
	zassert_false(app_alarm_poll(), "1013 hPa is inside [950, 1050]");
	APP_SENSOR_MB_F(&g_app_sensor_data, PRESSURE) = 1080.0f;
	zassert_true(app_alarm_poll(), "1080 hPa is above hi");
}

static size_t count_onboard_nodata(uint8_t edge)
{
	size_t n = 0;

	for (size_t i = 0; i < test_alarm_event_count; i++) {
		const struct app_cmd_alarm_event *e = &test_alarm_events[i];

		if (e->slot == APP_ALARM_SLOT_MB && e->type == 4 /* no_data */ &&
		    e->edge == edge &&
		    (e->channel == APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE ||
		     e->channel == APP_SENSOR_CH_MOTHERBOARD_HUMIDITY)) {
			n++;
		}
	}
	return n;
}

/* #465: the no-data watchdog only watches the onboard SHT4x while cap_sht is on.
 * On: a NaN temperature/humidity raises no_data after APP_ALARM_NO_DATA_MS.
 * Turning the cap off emits the deactivate edges; while off, NaN never fires. */
ZTEST(alarm_eval, test_cap_sht_gates_onboard_nodata)
{
	g_app_config.alarm_limit = 0; /* flush synchronously inside poll */
	APP_SENSOR_MB_F(&g_app_sensor_data, TEMPERATURE) = NAN;
	APP_SENSOR_MB_F(&g_app_sensor_data, HUMIDITY) = NAN;

	g_app_config.cap_sht = true;
	test_alarm_event_count = 0;
	app_alarm_poll(); /* arms the NaN timers */
	k_sleep(K_MSEC(5100));
	app_alarm_poll();
	zassert_equal(count_onboard_nodata(0), 2, "cap_sht on: expected 2 no_data activations");

	g_app_config.cap_sht = false;
	test_alarm_event_count = 0;
	app_alarm_poll();
	zassert_equal(count_onboard_nodata(1), 2, "cap off must clear the latched no_data");

	test_alarm_event_count = 0;
	app_alarm_poll();
	k_sleep(K_MSEC(5100));
	app_alarm_poll();
	zassert_equal(test_alarm_event_count, 0, "cap_sht off: %zu events", test_alarm_event_count);
}

/* Flush a batch an earlier case left collecting (alarm_limit > 0), so the
 * capture starts empty. */
static void flush_stale_alarms(void)
{
	g_app_config.alarm_limit = 0; /* flush synchronously inside poll */
	(void)app_alarm_flush_pending();
	k_sleep(K_MSEC(10)); /* let the batch work item run */
	test_alarm_event_count = 0;
}

static size_t count_events(uint8_t slot, uint8_t type, uint8_t edge)
{
	size_t n = 0;

	for (size_t i = 0; i < test_alarm_event_count; i++) {
		const struct app_cmd_alarm_event *e = &test_alarm_events[i];

		if (e->slot == slot && e->type == type && e->edge == edge) {
			n++;
		}
	}
	return n;
}

/* #430 step 3: a 1-Wire slot in mismatch raises one TYPE_SENSOR_MISMATCH (5)
 * with sensor_type = expected and value = detected; the slot's no-data watchdog
 * stays quiet although its temperature is NaN. Re-teaching the slot (state OK)
 * emits the deactivate edge naming the same types. */
ZTEST(alarm_eval, test_sensor_mismatch_alarm_suppresses_nodata)
{
	flush_stale_alarms();
	g_app_config.cap_w1_sensors = true;
	test_w1_configured[1] = true;
	test_w1_state[1] = APP_W1_SLOT_STATE_MISMATCH;
	test_w1_expected[1] = APP_SENSOR_TYPE_MACHINE_PROBE;
	test_w1_detected[1] = APP_SENSOR_TYPE_DALLAS;
	app_sensor_w1_clear(&g_app_sensor_data.w1[1], APP_SENSOR_TYPE_MACHINE_PROBE);

	test_alarm_event_count = 0;
	zassert_true(app_alarm_poll(), "mismatch counts as an active alarm");
	k_sleep(K_MSEC(5100));
	app_alarm_poll();

	zassert_equal(test_alarm_event_count, 1, "expected one event, got %zu",
		      test_alarm_event_count);
	const struct app_cmd_alarm_event *e = &test_alarm_events[0];

	zassert_equal(e->type, 5, "type sensor_mismatch");
	zassert_equal(e->slot, 2);
	zassert_equal(e->channel, 0);
	zassert_equal(e->edge, 0, "activate");
	zassert_equal(e->rule, 0xFF, "watchdog event");
	zassert_equal(e->sensor_type, APP_SENSOR_TYPE_MACHINE_PROBE, "expected type");
	zassert_true(e->has_value && e->value == APP_SENSOR_TYPE_DALLAS, "detected type");

	uint32_t flags = app_alarm_status_flags();

	zassert_true(flags & APP_DEVICE_STATUS_ALARM_SENSOR_MISMATCH, "status bit");
	zassert_false(flags & APP_DEVICE_STATUS_ALARM_NO_DATA, "no-data suppressed");

	struct app_alarm_active act[8];
	size_t n = app_alarm_active_snapshot(act, ARRAY_SIZE(act));

	zassert_equal(n, 1, "one active alarm, got %zu", n);
	zassert_equal(act[0].type, 5);
	zassert_equal(act[0].slot, 2);

	/* Re-taught: the right probe is back and reads. */
	test_w1_state[1] = APP_W1_SLOT_STATE_OK;
	test_w1_detected[1] = 0;
	app_sensor_put_f(APP_SENSOR_TYPE_MACHINE_PROBE, g_app_sensor_data.w1[1].v,
			 &g_app_sensor_data.w1[1].valid, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE,
			 21.0f);
	test_alarm_event_count = 0;
	zassert_false(app_alarm_poll(), "nothing active after re-teach");
	zassert_equal(count_events(2, 5, 1), 1, "deactivate edge");
	zassert_equal(test_alarm_events[0].sensor_type, APP_SENSOR_TYPE_MACHINE_PROBE);
	zassert_equal(test_alarm_events[0].value, APP_SENSOR_TYPE_DALLAS);
	zassert_equal(test_alarm_event_count, 1, "no no-data edges");
}

/* A mismatch that appears while the slot's no-data alarm is latched replaces
 * it: the no-data deactivate edge, then the mismatch activate edge. */
ZTEST(alarm_eval, test_sensor_mismatch_replaces_latched_nodata)
{
	flush_stale_alarms();
	g_app_config.cap_w1_sensors = true;
	test_w1_configured[0] = true;
	test_w1_state[0] = APP_W1_SLOT_STATE_ABSENT;
	test_w1_expected[0] = APP_SENSOR_TYPE_DALLAS;
	app_sensor_w1_clear(&g_app_sensor_data.w1[0], APP_SENSOR_TYPE_DALLAS);

	test_alarm_event_count = 0;
	k_sleep(K_MSEC(10));
	app_alarm_poll();
	k_sleep(K_MSEC(5100));
	app_alarm_poll();
	zassert_equal(count_events(1, 4, 0), 1, "absent probe: no_data");

	test_w1_state[0] = APP_W1_SLOT_STATE_MISMATCH;
	test_w1_detected[0] = APP_SENSOR_TYPE_MACHINE_PROBE;
	test_alarm_event_count = 0;
	app_alarm_poll();
	zassert_equal(count_events(1, 4, 1), 1, "no_data cleared");
	zassert_equal(count_events(1, 5, 0), 1, "mismatch raised");

	/* Slot cleared: the mismatch deactivates, nothing else fires. */
	test_w1_configured[0] = false;
	test_w1_state[0] = APP_W1_SLOT_STATE_NONE;
	test_alarm_event_count = 0;
	app_alarm_poll();
	zassert_equal(count_events(1, 5, 1), 1, "mismatch cleared");
	zassert_equal(test_alarm_event_count, 1);
}

/* ---- #430 step 4: slot/channel rules ------------------------------------ */

static void w1_slot_reads(int s, uint8_t type, uint8_t ch, float v)
{
	g_app_sensor_data.w1[s].present = true;
	app_sensor_put_f(type, g_app_sensor_data.w1[s].v, &g_app_sensor_data.w1[s].valid, ch, v);
}

static void set_threshold(uint8_t rule, uint8_t slot, uint8_t type, uint8_t ch, float hi)
{
	struct app_alarm_rule r = {
		.slot = slot,
		.channel = ch,
		.sensor_type = type,
		.enabled = 1,
		.lo = 0.0f,
		.hi = hi,
		.dwell = 0.0f,
	};

	zassert_equal(app_alarm_rules_set(rule, &r), 0, "rule %u setup rejected", rule);
}

/* Two temperature channels of one machine probe are independent rules. */
ZTEST(alarm_eval, test_machine_probe_two_temperatures_independent)
{
	const uint8_t mp = APP_SENSOR_TYPE_MACHINE_PROBE;

	g_app_config.sensor1_type = mp;
	app_sensor_w1_clear(&g_app_sensor_data.w1[0], mp);
	set_threshold(0, 1, mp, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE, 30.0f);
	set_threshold(1, 1, mp, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX, 50.0f);

	w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE, 35.0f);
	w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX, 40.0f);
	zassert_true(app_alarm_poll());
	zassert_true(is_active(1, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE));
	zassert_false(is_active(1, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX));
	zassert_equal(app_alarm_active_mask(), BIT(0));

	w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE, 20.0f);
	w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX, 60.0f);
	zassert_true(app_alarm_poll());
	zassert_false(is_active(1, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE));
	zassert_true(is_active(1, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX));
	zassert_equal(app_alarm_active_mask(), BIT(1));
}

/* A slot holding another type than the rule's (mismatch) never evaluates the
 * rule, even when the foreign sensor's value at that channel is out of band. */
ZTEST(alarm_eval, test_mismatch_slot_rule_inert)
{
	flush_stale_alarms();
	g_app_config.sensor2_type = APP_SENSOR_TYPE_MACHINE_PROBE;
	set_threshold(0, 2, APP_SENSOR_TYPE_MACHINE_PROBE, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE,
		      30.0f);
	app_sensor_w1_clear(&g_app_sensor_data.w1[1], APP_SENSOR_TYPE_DALLAS);
	w1_slot_reads(1, APP_SENSOR_TYPE_DALLAS, APP_SENSOR_CH_DALLAS_TEMPERATURE, 35.0f);

	app_alarm_poll();
	zassert_false(is_active(2, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE),
		      "rule fired on a foreign sensor's value");
	zassert_equal(test_alarm_event_count, 0);
}

/* A stale rule (slot type changed) is inert; a STATE latch it held is dropped
 * with a deactivate edge. */
ZTEST(alarm_eval, test_stale_rule_inert)
{
	flush_stale_alarms();
	g_app_config.sensor1_type = APP_SENSOR_TYPE_MACHINE_PROBE;
	app_sensor_w1_clear(&g_app_sensor_data.w1[0], APP_SENSOR_TYPE_MACHINE_PROBE);
	set_threshold(0, 1, APP_SENSOR_TYPE_MACHINE_PROBE, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE,
		      30.0f);
	w1_slot_reads(0, APP_SENSOR_TYPE_MACHINE_PROBE, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE,
		      35.0f);
	zassert_true(app_alarm_poll());

	g_app_config.sensor1_type = APP_SENSOR_TYPE_DALLAS; /* re-provisioned */
	test_alarm_event_count = 0;
	zassert_false(app_alarm_poll(), "stale rule still active");
	zassert_equal(count_events(1, 2 /* high */, 1), 1, "deactivate edge");
}

/* A STATE edge delivered by channel number reaches only rules on that channel. */
ZTEST(alarm_eval, test_event_routed_by_channel)
{
	struct app_alarm_rule r = {
		.slot = 0,
		.channel = APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.from_state = 0,
		.to_state = 1,
		.dwell = 0.3f,
	};

	zassert_equal(app_alarm_rules_set(0, &r), 0);
	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_ACCEL_MOTION, true);
	zassert_false(is_active(0, APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION), "wrong channel fired");
	app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION, true);
	zassert_true(is_active(0, APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION));
}

/* The alarm value is scaled by the channel's wire scale (humidity x2). */
ZTEST(alarm_eval, test_event_value_uses_channel_wire_scale)
{
	flush_stale_alarms();
	set_threshold(0, 0, APP_SENSOR_TYPE_MOTHERBOARD, APP_SENSOR_CH_MOTHERBOARD_HUMIDITY, 60.0f);
	APP_SENSOR_MB_F(&g_app_sensor_data, HUMIDITY) = 90.5f;
	zassert_true(app_alarm_poll());

	const struct app_cmd_alarm_event *e = last_event();

	zassert_equal(e->rule, 0);
	zassert_equal(e->slot, 0);
	zassert_equal(e->channel, APP_SENSOR_CH_MOTHERBOARD_HUMIDITY);
	zassert_true(e->has_value);
	zassert_equal(e->value, 181, "value %d", e->value);
}

/* Low battery reads the battery-voltage channel (ch 20) and reports it in mV. */
ZTEST(alarm_eval, test_low_battery_on_battery_channel)
{
	flush_stale_alarms();
	g_app_config.battery_level = 2400;
	APP_SENSOR_MB_F(&g_app_sensor_data, BATTERY_VOLTAGE) = 2.2f;
	zassert_true(app_alarm_poll());

	const struct app_cmd_alarm_event *e = last_event();

	zassert_equal(e->rule, 0xFE);
	zassert_equal(e->slot, 0);
	zassert_equal(e->channel, APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE);
	zassert_equal(e->value, 2200, "value %d", e->value);

	APP_SENSOR_MB_F(&g_app_sensor_data, BATTERY_VOLTAGE) = 3.0f;
	zassert_false(app_alarm_poll());
	zassert_equal(last_event()->edge, 1);
	g_app_config.battery_level = 0;
}

/* The no-data latches are sized per slot; a registry type with more liveness
 * channels (motherboard) or parts (1-Wire) than that would silently go
 * unwatched. */
ZTEST(alarm_eval, test_registry_liveness_fits_nodata_latches)
{
	for (uint8_t id = 1; id < 255; id++) {
		const struct app_sensor_type *t = app_sensor_type_get(id);
		int n = 0;

		if (t == NULL) {
			continue;
		}
		for (uint8_t ch = 0; ch < t->channel_count; ch++) {
			if (t->channels[ch].flags & APP_SENSOR_F_LIVENESS) {
				n++;
			}
		}
		if (id == APP_SENSOR_TYPE_MOTHERBOARD) {
			zassert_true(n <= APP_ALARM_NODATA_MB_MAX, "%s: %d", t->name, n);
		} else {
			zassert_equal(n, 0, "%s: 1-Wire is watched per part", t->name);
			zassert_true(1 + t->part_count <= APP_ALARM_NODATA_W1_MAX, "%s: %d parts",
				     t->name, t->part_count);
		}
	}
}

/* ---- 1-Wire no-data: whole device vs. one part ------------------------- */

static size_t count_nodata(uint8_t slot, uint8_t channel, uint8_t edge)
{
	size_t n = 0;

	for (size_t i = 0; i < test_alarm_event_count; i++) {
		const struct app_cmd_alarm_event *e = &test_alarm_events[i];

		if (e->slot == slot && e->type == 4 /* no_data */ && e->channel == channel &&
		    e->edge == edge) {
			n++;
		}
	}
	return n;
}

/* Machine probe in s1 reading every chip but TMP112 (not fitted). `sht` /
 * `accel` = false leaves that chip's channels NaN. */
static void mp_reads(bool sht, bool accel)
{
	const uint8_t mp = APP_SENSOR_TYPE_MACHINE_PROBE;
	struct app_sensor_w1 *w = &g_app_sensor_data.w1[0];

	app_sensor_w1_clear(w, mp);
	w->present = true;
	if (sht) {
		w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE, 21.0f);
		w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_HUMIDITY, 40.0f);
	}
	w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_ILLUMINANCE, 100.0f);
	w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_MAGNETIC_FIELD, 0.1f);
	if (accel) {
		w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_TILT, 0.0f);
		w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_ACCEL_X, 0.1f);
		w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_ACCEL_Y, 0.2f);
		w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_ACCEL_Z, 9.8f);
	}
}

static void mp_slot_armed(void)
{
	flush_stale_alarms();
	g_app_config.cap_w1_sensors = true;
	test_w1_configured[0] = true;
	test_w1_state[0] = APP_W1_SLOT_STATE_OK;
	test_w1_expected[0] = APP_SENSOR_TYPE_MACHINE_PROBE;
	mp_reads(true, true);
	app_alarm_poll();
	test_alarm_event_count = 0;
}

/* Poll, wait past APP_ALARM_NO_DATA_MS, poll again. */
static void poll_past_nodata(void)
{
	app_alarm_poll();
	k_sleep(K_MSEC(5100));
	app_alarm_poll();
}

/* An unplugged probe raises ONE no_data on channel 255 (the device), not one
 * per channel; it clears when the probe answers again. TMP112, never seen,
 * stays quiet throughout. */
ZTEST(alarm_eval, test_w1_unplugged_probe_raises_one_device_alarm)
{
	mp_slot_armed();

	app_sensor_w1_clear(&g_app_sensor_data.w1[0], APP_SENSOR_TYPE_NONE);
	poll_past_nodata();
	zassert_equal(test_alarm_event_count, 1, "one event, got %zu", test_alarm_event_count);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_DEVICE, 0), 1, "device no_data");
	zassert_equal(test_alarm_events[0].sensor_type, APP_SENSOR_TYPE_MACHINE_PROBE);
	zassert_true(is_active(1, APP_SENSOR_CH_DEVICE));
	zassert_true(app_alarm_status_flags() & APP_DEVICE_STATUS_ALARM_NO_DATA);

	mp_reads(true, true);
	test_alarm_event_count = 0;
	app_alarm_poll();
	zassert_equal(test_alarm_event_count, 1, "one event, got %zu", test_alarm_event_count);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_DEVICE, 1), 1, "device recovered");
	zassert_false(is_active(1, APP_SENSOR_CH_DEVICE));
}

/* One failed chip while the probe still answers raises no_data on that chip's
 * first channel only (SHT -> temperature, LIS2DH12 -> tilt). */
ZTEST(alarm_eval, test_w1_failed_part_names_its_first_channel)
{
	mp_slot_armed();

	mp_reads(false, true);
	poll_past_nodata();
	zassert_equal(test_alarm_event_count, 1, "one event, got %zu", test_alarm_event_count);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE, 0), 1, "sht");

	mp_reads(false, false);
	test_alarm_event_count = 0;
	poll_past_nodata();
	zassert_equal(test_alarm_event_count, 1, "one event, got %zu", test_alarm_event_count);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_MACHINE_PROBE_TILT, 0), 1, "lis2dh12");

	mp_reads(true, true);
	test_alarm_event_count = 0;
	app_alarm_poll();
	zassert_equal(test_alarm_event_count, 2, "two events, got %zu", test_alarm_event_count);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE, 1), 1);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_MACHINE_PROBE_TILT, 1), 1);
}

/* A part alarm latched before the probe is unplugged is held (no false
 * recovery) and the unplug adds only the device alarm; both clear once the
 * probe answers with that part working again. */
ZTEST(alarm_eval, test_w1_device_alarm_holds_part_alarm)
{
	mp_slot_armed();

	mp_reads(false, true);
	poll_past_nodata();
	zassert_equal(count_nodata(1, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE, 0), 1);

	app_sensor_w1_clear(&g_app_sensor_data.w1[0], APP_SENSOR_TYPE_NONE);
	test_alarm_event_count = 0;
	poll_past_nodata();
	zassert_equal(test_alarm_event_count, 1, "one event, got %zu", test_alarm_event_count);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_DEVICE, 0), 1, "device no_data");
	zassert_true(is_active(1, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE), "part held");

	mp_reads(true, true);
	test_alarm_event_count = 0;
	app_alarm_poll();
	zassert_equal(test_alarm_event_count, 2, "two events, got %zu", test_alarm_event_count);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_DEVICE, 1), 1);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE, 1), 1);
}

/* A part that has not reported since the slot was armed (chip not fitted) is
 * not watched, even while the rest of the probe reports. */
ZTEST(alarm_eval, test_w1_unseen_part_is_not_watched)
{
	flush_stale_alarms();
	g_app_config.cap_w1_sensors = true;
	test_w1_configured[0] = true;
	test_w1_state[0] = APP_W1_SLOT_STATE_OK;
	test_w1_expected[0] = APP_SENSOR_TYPE_MACHINE_PROBE;
	mp_reads(false, true); /* SHT dead from the start */

	test_alarm_event_count = 0;
	poll_past_nodata();
	zassert_equal(test_alarm_event_count, 0, "%zu events", test_alarm_event_count);
}

/* A chip that answers for some of its channels only is broken: a LIS2DH12
 * that lost power still reads its tilt latch but delivers no samples. */
ZTEST(alarm_eval, test_w1_partly_reporting_part_alarms)
{
	const uint8_t mp = APP_SENSOR_TYPE_MACHINE_PROBE;

	mp_slot_armed();

	mp_reads(true, false);
	w1_slot_reads(0, mp, APP_SENSOR_CH_MACHINE_PROBE_TILT, 0.0f); /* tilt only */
	poll_past_nodata();
	zassert_equal(test_alarm_event_count, 1, "one event, got %zu", test_alarm_event_count);
	zassert_equal(count_nodata(1, APP_SENSOR_CH_MACHINE_PROBE_TILT, 0), 1, "lis2dh12");

	mp_reads(true, true);
	test_alarm_event_count = 0;
	app_alarm_poll();
	zassert_equal(count_nodata(1, APP_SENSOR_CH_MACHINE_PROBE_TILT, 1), 1, "recovered");
}
