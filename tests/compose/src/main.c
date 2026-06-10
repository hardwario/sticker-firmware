/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for app_compose (telemetry composition): capability gating,
 * per-group flags, budget fit and lossless multi-frame split.
 */

#include "app_compose.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_sensor.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_w1_slots.h"

#include <pb_decode.h>
#include "src/app_config.pb.h"

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>

#include <limits.h>
#include <math.h>
#include <string.h>

/* Seeded globals the module under test reads. */
struct app_config g_app_config;
struct app_sensor_data g_app_sensor_data;
K_MUTEX_DEFINE(g_app_sensor_data_lock);

/* Stub-controlled state (defined in stubs.c). */
extern uint8_t test_budget;
extern struct app_hall_data test_hall;
extern struct app_input_data test_input;
extern enum app_w1_slot_type test_w1_types[APP_W1_SLOT_COUNT];

#define SYSTEM_FLAG_BOOT 0x1

static void set_clean(void)
{
	memset(&g_app_config, 0, sizeof(g_app_config));
	memset(&test_hall, 0, sizeof(test_hall));
	memset(&test_input, 0, sizeof(test_input));
	memset(test_w1_types, 0, sizeof(test_w1_types)); /* all slots empty */
	test_budget = 200;

	g_app_sensor_data = (struct app_sensor_data){0};
	g_app_sensor_data.orientation = INT_MAX; /* absent */
	/* Analog scalars: NaN = absent. */
	g_app_sensor_data.voltage = NAN;
	g_app_sensor_data.temperature = NAN;
	g_app_sensor_data.humidity = NAN;
	g_app_sensor_data.illuminance = NAN;
	g_app_sensor_data.altitude = NAN;
	g_app_sensor_data.pressure = NAN;
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		g_app_sensor_data.w1[s].temperature = NAN;
		g_app_sensor_data.w1[s].humidity = NAN;
	}
}

static Telemetry decode(const uint8_t *buf, size_t len)
{
	Telemetry t = Telemetry_init_zero;

	/* buf[0] is the APP_PROTO_VERSION prefix (#55); decode the protobuf after it. */
	zassert_true(len >= 1, "missing version byte");
	zassert_equal(buf[0], APP_PROTO_VERSION, "bad version 0x%02x", buf[0]);
	pb_istream_t is = pb_istream_from_buffer(buf + 1, len - 1);

	zassert_true(pb_decode(&is, Telemetry_fields, &t), "pb_decode failed");
	return t;
}

/* Drive one full report to completion; return the frames and their count. */
static void run_report(Telemetry *frames, size_t max, size_t *n)
{
	uint8_t buf[256];
	bool more = true;
	*n = 0;

	while (more) {
		size_t len = 0;
		int ret = app_compose(buf, sizeof(buf), &len, &more);

		zassert_equal(ret, 0, "app_compose ret %d", ret);
		zassert_true(len <= test_budget, "frame %zuB > budget %uB", len, test_budget);
		if (len == 0) {
			break; /* nothing-to-report case */
		}
		zassert_true(*n < max, "too many frames");
		frames[(*n)++] = decode(buf, len);
	}
}

/* NOTE: tests run in source order; the very first report carries the one-shot
 * boot flag, so test_boot_internal must come first. */

ZTEST(compose, test_boot_internal)
{
	Telemetry fr[8];
	size_t n;

	set_clean();
	g_app_sensor_data.temperature = 23.5f;
	g_app_sensor_data.humidity = 50.0f;

	run_report(fr, 8, &n);

	zassert_equal(n, 1, "expected one frame, got %zu", n);
	zassert_true(fr[0].has_temperature, "temperature missing");
	zassert_equal(fr[0].temperature, 2350, "temp scaled wrong: %d", fr[0].temperature);
	zassert_true(fr[0].has_humidity, "humidity missing");
	zassert_equal(fr[0].humidity, 100, "hum scaled wrong: %u", fr[0].humidity);
	/* #80: the system group is always present; an absent (NaN) voltage is
	 * sent as a 0 sentinel rather than omitted. */
	zassert_true(fr[0].has_voltage, "system voltage must always be present");
	zassert_equal(fr[0].voltage, 0, "absent voltage -> 0 sentinel, got %u", fr[0].voltage);
	zassert_true(fr[0].has_system_flags, "boot system_flags missing");
	zassert_equal(fr[0].system_flags, SYSTEM_FLAG_BOOT, "boot flag wrong");
}

ZTEST(compose, test_capability_gating)
{
	Telemetry fr[8];
	size_t n;

	/* barometer capability OFF -> pressure dropped even with valid data */
	set_clean();
	g_app_sensor_data.pressure = 1000.0f;
	g_app_config.cap_barometer = false;
	run_report(fr, 8, &n);
	for (size_t i = 0; i < n; i++) {
		zassert_false(fr[i].has_pressure, "pressure leaked with cap off");
	}

	/* barometer capability ON -> pressure present and scaled x1000 */
	set_clean();
	g_app_sensor_data.pressure = 1000.0f;
	g_app_config.cap_barometer = true;
	run_report(fr, 8, &n);
	bool seen = false;
	for (size_t i = 0; i < n; i++) {
		if (fr[i].has_pressure) {
			seen = true;
			zassert_equal(fr[i].pressure, 1000000, "pressure scale");
		}
	}
	zassert_true(seen, "pressure missing with cap on");
}

ZTEST(compose, test_counter_flags)
{
	Telemetry fr[8];
	size_t n;

	set_clean();
	g_app_config.cap_hall_left = true;
	test_hall.left_count = 7;
	test_hall.left_is_active = true;
	test_hall.left_notify_act = true;
	run_report(fr, 8, &n);

	bool seen = false;
	for (size_t i = 0; i < n; i++) {
		if (fr[i].has_hall_left_count) {
			seen = true;
			zassert_equal(fr[i].hall_left_count, 7, "hall count");
			zassert_true(fr[i].has_hall_left_flags, "flags missing");
			/* NOTIFY_ACT(bit0) | ACTIVE(bit2) = 0x5 */
			zassert_equal(fr[i].hall_left_flags, 0x5, "flags %u",
				      fr[i].hall_left_flags);
		}
	}
	zassert_true(seen, "hall_left missing");
}

ZTEST(compose, test_multiframe_split)
{
	Telemetry fr[16];
	size_t n;

	set_clean();
	/* Enable several independent groups with data. */
	g_app_sensor_data.temperature = 20.0f;
	g_app_sensor_data.humidity = 40.0f;
	g_app_config.cap_barometer = true;
	g_app_sensor_data.pressure = 990.0f;
	g_app_config.cap_light_sensor = true;
	g_app_sensor_data.illuminance = 300.0f;
	g_app_config.cap_w1_sensors = true;
	test_w1_types[0] = APP_W1_SLOT_DALLAS;
	g_app_sensor_data.w1[0].temperature = 11.0f;
	g_app_config.cap_hall_left = true;
	test_hall.left_count = 42;

	/* Small budget forces several frames, but must still hold the largest
	 * single unit alone (a w1 SensorReading ~10 B); smaller would trip the
	 * oversized-unit stall-guard rather than test the split. */
	test_budget = 16;
	run_report(fr, 16, &n);

	zassert_true(n > 1, "expected a multi-frame split, got %zu", n);

	/* Lossless + disjoint: each field/reading appears in exactly one frame. */
	int temp = 0, hum = 0, press = 0, illum = 0, w1 = 0, hall = 0;
	for (size_t i = 0; i < n; i++) {
		temp += fr[i].has_temperature;
		hum += fr[i].has_humidity;
		press += fr[i].has_pressure;
		illum += fr[i].has_illuminance;
		w1 += fr[i].w1_sensors_count; /* one slot reading total */
		hall += fr[i].has_hall_left_count;
	}
	zassert_equal(temp, 1, "temperature not exactly once (%d)", temp);
	zassert_equal(hum, 1, "humidity not exactly once");
	zassert_equal(press, 1, "pressure not exactly once");
	zassert_equal(illum, 1, "illuminance not exactly once");
	zassert_equal(w1, 1, "w1 reading not exactly once (%d)", w1);
	zassert_equal(hall, 1, "hall_left not exactly once");
}

ZTEST(compose, test_machine_probe_cluster)
{
	Telemetry fr[4];
	size_t n;

	set_clean();
	g_app_config.cap_w1_sensors = true;
	test_w1_types[0] = APP_W1_SLOT_MACHINE_PROBE;
	g_app_sensor_data.w1[0].temperature = 23.65f;
	g_app_sensor_data.w1[0].humidity = 54.0f;
	g_app_sensor_data.w1[0].illuminance = 27.0f;
	g_app_sensor_data.w1[0].magnetic_field = 0.062f; /* mT */
	g_app_sensor_data.w1[0].accel_x = 0.38f;
	g_app_sensor_data.w1[0].accel_y = -9.35f;
	g_app_sensor_data.w1[0].accel_z = -0.54f;
	g_app_sensor_data.w1[0].is_tilt_alert = true;
	g_app_sensor_data.w1[0].present = true;

	run_report(fr, 4, &n);

	/* One SensorReading carrying the whole cluster, in one frame (ample budget). */
	zassert_equal(n, 1, "expected one frame, got %zu", n);
	zassert_equal(fr[0].w1_sensors_count, 1, "expected one w1 reading");
	const SensorReading *sr = &fr[0].w1_sensors[0];
	zassert_equal(sr->slot, 1, "slot (1-based: internal slot 0 -> wire 1)");
	zassert_equal(sr->type, APP_W1_SLOT_MACHINE_PROBE, "type");
	zassert_true(sr->has_temperature && sr->temperature == 2365, "temperature %d",
		     sr->temperature);
	zassert_true(sr->has_humidity && sr->humidity == 108, "humidity %u", sr->humidity);
	zassert_true(sr->has_illuminance && sr->illuminance == 27, "lux %u", sr->illuminance);
	zassert_true(sr->has_magnetic_field && sr->magnetic_field == 62, "field %d",
		     sr->magnetic_field);
	zassert_true(sr->has_accel_x && sr->accel_x == 38, "ax %d", sr->accel_x);
	zassert_true(sr->has_accel_y && sr->accel_y == -935, "ay %d", sr->accel_y);
	zassert_true(sr->has_accel_z && sr->accel_z == -54, "az %d", sr->accel_z);
	zassert_true(sr->has_flags && sr->flags == 1, "tilt flag %u", sr->flags);
}

ZTEST(compose, test_dallas_temperature_only)
{
	Telemetry fr[4];
	size_t n;

	set_clean();
	g_app_config.cap_w1_sensors = true;
	test_w1_types[0] = APP_W1_SLOT_DALLAS;
	g_app_sensor_data.w1[0].temperature = 21.5f;
	g_app_sensor_data.w1[0].present = true;

	run_report(fr, 4, &n);

	zassert_equal(fr[0].w1_sensors_count, 1, "expected one w1 reading");
	const SensorReading *sr = &fr[0].w1_sensors[0];
	zassert_true(sr->has_temperature && sr->temperature == 2150, "temperature %d",
		     sr->temperature);
	/* Dallas is temperature-only: cluster + flags must be absent. */
	zassert_false(sr->has_humidity, "dallas humidity leaked");
	zassert_false(sr->has_flags, "dallas flags leaked");
	zassert_false(sr->has_illuminance, "dallas lux leaked");
	zassert_false(sr->has_magnetic_field, "dallas field leaked");
	zassert_false(sr->has_accel_x, "dallas accel leaked");
}

ZTEST(compose, test_system_always_present)
{
	Telemetry fr[8];
	size_t n;

	set_clean(); /* all NaN, boot flag already consumed by test_boot_internal */
	run_report(fr, 8, &n);

	/* #80: the system group is always emitted, so a report is never empty even
	 * when every sensor reading is absent. boot was consumed earlier -> flags 0. */
	zassert_equal(n, 1, "expected one frame, got %zu", n);
	zassert_true(fr[0].has_voltage, "system voltage must always be present");
	zassert_equal(fr[0].voltage, 0, "absent voltage -> 0 sentinel, got %u", fr[0].voltage);
	zassert_true(fr[0].has_system_flags, "system_flags must always be present");
	zassert_equal(fr[0].system_flags, 0, "boot consumed -> flags 0, got %u",
		      fr[0].system_flags);
	/* No sensor groups leak in when nothing is available. */
	zassert_false(fr[0].has_temperature, "temperature leaked");
	zassert_false(fr[0].has_hall_left_count, "hall_left leaked");
}

ZTEST(compose, test_budget_unknown_pre_join)
{
	uint8_t buf[64];
	size_t len = 0;
	bool more = false;

	set_clean();
	g_app_sensor_data.temperature = 20.0f;
	test_budget = 0; /* app_lrw_get_max_payload() == 0 -> pre-join */
	int ret = app_compose(buf, sizeof(buf), &len, &more);

	zassert_equal(ret, -EAGAIN, "expected -EAGAIN, got %d", ret);
}

ZTEST_SUITE(compose, NULL, NULL, NULL, NULL, NULL);
