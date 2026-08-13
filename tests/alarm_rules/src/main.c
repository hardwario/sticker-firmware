/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for app_alarm_rules rule-shape validation (#348):
 *  - THRESHOLD: hi > lo (a collapsed/inverted/NaN band can never satisfy the
 *    deactivate check in eval_threshold(), so a rule that ever activates would
 *    latch forever, #203).
 *  - All kinds: dwell is a plain dwell/hold duration in seconds, bounded to
 *    [0, 3600] regardless of kind.
 */

#include "app_alarm_rules.h"
#include "app_config.h"

#include <zephyr/ztest.h>

#include <math.h>
#include <string.h>

/* A canonical valid THRESHOLD rule (onboard temperature). */
static struct app_alarm_rule threshold(float lo, float hi, float dwell)
{
	return (struct app_alarm_rule){
		.source = APP_ALARM_SRC_ONBOARD,
		.quantity = APP_ALARM_Q_TEMPERATURE,
		.enabled = 1,
		.lo = lo,
		.hi = hi,
		.dwell = dwell,
	};
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	app_alarm_rules_clear_all();
}

ZTEST_SUITE(alarm_rules, NULL, NULL, before, NULL, NULL);

/* ---- THRESHOLD band: the stuck-alarm bug this guards against ------------ */

ZTEST(alarm_rules, test_threshold_valid_band_accepted)
{
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 5.0f);
	zassert_equal(app_alarm_rules_set(0, &r), 0, "valid band rejected");
	zassert_true(app_alarm_rules_occupied(0), "slot not occupied after accept");
}

ZTEST(alarm_rules, test_threshold_inverted_bounds_rejected)
{
	struct app_alarm_rule r = threshold(30.0f, 20.0f, 0.0f); /* hi < lo */
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "inverted band accepted");
	zassert_false(app_alarm_rules_occupied(0), "rejected rule must not be stored");
}

ZTEST(alarm_rules, test_threshold_equal_bounds_rejected)
{
	/* hi == lo: the deactivate check (value >= lo && value <= hi) is satisfiable
	 * only by that exact value, effectively never for a real sensor reading. */
	struct app_alarm_rule r = threshold(20.0f, 20.0f, 0.0f);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "collapsed band accepted");
}

ZTEST(alarm_rules, test_threshold_nan_bound_rejected)
{
	struct app_alarm_rule r = threshold(NAN, 30.0f, 0.0f);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "NaN bound accepted");
}

ZTEST(alarm_rules, test_threshold_narrow_but_open_band_accepted)
{
	/* A narrow but non-empty band is fine now — dwell no longer eats into it. */
	struct app_alarm_rule r = threshold(10.0f, 10.01f, 60.0f);
	zassert_equal(app_alarm_rules_set(0, &r), 0, "narrow-but-open band rejected");
}

/* ---- dwell: plain dwell/hold seconds, bounded [0, 3600], all kinds --------- */

ZTEST(alarm_rules, test_threshold_zero_dwell_accepted)
{
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 0.0f); /* 0 = immediate */
	zassert_equal(app_alarm_rules_set(0, &r), 0, "zero-dwell rule rejected");
}

ZTEST(alarm_rules, test_threshold_negative_dwell_rejected)
{
	struct app_alarm_rule r = threshold(10.0f, 30.0f, -5.0f);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "negative dwell accepted");
}

ZTEST(alarm_rules, test_threshold_dwell_at_max_accepted)
{
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 3600.0f);
	zassert_equal(app_alarm_rules_set(0, &r), 0, "dwell at max rejected");
}

ZTEST(alarm_rules, test_threshold_dwell_over_max_rejected)
{
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 3600.01f);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "dwell over max accepted");
}

ZTEST(alarm_rules, test_state_dwell_range_enforced)
{
	/* STATE rules now also carry a meaningful dwell (confirm/hold seconds, #348)
	 * and must pass the same range check. */
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_INPUT_A,
		.quantity = APP_ALARM_Q_STATE,
		.enabled = 1,
		.from_state = 0,
		.to_state = 1,
		.dwell = 30.0f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "valid STATE dwell rejected");

	r.dwell = -1.0f;
	zassert_equal(app_alarm_rules_set(1, &r), -EINVAL, "negative STATE dwell accepted");
}

ZTEST(alarm_rules, test_rate_dwell_range_enforced)
{
	/* COUNT (RATE): hi is the max delta, lo is unused — lo > hi is fine, only
	 * dwell is range-checked. */
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_HALL_LEFT,
		.quantity = APP_ALARM_Q_COUNT,
		.enabled = 1,
		.lo = 99.0f,
		.hi = 5.0f,
		.dwell = 10.0f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "valid RATE dwell rejected");

	r.dwell = 99999.0f;
	zassert_equal(app_alarm_rules_set(1, &r), -EINVAL, "out-of-range RATE dwell accepted");
}

/* ---- #319: on-board illuminance is a valid THRESHOLD source ------------- */

ZTEST(alarm_rules, test_onboard_illuminance_accepted)
{
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_ONBOARD,
		.quantity = APP_ALARM_Q_ILLUMINANCE,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 50.0f,
		.dwell = 5.0f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "onboard illuminance rule rejected");
	zassert_true(app_alarm_rules_occupied(0), "onboard illuminance slot not occupied");
}

/* ---- reload path must sanitize a persisted bad band ---------------------- */

ZTEST(alarm_rules, test_reload_sanitizes_collapsed_band)
{
	/* Store a valid rule, then corrupt the persisted bytes so hi == lo —
	 * emulating a host that wrote garbage or an older FW. Reload must drop it
	 * rather than load a permanently-latching rule. */
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 1.0f);
	zassert_equal(app_alarm_rules_set(0, &r), 0, "setup set failed");

	uint8_t *field = app_config()->alarm_0; /* slot 0 packed bytes */
	zassert_true(field[0] & 0x01, "slot 0 should be marked present");
	memcpy(&field[9], &field[5], sizeof(float)); /* hi := lo */

	app_alarm_rules_reload_from_config();

	zassert_false(app_alarm_rules_occupied(0), "collapsed-band rule survived reload");
	zassert_equal(field[0], 0, "sanitized slot bytes must be zeroed");
}
