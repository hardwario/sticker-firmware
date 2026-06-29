/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for the app_history NVS flash backend
 * (CONFIG_APP_HISTORY_FLASH=y) on the native_sim flash simulator. These cover
 * what the RAM-backend suite (tests/history) cannot: the meta-write
 * coalescing (#HIGH-2) and its reboot-survival.
 *
 * The fix writes the record on every capture but coalesces the meta record
 * (count/start/base) to once every HISTORY_META_SAVE_EVERY captures. So after
 * an unclean reboot the buffer should restore to the last *coalesced* count —
 * losing visibility to at most the last (window - 1) records, which are still
 * in flash but not yet counted. A "reboot" here is a fresh app_history_init():
 * the simulator flash persists in-process, and init reloads m_count from the
 * last meta record via backend_load_meta().
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

/* Must match HISTORY_META_SAVE_EVERY in app_history.c. */
#define META_WINDOW 16

#define BIT16(n) ((uint16_t)(1u << (n)))

static void set_temp(float t)
{
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.temperature = t;
	k_mutex_unlock(&g_app_sensor_data_lock);
}

/* Simulate a reboot: the NVS flash persists in-process, so a fresh init
 * reloads the in-RAM count/start/base from the last persisted meta record. */
static void reboot(void)
{
	zassert_equal(app_history_init(), 0, "re-init (reboot) failed");
}

static void capture_n(size_t n)
{
	for (size_t i = 0; i < n; i++) {
		app_history_capture();
	}
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	memset(&g_app_config, 0, sizeof(g_app_config));
	g_app_config.history_enable = true;
	g_app_config.history_sensors = 0; /* all capability-available (temp+hum) */
	g_app_config.interval_report = 60;
	g_app_sensor_data = (struct app_sensor_data){0};
	g_app_sensor_data.temperature = 20.0f;
	g_app_sensor_data.humidity = 50.0f;
	test_clock_has = false;

	zassert_equal(app_history_init(), 0, "init failed");
	app_history_clear(); /* erase NVS + persist a clean (count 0) meta */
}

ZTEST_SUITE(history_flash, NULL, NULL, before, NULL, NULL);

/* Sanity: the backend is the flash ring (capacity far above the test counts so
 * nothing wraps), and a fresh buffer is empty. */
ZTEST(history_flash, test_flash_backend_sane)
{
	zassert_true(app_history_capacity() > 64, "expected large flash capacity, got %zu",
		     app_history_capacity());
	zassert_equal(app_history_count(), 0, "fresh buffer not empty");
}

/* Exactly one full window: meta is saved at capture 16, so all 16 survive. */
ZTEST(history_flash, test_meta_saved_at_window)
{
	capture_n(META_WINDOW);
	zassert_equal(app_history_count(), META_WINDOW, "pre-reboot count");
	reboot();
	zassert_equal(app_history_count(), META_WINDOW, "all %d should survive (saved at window)",
		      META_WINDOW);
}

/* Above the window: 20 captured, meta last saved at 16 → the 4-record tail is
 * lost on reboot (still in flash, just not counted). This is the documented
 * coalescing trade-off. */
ZTEST(history_flash, test_tail_above_window_lost)
{
	capture_n(META_WINDOW + 4);
	zassert_equal(app_history_count(), META_WINDOW + 4, "pre-reboot count");
	reboot();
	zassert_equal(app_history_count(), META_WINDOW,
		      "tail past the last coalesced save must not survive");
}

/* Below the window: 5 captured, meta never re-saved after the clean count-0
 * meta → all 5 are lost on reboot (worst case of the trade-off). */
ZTEST(history_flash, test_tail_below_window_lost)
{
	capture_n(5);
	zassert_equal(app_history_count(), 5, "pre-reboot count");
	reboot();
	zassert_equal(app_history_count(), 0, "sub-window captures are not persisted");
}

/* clear() is a forced-save site: it must persist immediately and reset the
 * dirty counter, so a reboot right after clear shows an empty buffer even
 * though records were captured (and meta saved) just before. */
ZTEST(history_flash, test_clear_forces_save)
{
	capture_n(META_WINDOW + 3); /* meta saved at 16, dirty = 3 */
	app_history_clear();        /* forced save: count 0 */
	reboot();
	zassert_equal(app_history_count(), 0, "clear() must persist the empty state");
}

/* Records themselves persist across the reboot and decode correctly. */
ZTEST(history_flash, test_records_persist_and_readable)
{
	set_temp(22.5f);
	capture_n(META_WINDOW); /* saved at the window */
	reboot();
	zassert_equal(app_history_count(), META_WINDOW, "count after reboot");

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get(0) after reboot");
	zassert_true(r.present & BIT16(APP_HISTORY_TEMPERATURE), "temp not present");
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], 22.5, 0.01, "temp %g",
		       r.value[APP_HISTORY_TEMPERATURE]);
}
