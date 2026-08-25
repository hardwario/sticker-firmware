/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for app_history (RAM ring backend): capture/get, ring wrap
 * with eviction, clock-sync base fix-up, and export.
 */

#include "app_history.h"
#include "app_config.h"
#include "app_sensor.h"

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include <limits.h>
#include <math.h>
#include <string.h>

struct app_config g_app_config;
struct app_sensor_data g_app_sensor_data;
K_MUTEX_DEFINE(g_app_sensor_data_lock);

extern bool test_clock_has;
extern uint32_t test_clock_unix;

/* NOTE (#311): `present` is uint32_t — this used to narrow to uint16_t, which
 * silently truncated to 0 for any channel index >= 16 (e.g. ILLUMINANCE, enum
 * value 16: `(uint16_t)(1u << 16)` wraps to 0). Widened when adding the first
 * channels past index 15. */
#define BIT16(n) ((uint32_t)(1u << (n)))

/* Fresh start: temperature+humidity only (NO_CAP), all caps off, clock unsynced. */
static void setup(void)
{
	memset(&g_app_config, 0, sizeof(g_app_config));
	g_app_config.history_enable = true;
	g_app_config.history_sensors = BIT(APP_HISTORY_TEMPERATURE) | BIT(APP_HISTORY_HUMIDITY);

	g_app_sensor_data = (struct app_sensor_data){0};
	test_clock_has = false;

	zassert_equal(app_history_init(), 0, "init failed");
	app_history_clear();
}

static void set_th(float t, float h)
{
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.temperature = t;
	g_app_sensor_data.humidity = h;
	k_mutex_unlock(&g_app_sensor_data_lock);
}

ZTEST(history, test_capture_and_get)
{
	setup();
	set_th(22.5f, 44.0f);
	app_history_capture();
	set_th(23.0f, 45.0f);
	app_history_capture();

	zassert_equal(app_history_count(), 2, "count %zu", app_history_count());

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get(0)");
	zassert_true(r.present & BIT16(APP_HISTORY_TEMPERATURE), "temp not present");
	zassert_true(r.present & BIT16(APP_HISTORY_HUMIDITY), "hum not present");
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], 22.5, 0.01, "temp %g",
		       r.value[APP_HISTORY_TEMPERATURE]);
	zassert_within(r.value[APP_HISTORY_HUMIDITY], 44.0, 0.5, "hum %g",
		       r.value[APP_HISTORY_HUMIDITY]);

	zassert_equal(app_history_get(1, &r), 0, "get(1)");
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], 23.0, 0.01, "temp1");

	zassert_equal(app_history_get(2, &r), -ENOENT, "get past end");
}

ZTEST(history, test_ring_wrap_evicts_oldest)
{
	setup();
	size_t cap = app_history_capacity();

	zassert_true(cap >= 4 && cap < 64, "unexpected capacity %zu", cap);

	/* Capture cap+3 records; temperature encodes the capture index. */
	size_t m = cap + 3;
	for (size_t i = 0; i < m; i++) {
		set_th((float)i, 50.0f);
		app_history_capture();
	}

	zassert_equal(app_history_count(), cap, "count should saturate at capacity");

	/* Oldest surviving record is index (m - cap). */
	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get oldest");
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], (double)(m - cap), 0.01,
		       "oldest temp %g (want %zu)", r.value[APP_HISTORY_TEMPERATURE], m - cap);

	zassert_equal(app_history_get(cap - 1, &r), 0, "get newest");
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], (double)(m - 1), 0.01, "newest temp %g",
		       r.value[APP_HISTORY_TEMPERATURE]);
}

ZTEST(history, test_clock_sync_base_fixup)
{
	setup();
	/* Capture while unsynced (timestamps are uptime-relative). */
	set_th(20.0f, 40.0f);
	app_history_capture();
	app_history_capture();

	struct app_history_record r;
	app_history_get(0, &r);
	zassert_false(r.time_synced, "should be unsynced before clock sync");

	uint32_t unix_now = 1735689600; /* 2025-01-01 */
	app_history_on_clock_sync(unix_now);

	zassert_equal(app_history_get(0, &r), 0, "get after sync");
	zassert_true(r.time_synced, "records should be synced now");
	/* base ~= unix_now (uptime at capture is a few ms). */
	zassert_within((double)r.time_unix, (double)unix_now, 5.0, "time_unix %u", r.time_unix);
}

/* #267: history reconstructs each record's time as base + ordinal*interval_report
 * — a FIXED interval, independent of the actual capture wall-time. This is why the
 * report timer must stay a fixed cadence (the fleet-uplink jitter was moved onto
 * the uplink in app_lrw): a jittered capture period would make the real sample
 * times drift from this fixed-interval reconstruction, biasing every timestamp.
 * Guard the invariant: consecutive records are exactly interval_report apart. */
ZTEST(history, test_fixed_interval_reconstruction)
{
	setup();
	g_app_config.interval_report = 60;
	for (int i = 0; i < 4; i++) {
		set_th(20.0f + (float)i, 40.0f);
		app_history_capture();
	}
	app_history_on_clock_sync(1735689600);

	struct app_history_record r0, r1, r2, r3;
	zassert_equal(app_history_get(0, &r0), 0, "get0");
	zassert_equal(app_history_get(1, &r1), 0, "get1");
	zassert_equal(app_history_get(2, &r2), 0, "get2");
	zassert_equal(app_history_get(3, &r3), 0, "get3");
	zassert_equal(r1.time_unix - r0.time_unix, 60, "rec 0->1 spacing %u",
		      r1.time_unix - r0.time_unix);
	zassert_equal(r2.time_unix - r1.time_unix, 60, "rec 1->2 spacing %u",
		      r2.time_unix - r1.time_unix);
	zassert_equal(r3.time_unix - r2.time_unix, 60, "rec 2->3 spacing %u",
		      r3.time_unix - r2.time_unix);
}

ZTEST(history, test_export)
{
	setup();
	for (int i = 0; i < 3; i++) {
		set_th(21.0f + i, 50.0f);
		app_history_capture();
	}

	uint8_t buf[128];
	uint32_t t0 = 0;
	uint16_t n = 0;
	size_t next = 0;
	size_t bytes = app_history_export_page(0, 0xFFFFFFFF, 0, buf, sizeof(buf), &t0, &n, &next);

	zassert_equal(n, 3, "n_written %u", n);
	zassert_equal(next, 3, "next_ord %zu", next);
	zassert_true(bytes > 0, "no bytes exported");

	/* count_frames mirrors the packing: 3 small records fit a single frame. */
	zassert_equal(app_history_count_frames(0, 0xFFFFFFFF, sizeof(buf)), 1, "frames");
}

/* Per-slot 1-Wire channels (s1..s4 temp/hum) become available when cap_w1_sensors
 * is on; a Dallas-like slot with NaN humidity stores the sentinel → absent. */
ZTEST(history, test_per_slot_w1_channels)
{
	setup();
	g_app_config.cap_w1_sensors = true;
	g_app_config.history_sensors |= BIT(APP_HISTORY_S1_TEMP) | BIT(APP_HISTORY_S1_HUM) |
					BIT(APP_HISTORY_S3_TEMP) | BIT(APP_HISTORY_S3_HUM);
	zassert_equal(app_history_init(), 0, "re-init with cap_w1_sensors");
	app_history_clear();

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.temperature = 20.0f;
	g_app_sensor_data.humidity = 40.0f;
	g_app_sensor_data.w1[0].temperature = 24.5f; /* s1: temp + hum (machine-probe) */
	g_app_sensor_data.w1[0].humidity = 55.0f;
	g_app_sensor_data.w1[2].temperature = 30.0f; /* s3: temp only (Dallas-like) */
	g_app_sensor_data.w1[2].humidity = NAN;
	k_mutex_unlock(&g_app_sensor_data_lock);
	app_history_capture();

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get");
	zassert_true(r.present & BIT16(APP_HISTORY_S1_TEMP), "s1-temp present");
	zassert_within(r.value[APP_HISTORY_S1_TEMP], 24.5, 0.01, "s1-temp %g",
		       r.value[APP_HISTORY_S1_TEMP]);
	zassert_true(r.present & BIT16(APP_HISTORY_S1_HUM), "s1-hum present");
	zassert_within(r.value[APP_HISTORY_S1_HUM], 55.0, 0.5, "s1-hum %g",
		       r.value[APP_HISTORY_S1_HUM]);
	zassert_true(r.present & BIT16(APP_HISTORY_S3_TEMP), "s3-temp present");
	zassert_within(r.value[APP_HISTORY_S3_TEMP], 30.0, 0.01, "s3-temp %g",
		       r.value[APP_HISTORY_S3_TEMP]);
	zassert_false(r.present & BIT16(APP_HISTORY_S3_HUM), "s3-hum absent (NaN sentinel)");
}

/* #311: pressure/illuminance/orientation/accel-motion — same fixed-width
 * present/absent pattern as the other channels, gated on their own caps. */
ZTEST(history, test_pressure_illuminance_orientation_accel_channels)
{
	setup();
	g_app_config.cap_barometer = true;
	g_app_config.cap_light_sensor = true;
	g_app_config.cap_accelerometer = true;
	g_app_config.history_sensors |= BIT(APP_HISTORY_PRESSURE) | BIT(APP_HISTORY_ILLUMINANCE) |
					BIT(APP_HISTORY_ORIENTATION) |
					BIT(APP_HISTORY_ACCEL_MOTION);
	zassert_equal(app_history_init(), 0, "re-init with new caps");
	app_history_clear();

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.pressure = 101.32f; /* kPa -> 1013.2 hPa on the wire */
	g_app_sensor_data.illuminance = 450.0f;
	g_app_sensor_data.orientation = 3;
	g_app_sensor_data.accel_motion_count = 7;
	k_mutex_unlock(&g_app_sensor_data_lock);
	app_history_capture();

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get");
	zassert_true(r.present & BIT16(APP_HISTORY_PRESSURE), "pressure present");
	zassert_within(r.value[APP_HISTORY_PRESSURE], 1013.2, 0.1, "pressure %g",
		       r.value[APP_HISTORY_PRESSURE]);
	zassert_true(r.present & BIT16(APP_HISTORY_ILLUMINANCE), "illuminance present");
	zassert_within(r.value[APP_HISTORY_ILLUMINANCE], 450.0, 2.0, "illuminance %g",
		       r.value[APP_HISTORY_ILLUMINANCE]);
	zassert_true(r.present & BIT16(APP_HISTORY_ORIENTATION), "orientation present");
	zassert_within(r.value[APP_HISTORY_ORIENTATION], 3.0, 0.01, "orientation %g",
		       r.value[APP_HISTORY_ORIENTATION]);
	zassert_true(r.present & BIT16(APP_HISTORY_ACCEL_MOTION), "accel-motion present");
	zassert_within(r.value[APP_HISTORY_ACCEL_MOTION], 7.0, 0.01, "accel-motion %g",
		       r.value[APP_HISTORY_ACCEL_MOTION]);

	/* Absent sentinels: NaN pressure/illuminance, INT_MAX orientation (the
	 * app_sensor_data default when the capability is off). */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.pressure = NAN;
	g_app_sensor_data.illuminance = NAN;
	g_app_sensor_data.orientation = INT_MAX;
	k_mutex_unlock(&g_app_sensor_data_lock);
	app_history_capture();

	zassert_equal(app_history_get(1, &r), 0, "get1");
	zassert_false(r.present & BIT16(APP_HISTORY_PRESSURE), "pressure absent (NaN sentinel)");
	zassert_false(r.present & BIT16(APP_HISTORY_ILLUMINANCE),
		      "illuminance absent (NaN sentinel)");
	zassert_false(r.present & BIT16(APP_HISTORY_ORIENTATION),
		      "orientation absent (INT_MAX sentinel)");
}

/* #396: GP_A/GP_B analog voltage — same fixed-width present/absent pattern,
 * gated on cap_analog_a/cap_analog_b. */
ZTEST(history, test_analog_input_voltage_channels)
{
	setup();
	g_app_config.cap_analog_a = true;
	g_app_config.cap_analog_b = true;
	g_app_config.history_sensors |=
		BIT(APP_HISTORY_INPUT_A_VOLTAGE) | BIT(APP_HISTORY_INPUT_B_VOLTAGE);
	zassert_equal(app_history_init(), 0, "re-init with cap_analog_a/b");
	app_history_clear();

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.input_a_voltage = 1.65f;
	g_app_sensor_data.input_b_voltage = 3.3f;
	k_mutex_unlock(&g_app_sensor_data_lock);
	app_history_capture();

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get");
	zassert_true(r.present & BIT16(APP_HISTORY_INPUT_A_VOLTAGE), "input-a-voltage present");
	zassert_within(r.value[APP_HISTORY_INPUT_A_VOLTAGE], 1.65, 0.001, "input-a-voltage %g",
		       r.value[APP_HISTORY_INPUT_A_VOLTAGE]);
	zassert_true(r.present & BIT16(APP_HISTORY_INPUT_B_VOLTAGE), "input-b-voltage present");
	zassert_within(r.value[APP_HISTORY_INPUT_B_VOLTAGE], 3.3, 0.001, "input-b-voltage %g",
		       r.value[APP_HISTORY_INPUT_B_VOLTAGE]);

	/* Absent sentinel: NaN voltage (capability off or ADC/pin conflict). */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.input_a_voltage = NAN;
	k_mutex_unlock(&g_app_sensor_data_lock);
	app_history_capture();

	zassert_equal(app_history_get(1, &r), 0, "get1");
	zassert_false(r.present & BIT16(APP_HISTORY_INPUT_A_VOLTAGE),
		      "input-a-voltage absent (NaN sentinel)");
}

ZTEST_SUITE(history, NULL, NULL, NULL, NULL, NULL);
