/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for the app_history raw flash ring backend (#265,
 * CONFIG_APP_HISTORY_FLASH=y) on the native_sim flash simulator configured with
 * the STM32WL geometry the backend targets: 2 KB erase pages, 8 B write unit
 * (see app.overlay). These cover what the RAM-backend suite (tests/history)
 * cannot: durable record survival across reboot, page-granularity eviction /
 * ring wrap, torn-tail loss of the unflushed staging bytes, and logical reset.
 *
 * A "reboot" is a fresh app_history_init(): the simulator flash persists
 * in-process, so init re-scans the page headers and reconstructs the ring.
 *
 * With the default mask (temperature + humidity) a record is 3 B. Records are
 * flushed to flash one 8 B double word (7 data bytes) at a time, so a capture
 * count whose byte total is a multiple of 7 (i.e. a multiple of 7 records) is
 * fully durable; otherwise the < 7 B staged tail is lost on an unclean reboot.
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

static void set_temp(float t)
{
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.temperature = t;
	k_mutex_unlock(&g_app_sensor_data_lock);
}

/* Simulate a reboot: the simulator flash persists in-process, so a fresh init
 * re-scans the page headers and rebuilds the ring. */
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
	g_app_config.history_sensors = BIT(APP_HISTORY_TEMPERATURE) | BIT(APP_HISTORY_HUMIDITY);
	g_app_config.interval_report = 60;
	g_app_sensor_data = (struct app_sensor_data){0};
	g_app_sensor_data.temperature = 20.0f;
	g_app_sensor_data.humidity = 50.0f;
	test_clock_has = false;

	zassert_equal(app_history_init(), 0, "init failed");
	app_history_clear(); /* erase the whole partition → clean slate */
}

ZTEST_SUITE(history_flash, NULL, NULL, before, NULL, NULL);

/* Sanity: the ring capacity is large and a fresh buffer is empty. */
ZTEST(history_flash, test_flash_backend_sane)
{
	zassert_true(app_history_capacity() > 64, "expected large ring capacity, got %zu",
		     app_history_capacity());
	zassert_equal(app_history_count(), 0, "fresh buffer not empty");
}

/* A double-word-aligned capture count (multiple of 7 records at 3 B each = 21 B
 * = 3 double words) is fully durable across a reboot. */
ZTEST(history_flash, test_aligned_captures_survive)
{
	capture_n(14);
	zassert_equal(app_history_count(), 14, "pre-reboot count");
	reboot();
	zassert_equal(app_history_count(), 14, "all aligned records must survive reboot");
}

/* Records themselves persist across the reboot and decode correctly. */
ZTEST(history_flash, test_records_persist_and_readable)
{
	set_temp(22.5f);
	capture_n(7);
	reboot();
	zassert_equal(app_history_count(), 7, "count after reboot");

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get(0) after reboot");
	zassert_true(r.present & BIT16(APP_HISTORY_TEMPERATURE), "temp not present");
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], 22.5, 0.01, "temp %g",
		       r.value[APP_HISTORY_TEMPERATURE]);
}

/* A non-aligned capture count loses only the < 7 B staged tail on reboot: the
 * count drops by at most a couple of records, never more. */
ZTEST(history_flash, test_unaligned_tail_bounded_loss)
{
	capture_n(10); /* 30 B = 4 double words (28 B) flushed + 2 B staged */
	zassert_equal(app_history_count(), 10, "pre-reboot count");
	reboot();
	size_t after = app_history_count();
	zassert_true(after <= 10, "count cannot grow");
	zassert_true(after >= 7, "at most the staged tail (< 7 B ≈ 2 records) may be lost, got %zu",
		     after);
}

/* Values are distinct per record and read back in order after reboot. */
ZTEST(history_flash, test_ordering_preserved)
{
	for (int i = 0; i < 21; i++) {
		set_temp((float)i * 0.10f);
		app_history_capture();
	}
	reboot();
	zassert_equal(app_history_count(), 21, "count after reboot");

	for (int i = 0; i < 21; i++) {
		struct app_history_record r;
		zassert_equal(app_history_get(i, &r), 0, "get(%d)", i);
		zassert_within(r.value[APP_HISTORY_TEMPERATURE], (double)i * 0.10, 0.01,
			       "record %d value %g", i, r.value[APP_HISTORY_TEMPERATURE]);
	}
}

/* Overflowing the ring evicts the oldest whole page(s): the count stays bounded
 * by capacity and the oldest surviving record has advanced past record 0. */
ZTEST(history_flash, test_wrap_evicts_oldest)
{
	size_t cap = app_history_capacity();

	/* Fill well past capacity so the ring must wrap and evict. */
	for (size_t i = 0; i < cap + cap / 2; i++) {
		set_temp((float)(i % 100) * 0.10f);
		app_history_capture();
	}

	size_t count = app_history_count();
	zassert_true(count <= cap, "count %zu must not exceed capacity %zu", count, cap);
	zassert_true(count > 0, "buffer must not be empty after wrap");

	/* Fewer records remain visible than were captured — the oldest were evicted
	 * a whole page at a time. */
	zassert_true(count < cap + cap / 2, "eviction must have dropped the oldest records");

	/* get(0) must still decode a valid record after wrap. */
	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get(0) after wrap");
	zassert_true(r.present & BIT16(APP_HISTORY_TEMPERATURE), "oldest temp present");
}

/* Eviction advances the oldest record's timestamp base by whole pages. */
ZTEST(history_flash, test_wrap_advances_time_base)
{
	test_clock_has = true;
	test_clock_unix = 1000000;

	size_t cap = app_history_capacity();
	capture_n(cap + 600); /* wrap by more than one page */

	struct app_history_record r0;
	zassert_equal(app_history_get(0, &r0), 0, "get(0)");
	/* Oldest record's time must be later than the very first capture's time
	 * (base advanced by the evicted page(s) × interval). */
	zassert_true(r0.time_unix > 1000000, "time base did not advance after eviction (%u)",
		     r0.time_unix);
}

/* clear() erases the partition; the empty state survives a reboot. */
ZTEST(history_flash, test_clear_persists)
{
	capture_n(14);
	app_history_clear();
	zassert_equal(app_history_count(), 0, "count after clear");
	reboot();
	zassert_equal(app_history_count(), 0, "clear() must persist the empty state");
}

/* A mask change is a logical reset: the buffer empties, and records captured
 * under the new layout survive a reboot while the old ones stay gone. */
ZTEST(history_flash, test_mask_change_resets)
{
	capture_n(14);
	zassert_equal(app_history_count(), 14, "pre-change count");

	/* Drop humidity → layout changes, buffer resets. Persist the new selection
	 * to config too (the shell does this on `settings save`) so a reboot re-seeds
	 * the same mask. */
	app_history_set_mask(BIT(APP_HISTORY_TEMPERATURE));
	g_app_config.history_sensors = BIT(APP_HISTORY_TEMPERATURE);
	zassert_equal(app_history_count(), 0, "mask change must reset the buffer");

	/* 7 records × 2 B (temp only) = 14 B = 2 double words → fully durable. */
	set_temp(11.0f);
	capture_n(7);
	zassert_equal(app_history_count(), 7, "captures under the new mask");
	reboot();
	zassert_equal(app_history_count(), 7, "new-layout records survive reboot");

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get(0)");
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], 11.0, 0.01, "temp after reset");
}
