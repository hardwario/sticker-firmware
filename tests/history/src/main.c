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

#include <math.h>
#include <string.h>

struct app_config g_app_config;
struct app_sensor_data g_app_sensor_data;
K_MUTEX_DEFINE(g_app_sensor_data_lock);

extern bool test_clock_has;
extern uint32_t test_clock_unix;

#define BIT16(n) ((uint16_t)(1u << (n)))

/* Fresh start: temperature+humidity only (NO_CAP), all caps off, clock unsynced. */
static void setup(void)
{
	memset(&g_app_config, 0, sizeof(g_app_config));
	g_app_config.history_enable = true;
	g_app_config.history_sensors = 0; /* 0 => all capability-available */

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

ZTEST(history, test_export)
{
	setup();
	for (int i = 0; i < 3; i++) {
		set_th(21.0f + i, 50.0f);
		app_history_capture();
	}

	uint8_t buf[128];
	uint32_t t0 = 0;
	uint16_t n = 0, total = 0;
	size_t bytes = app_history_export(0, 0xFFFFFFFF, buf, sizeof(buf), &t0, &n, &total);

	zassert_equal(total, 3, "total %u", total);
	zassert_equal(n, 3, "n_written %u", n);
	zassert_true(bytes > 0, "no bytes exported");
}

ZTEST_SUITE(history, NULL, NULL, NULL, NULL, NULL);
