/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for app_alarm_rules rule-shape validation, focused on the
 * THRESHOLD hysteresis dead-band guard: a rule whose clear window
 * (lo+hst, hi-hst) is empty can never clear once latched, so it must be
 * rejected on the write path and sanitized on reload.
 */

#include "app_alarm_rules.h"
#include "app_config.h"

#include <zephyr/ztest.h>

#include <math.h>
#include <string.h>

/* A canonical valid THRESHOLD rule (onboard temperature). */
static struct app_alarm_rule threshold(float lo, float hi, float hst)
{
	return (struct app_alarm_rule){
		.source = APP_ALARM_SRC_ONBOARD,
		.quantity = APP_ALARM_Q_TEMPERATURE,
		.enabled = 1,
		.lo = lo,
		.hi = hi,
		.hst = hst,
	};
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	app_alarm_rules_clear_all();
}

ZTEST_SUITE(alarm_rules, NULL, NULL, before, NULL, NULL);

/* ---- THRESHOLD dead-band: the bug this guards against ------------------- */

ZTEST(alarm_rules, test_threshold_valid_band_accepted)
{
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 1.0f); /* band 20 > 2*hst */
	zassert_equal(app_alarm_rules_set(0, &r), 0, "valid band rejected");
	zassert_true(app_alarm_rules_occupied(0), "slot not occupied after accept");
}

ZTEST(alarm_rules, test_threshold_narrow_band_rejected)
{
	/* hi - lo = 1, 2*hst = 2 -> empty clear window -> would latch forever. */
	struct app_alarm_rule r = threshold(20.0f, 21.0f, 1.0f);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "narrow band accepted");
	zassert_false(app_alarm_rules_occupied(0), "rejected rule must not be stored");
}

ZTEST(alarm_rules, test_threshold_inverted_bounds_rejected)
{
	struct app_alarm_rule r = threshold(30.0f, 20.0f, 0.0f); /* hi < lo */
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "inverted band accepted");
}

ZTEST(alarm_rules, test_threshold_exactly_2hst_rejected)
{
	/* hi - lo == 2*hst: clear window is a single point and the comparisons are
	 * strict, so it never clears -> reject the boundary too. */
	struct app_alarm_rule r = threshold(10.0f, 12.0f, 1.0f);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "boundary band accepted");
}

ZTEST(alarm_rules, test_threshold_just_above_2hst_accepted)
{
	struct app_alarm_rule r = threshold(10.0f, 12.01f, 1.0f); /* just over 2*hst */
	zassert_equal(app_alarm_rules_set(0, &r), 0, "valid narrow-but-open band rejected");
}

ZTEST(alarm_rules, test_threshold_zero_hysteresis_accepted)
{
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 0.0f); /* hst 0 = no dead-band */
	zassert_equal(app_alarm_rules_set(0, &r), 0, "zero-hst band rejected");
}

ZTEST(alarm_rules, test_threshold_negative_hysteresis_clamped)
{
	/* hst < 0 is clamped to 0 (matches eval_threshold), so the band is open. */
	struct app_alarm_rule r = threshold(10.0f, 30.0f, -5.0f);
	zassert_equal(app_alarm_rules_set(0, &r), 0, "negative hst should clamp, not reject");
}

ZTEST(alarm_rules, test_threshold_nan_bound_rejected)
{
	struct app_alarm_rule r = threshold(NAN, 30.0f, 0.0f);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "NaN bound accepted");
}

/* ---- #319: on-board illuminance is now a valid THRESHOLD source --------- */

ZTEST(alarm_rules, test_onboard_illuminance_accepted)
{
	/* Part A of #319 made the on-board OPT3001 alarm-capable. A high-illuminance
	 * band on the onboard source must now validate (it was rejected before). */
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_ONBOARD,
		.quantity = APP_ALARM_Q_ILLUMINANCE,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 50.0f,
		.hst = 5.0f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "onboard illuminance rule rejected");
	zassert_true(app_alarm_rules_occupied(0), "onboard illuminance slot not occupied");
}

/* ---- non-THRESHOLD kinds must NOT be band-checked ----------------------- */

ZTEST(alarm_rules, test_rate_rule_ignores_band)
{
	/* COUNT (RATE): hi is the max delta, lo is unused — lo > hi is fine. */
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_HALL_LEFT,
		.quantity = APP_ALARM_Q_COUNT,
		.enabled = 1,
		.lo = 99.0f,
		.hi = 5.0f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "RATE rule wrongly band-checked");
}

ZTEST(alarm_rules, test_state_rule_ignores_band)
{
	struct app_alarm_rule r = {
		.source = APP_ALARM_SRC_INPUT_A,
		.quantity = APP_ALARM_Q_STATE,
		.enabled = 1,
		.from_state = 0,
		.to_state = 1,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "STATE rule wrongly band-checked");
}

/* ---- reload path must sanitize a persisted empty-band rule --------------- */

ZTEST(alarm_rules, test_reload_sanitizes_empty_band)
{
	/* Store a valid rule, then corrupt the persisted bytes so hi == lo (empty
	 * band) — emulating a host that wrote garbage or an older FW. Reload must
	 * drop it rather than load a permanently-latching rule. */
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 1.0f);
	zassert_equal(app_alarm_rules_set(0, &r), 0, "setup set failed");

	uint8_t *field = app_config()->alarm_0; /* slot 0 packed bytes */
	zassert_true(field[0] & 0x01, "slot 0 should be marked present");
	memcpy(&field[9], &field[5], sizeof(float)); /* hi := lo */

	app_alarm_rules_reload_from_config();

	zassert_false(app_alarm_rules_occupied(0), "empty-band rule survived reload");
	zassert_equal(field[0], 0, "sanitized slot bytes must be zeroed");
}
