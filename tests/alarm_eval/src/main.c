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
#include "app_sensor.h"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

extern struct app_hall_data test_hall;
extern int g_buzzer_play_calls;
extern uint32_t g_buzzer_play_last_kind;
extern uint16_t g_buzzer_play_last_repeat_s;

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

static void set_hall_left_edge_rule(uint8_t from, uint8_t to, float dwell_s)
{
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_HALL_LEFT,
		.quantity = APP_ALARM_Q_STATE,
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

	k_sleep(K_MSEC(400)); /* past dwell, level still held (no second edge) */
	app_alarm_poll();     /* the real re-check path: read_poll_state() -> eval_state() */
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
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
	app_alarm_event(APP_ALARM_SRC_HALL_LEFT, true); /* seed prev_state = 1 */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		      "spuriously active before any transition");

	test_hall.left_is_active = false;
	app_alarm_event(APP_ALARM_SRC_HALL_LEFT, false); /* raw 1->0 transition: arms confirm */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		      "edge fired before the confirm dwell elapsed");

	k_sleep(K_MSEC(400)); /* past dwell, level still held (no second edge) */
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		     "reverse-direction (1->0) edge never fired after the confirm dwell elapsed");

	k_sleep(K_MSEC(400)); /* past the hold */
	app_alarm_poll();
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
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
 * `dwell` seconds before it can re-arm. Not HIL-tested in the #348 session. */
ZTEST(alarm_eval, test_momentary_pir_fires_immediately_then_holds)
{
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_PIR,
		.quantity = APP_ALARM_Q_STATE,
		.enabled = 1,
		.from_state = 0,
		.to_state = 1,
		.dwell = 0.3f,
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
 * window reaches `hi`, then holds/re-arm-blocks for `dwell` seconds — same
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
		.dwell = 0.3f,
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
		.dwell = 0.3f,
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
		.dwell = 0.3f,
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

/* Bug A regression (today's PR#349 same-day finding): rt_sync() must reset the
 * per-slot runtime latch (m_rt[]) on ANY rule edit, not just a source/quantity
 * change. Before the fix, rt_sync() compared only source+quantity, so a live
 * edit that keeps those the same but changes e.g. dwell left a stale
 * confirm_deadline/active latch from the OLD rule in place.
 *
 * Demonstrated here with a STATE level rule on hall-left: arm a confirm
 * deadline under a LONG dwell (2.0 s), then edit the SAME slot (same
 * source+quantity) to a SHORT dwell (0.1 s) before the long one would ever
 * elapse. With the pre-fix rt_sync(), the stale long-dwell deadline keeps
 * gating firing (since the edit doesn't touch source/quantity), so the alarm
 * would still be pending well past when the NEW, shorter dwell says it should
 * already have fired. With the fix, rt_sync() detects the dwell field changed
 * (full-struct compare) and resets the latch, so the rule re-arms fresh under
 * the new dwell and fires on schedule. */
ZTEST(alarm_eval, test_rt_sync_resets_latch_on_same_source_rule_edit)
{
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_HALL_LEFT,
		.quantity = APP_ALARM_Q_STATE,
		.enabled = 1,
		.from_state = 1,
		.to_state = 1, /* level */
		.dwell = 2.0f, /* long: must NOT have elapsed by the time we check below */
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	test_hall.left_is_active = true; /* already at the target level */
	app_alarm_poll();                /* arms the (long) confirm deadline */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		      "level fired before any dwell elapsed");

	/* Edit the SAME slot: same source+quantity+from/to, only dwell changes
	 * (2.0 s -> 0.1 s). Bug A: rt_sync() only reset the latch on a
	 * source/quantity change, so the stale 2 s deadline armed above would
	 * otherwise still gate firing below instead of the new 0.1 s one. */
	r.dwell = 0.1f;
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule edit rejected");
	app_alarm_poll(); /* observes the edit; with the fix, re-arms under the NEW (short) dwell */

	k_sleep(K_MSEC(250)); /* past the NEW 0.1 s dwell, nowhere near the stale 2 s one */
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_STATE),
		     "stale latch from before the rule edit blocked firing under the new dwell "
		     "(rt_sync did not reset on a same-source/quantity edit)");
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
		.source = APP_ALARM_SRC_HALL_LEFT,
		.quantity = APP_ALARM_Q_COUNT,
		.enabled = 1,
		.hi = 2,       /* alarm when the window's delta >= 2 */
		.dwell = 1.5f, /* hold LONGER than the 1 s window, so it spans window 2 */
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	g_app_sensor_data.hall_left_count = 0;
	app_alarm_poll(); /* seeds the baseline + window start, no rule evaluated yet */
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_COUNT),
		      "spuriously active before any window elapsed");

	/* Window 1: over-threshold delta (3 >= hi 2) -> first fire. */
	g_app_sensor_data.hall_left_count = 3;
	k_sleep(K_MSEC(1100));
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_COUNT),
		     "rate alarm never fired after the first over-limit window elapsed");

	/* Window 2: STILL over-threshold (another delta of 3) while window 1's
	 * 1.5 s hold is still running (only ~1.1 s elapsed). Bug: this
	 * unconditionally re-fires and pushes oneshot_expiry another 1.5 s out.
	 * Fixed: the `!rt->active` gate suppresses this re-fire, so the hold set
	 * by window 1 is left untouched. */
	g_app_sensor_data.hall_left_count = 6;
	k_sleep(K_MSEC(1100));
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_COUNT),
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
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_COUNT),
		      "hold from window 1 never expired — a still-over-threshold window 2 "
		      "re-extended it instead of being suppressed by the one-shot-then-hold gate");

	/* Window 3: still over-threshold -> having genuinely gone inactive above,
	 * the rule must re-arm and fire again (not just suppress forever). */
	g_app_sensor_data.hall_left_count = 9;
	k_sleep(K_MSEC(600)); /* 500 + 600 = 1.1 s since window 2's re-baseline: window elapsed */
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_HALL_LEFT, APP_ALARM_Q_COUNT),
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
		.source = APP_ALARM_SRC_ONBOARD,
		.quantity = APP_ALARM_Q_TEMPERATURE,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 30.0f,
		.dwell = 0.0f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "rule setup rejected");

	g_app_sensor_data.temperature = 35.0f; /* above hi, dwell 0: fires now */
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_TEMPERATURE),
		     "threshold did not activate");
}

static const struct app_cmd_alarm_event *last_event(void)
{
	zassert_true(test_alarm_event_count > 0, "no alarm event captured");
	return &test_alarm_events[test_alarm_event_count - 1];
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
	zassert_equal(ev->slot, 0, "deactivate edge on wrong slot %u", ev->slot);
	zassert_equal(ev->source, APP_ALARM_SRC_ONBOARD, "wrong source %u", ev->source);
	zassert_equal(ev->quantity, APP_ALARM_Q_TEMPERATURE, "wrong quantity %u", ev->quantity);
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
	zassert_equal(ev->slot, 0, "deactivate edge on wrong slot %u", ev->slot);
}

ZTEST(alarm_eval, test_editing_active_rule_emits_deactivate_edge_then_rearms)
{
	g_app_config.alarm_limit = 0;
	activate_threshold_slot0();

	/* In-place edit (same source+quantity, new band): X9's HIL-confirmed
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
	zassert_false(app_alarm_is_active(APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_TEMPERATURE),
		      "latch survived the edit (35.0 is inside the new 0..40 band)");

	/* The edited rule must still evaluate and re-fire cleanly. */
	test_alarm_event_count = 0;
	g_app_sensor_data.temperature = 45.0f; /* above the NEW hi */
	app_alarm_poll();
	zassert_true(app_alarm_is_active(APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_TEMPERATURE),
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

		g_app_sensor_data.temperature = 20.0f; /* clear for the next round */
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
		.source = APP_ALARM_SRC_ONBOARD,
		.quantity = APP_ALARM_Q_HUMIDITY,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 60.0f,
		.dwell = 0.0f,
	};
	zassert_equal(app_alarm_rules_set(1, &r), 0, "rule setup rejected");
	g_app_sensor_data.humidity = 90.0f; /* above hi */

	g_buzzer_play_calls = 0;
	zassert_true(app_alarm_poll(), "aggregate unexpectedly cleared");
	zassert_equal(g_buzzer_play_calls, 1, "a NEW alarm while another is active must replay");
	zassert_equal(g_buzzer_play_last_kind, APP_BUZZER_KIND_ALARM, "wrong melody kind");

	/* Alarm #2 clearing while #1 stays active: no new melody, no stop. */
	g_app_sensor_data.humidity = 40.0f;
	g_buzzer_play_calls = 0;
	zassert_true(app_alarm_poll(), "alarm #1 should still be active");
	zassert_equal(g_buzzer_play_calls, 0, "partial deactivation must not touch the buzzer");
}

ZTEST(alarm_eval, test_buzzer_stops_on_deactivation)
{
	g_app_config.cap_buzzer = true;
	g_app_config.alarm_buzzer_mode = APP_CONFIG_ALARM_BUZZER_MODE_NORMAL;

	activate_threshold_slot0();
	g_buzzer_play_calls = 0;

	g_app_sensor_data.temperature = 20.0f; /* back in band: deactivates */
	zassert_false(app_alarm_poll(), "alarm did not clear");

	zassert_equal(g_buzzer_play_calls, 1, "deactivation must trigger exactly one buzzer call");
	zassert_equal(g_buzzer_play_last_kind, APP_BUZZER_KIND_STOP,
		      "expected a stop, not a melody");
}
