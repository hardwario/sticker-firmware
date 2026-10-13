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
 *  - #430: the (slot, channel, sensor_type) target is checked against the
 *    sensor type registry, the 18 B blob round-trips, and a rule whose type
 *    differs from the slot's is stale (kept, counted by reload, inert).
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
		.slot = APP_ALARM_SLOT_MB,
		.channel = APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
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
	memset(&g_app_config, 0, sizeof(g_app_config));
	app_config()->sensor1_type = 0;
	app_config()->sensor2_type = 0;
}

ZTEST_SUITE(alarm_rules, NULL, NULL, before, NULL, NULL);

/* ---- THRESHOLD band: the stuck-alarm bug this guards against ------------ */

ZTEST(alarm_rules, test_threshold_valid_band_accepted)
{
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 5.0f);
	zassert_equal(app_alarm_rules_set(0, &r), 0, "valid band rejected");
	zassert_true(app_alarm_rules_occupied(0), "rule not occupied after accept");
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
		.slot = APP_ALARM_SLOT_MB,
		.channel = APP_SENSOR_CH_MOTHERBOARD_INPUT_A_STATE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
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
		.slot = APP_ALARM_SLOT_MB,
		.channel = APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
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
		.slot = APP_ALARM_SLOT_MB,
		.channel = APP_SENSOR_CH_MOTHERBOARD_ILLUMINANCE,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.enabled = 1,
		.lo = 0.0f,
		.hi = 50.0f,
		.dwell = 5.0f,
	};
	zassert_equal(app_alarm_rules_set(0, &r), 0, "onboard illuminance rule rejected");
	zassert_true(app_alarm_rules_occupied(0), "onboard illuminance rule not occupied");
}

/* ---- reload path must sanitize a persisted bad band ---------------------- */

ZTEST(alarm_rules, test_reload_sanitizes_collapsed_band)
{
	/* Store a valid rule, then corrupt the persisted bytes so hi == lo —
	 * emulating a host that wrote garbage or an older FW. Reload must drop it
	 * rather than load a permanently-latching rule. */
	struct app_alarm_rule r = threshold(10.0f, 30.0f, 1.0f);
	zassert_equal(app_alarm_rules_set(0, &r), 0, "setup set failed");

	uint8_t *field = app_config()->alarm_0; /* rule 0 packed bytes */
	zassert_true(field[0] & 0x01, "rule 0 should be marked present");
	memcpy(&field[10], &field[6], sizeof(float)); /* hi := lo */

	app_alarm_rules_reload_from_config();

	zassert_false(app_alarm_rules_occupied(0), "collapsed-band rule survived reload");
	zassert_equal(field[0], 0, "sanitized rule bytes must be zeroed");
}

/* ---- #430: target validation against the registry ------------------------ */

static struct app_alarm_rule target(uint8_t slot, uint8_t type, uint8_t ch)
{
	struct app_alarm_rule r = threshold(0.0f, 30.0f, 0.0f);

	r.slot = slot;
	r.sensor_type = type;
	r.channel = ch;
	return r;
}

ZTEST(alarm_rules, test_slot_type_pairing)
{
	struct app_alarm_rule r;

	/* Slot 0 is the motherboard only. */
	r = target(0, APP_SENSOR_TYPE_MACHINE_PROBE, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "1-Wire type on slot 0");
	/* Slots 1..4 take 1-Wire types only. */
	r = target(1, APP_SENSOR_TYPE_MOTHERBOARD, APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "motherboard type on s1");
	r = target(5, APP_SENSOR_TYPE_DALLAS, APP_SENSOR_CH_DALLAS_TEMPERATURE);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "slot 5");
	r = target(1, APP_SENSOR_TYPE_NONE, 0);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "type none");
	r = target(1, 0x7F, 0);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "unknown type");

	r = target(4, APP_SENSOR_TYPE_DALLAS, APP_SENSOR_CH_DALLAS_TEMPERATURE);
	zassert_equal(app_alarm_rules_set(0, &r), 0, "dallas on s4");
	r = target(1, APP_SENSOR_TYPE_MACHINE_PROBE, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX);
	zassert_equal(app_alarm_rules_set(1, &r), 0, "machine-probe aux temperature on s1");
}

ZTEST(alarm_rules, test_channel_validation)
{
	struct app_alarm_rule r;

	r = target(0, APP_SENSOR_TYPE_MOTHERBOARD, APP_SENSOR_CH_MOTHERBOARD_COUNT);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "channel out of range");
	r = target(1, APP_SENSOR_TYPE_DALLAS, APP_SENSOR_CH_DALLAS_COUNT);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "dallas channel 1");
	r = target(0, APP_SENSOR_TYPE_MOTHERBOARD, APP_SENSOR_CH_MOTHERBOARD_ACCEL_ORIENTATION);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "kind none (orientation)");
	r = target(0, APP_SENSOR_TYPE_MOTHERBOARD, APP_SENSOR_CH_MOTHERBOARD_ALTITUDE);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "kind none (altitude)");
	r = target(0, APP_SENSOR_TYPE_MOTHERBOARD, APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE);
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "watchdog-only (battery)");
	zassert_false(app_alarm_rules_occupied(0));
}

/* A momentary channel (PIR / accelerometer motion) only takes edge rules. */
ZTEST(alarm_rules, test_momentary_channel_edge_only)
{
	struct app_alarm_rule r = target(0, APP_SENSOR_TYPE_MOTHERBOARD,
					 APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION);

	r.from_state = 1;
	r.to_state = 1;
	zassert_equal(app_alarm_rules_set(0, &r), -EINVAL, "level rule on PIR motion");
	r.from_state = 0;
	zassert_equal(app_alarm_rules_set(0, &r), 0, "edge rule on PIR motion");
	r.channel = APP_SENSOR_CH_MOTHERBOARD_ACCEL_MOTION;
	r.from_state = 1;
	zassert_equal(app_alarm_rules_set(1, &r), -EINVAL, "level rule on accel motion");
}

/* The 18 B blob: [0] flags, [1] slot, [2] channel, [3] sensor_type, [4] from,
 * [5] to, [6] lo, [10] hi, [14] dwell (little-endian floats). */
ZTEST(alarm_rules, test_blob_layout_round_trip)
{
	struct app_alarm_rule r = target(2, APP_SENSOR_TYPE_MACHINE_PROBE,
					 APP_SENSOR_CH_MACHINE_PROBE_TILT);
	const uint8_t *f = app_config()->alarm_3;
	float hi, dwell;

	r.from_state = 0;
	r.to_state = 1;
	r.dwell = 12.5f;
	zassert_equal(sizeof(app_config()->alarm_3), 18);
	zassert_equal(app_alarm_rules_set(3, &r), 0);

	zassert_equal(f[0], 0x03, "present | enabled");
	zassert_equal(f[1], 2, "slot");
	zassert_equal(f[2], APP_SENSOR_CH_MACHINE_PROBE_TILT, "channel");
	zassert_equal(f[3], APP_SENSOR_TYPE_MACHINE_PROBE, "sensor_type");
	zassert_equal(f[4], 0, "from");
	zassert_equal(f[5], 1, "to");
	memcpy(&hi, &f[10], sizeof(hi));
	memcpy(&dwell, &f[14], sizeof(dwell));
	zassert_equal(hi, 30.0f);
	zassert_equal(dwell, 12.5f);

	app_config()->sensor2_type = APP_SENSOR_TYPE_MACHINE_PROBE;
	zassert_equal(app_alarm_rules_reload_from_config(), 0, "clean reload");

	struct app_alarm_rule back;

	zassert_true(app_alarm_rules_get(3, &back));
	zassert_equal(back.slot, 2);
	zassert_equal(back.channel, APP_SENSOR_CH_MACHINE_PROBE_TILT);
	zassert_equal(back.sensor_type, APP_SENSOR_TYPE_MACHINE_PROBE);
	zassert_equal(back.to_state, 1);
	zassert_equal(back.dwell, 12.5f);
}

/* A rule whose type differs from the slot's staged type is stale: accepted by
 * set (the type may be provisioned later), kept by reload but counted. */
ZTEST(alarm_rules, test_stale_rule_kept_and_counted)
{
	struct app_alarm_rule r = target(1, APP_SENSOR_TYPE_MACHINE_PROBE,
					 APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE);

	zassert_equal(app_alarm_rules_set(0, &r), 0, "stale rule must be accepted by set");
	zassert_equal(app_alarm_rules_stale_count(), 1, "untyped slot");

	app_config()->sensor1_type = APP_SENSOR_TYPE_MACHINE_PROBE;
	zassert_equal(app_alarm_rules_stale_count(), 0);
	zassert_equal(app_alarm_rules_reload_from_config(), 0);

	app_config()->sensor1_type = APP_SENSOR_TYPE_DALLAS; /* slot re-typed */
	zassert_equal(app_alarm_rules_reload_from_config(), 1, "stale rule not reported");
	zassert_true(app_alarm_rules_occupied(0), "stale rule must be kept");
	zassert_equal(app_alarm_rules_stale_count(), 1);
}

/* Armed = enabled, not stale under the runtime config, capability on. */
ZTEST(alarm_rules, test_armed)
{
	struct app_alarm_rule r = threshold(0.0f, 30.0f, 0.0f);

	zassert_equal(app_alarm_rules_set(0, &r), 0, "cap off: still accepted");
	zassert_false(app_alarm_rule_armed(&r), "cap_sht off: inert");
	g_app_config.cap_sht = true;
	zassert_true(app_alarm_rule_armed(&r));
	r.enabled = 0;
	zassert_false(app_alarm_rule_armed(&r), "disabled");

	struct app_alarm_rule w = target(3, APP_SENSOR_TYPE_DALLAS,
					 APP_SENSOR_CH_DALLAS_TEMPERATURE);

	zassert_true(app_alarm_rule_stale(&w));
	zassert_false(app_alarm_rule_armed(&w), "stale: inert");
	g_app_config.sensor3_type = APP_SENSOR_TYPE_DALLAS;
	zassert_false(app_alarm_rule_stale(&w));
	zassert_true(app_alarm_rule_armed(&w), "1-Wire channel has no cap");
}
