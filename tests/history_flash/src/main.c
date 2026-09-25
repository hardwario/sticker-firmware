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

/* Regression for advance_page()'s success-path reorder (page rollover now
 * defers all in-RAM commits — m_live[] eviction shift, m_nlive, m_next_seq,
 * m_last_phys, the new live entry, m_head_dw/m_stage_len reset — until AFTER
 * both flash_area_erase() and flash_area_write() report success). This test
 * has no way to force either call to fail (the native_sim flash simulator
 * only exposes operation-count stats, not fault injection — see
 * flash_simulator.c), so it only exercises the happy path: several page
 * rollovers, one right after another, must evict exactly one page's worth of
 * records each time and leave the ring's ordinal/time bookkeeping identical to
 * what test_wrap_evicts_oldest / test_wrap_advances_time_base already expect.
 * The failure path itself (early return, zero mutation) is verified by
 * inspection only, not by this suite. */
ZTEST(history_flash, test_advance_page_multi_wrap_consistent)
{
	test_clock_has = true;
	test_clock_unix = 2000000;

	size_t cap = app_history_capacity();

	/* Drive several full-ring wraps back to back (multiple advance_page() page
	 * rollovers, several of which evict the tail page). */
	capture_n(cap * 3);

	size_t count = app_history_count();
	zassert_true(count <= cap, "count %zu must not exceed capacity %zu", count, cap);
	zassert_true(count > 0, "buffer must not be empty after repeated wraps");

	/* The ring must still be internally consistent: oldest and newest records
	 * both decode, and time strictly increases across the whole live window. */
	struct app_history_record r0, rlast;
	zassert_equal(app_history_get(0, &r0), 0, "get(0) after repeated wraps");
	zassert_equal(app_history_get(count - 1, &rlast), 0, "get(last) after repeated wraps");
	zassert_true(rlast.time_unix > r0.time_unix,
		     "time must advance across the retained window (%u -> %u)", r0.time_unix,
		     rlast.time_unix);
	zassert_true(r0.time_unix > 2000000,
		     "oldest surviving record's time base must have advanced past the very "
		     "first capture (%u)",
		     r0.time_unix);

	/* Ring stays mountable/consistent across a reboot after the repeated
	 * rollovers, and the logical count is unaffected by re-scanning headers. */
	reboot();
	zassert_equal(app_history_count(), count,
		      "count must be stable across reboot after repeated wraps");
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

/* #340 L7 regression: a logical reset that does NOT change the mask/layout (e.g.
 * `history sensors <same> on`) left m_next_seq/m_last_phys untouched, so the next
 * page written after the reset continued the pre-reset seq numbering exactly one
 * step above the last pre-reset page. backend_mount()'s backward chain walk
 * matches on a seq delta of exactly 1 (plus matching mask/interval/sample_size),
 * so after a reboot it wrongly re-attached that stale pre-reset page to the new
 * one. Force several real page wraps first so the stale chain's first_ord is
 * large (mirrors production where the device has run a while before a reset),
 * then reset with the SAME mask, capture a small aligned batch, and reboot: the
 * count must reflect only the post-reset batch, and its oldest record must carry
 * the post-reset value, not a resurrected pre-reset one.
 */
ZTEST(history_flash, test_same_mask_reset_does_not_chain_stale_page)
{
	size_t cap = app_history_capacity();

	set_temp(5.0f);
	capture_n(cap + cap / 2); /* force real wraps -> large first_ord baseline */

	uint32_t mask = app_history_get_mask();
	app_history_set_mask(mask); /* logical reset, mask unchanged */
	zassert_equal(app_history_count(), 0, "same-mask reset must still empty the buffer");

	set_temp(99.0f);
	capture_n(7); /* 21 B = 3 double words -> fully durable */
	zassert_equal(app_history_count(), 7, "count right after the post-reset batch");

	reboot();

	zassert_equal(app_history_count(), 7,
		      "post-reset count must not be corrupted by a stale pre-reset page chain");

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0, "get(0) after reboot");
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], 99.0, 0.01,
		       "oldest surviving record must be from the post-reset batch, got %g "
		       "(stale pre-reset page leaked in)",
		       r.value[APP_HISTORY_TEMPERATURE]);
}

/* ---- F28: post-reboot record timestamps --------------------------------- */

#define F28_T0     1750000000u  /* first record, RTC synced */
#define F28_OUTAGE (5u * 3600u) /* device powered off for 5 h */

/* Seven durable records (21 B = 3 double words) on the 60 s RTC cadence; returns
 * with the clock at the next slot (T0 + 7 * 60). */
static void f28_fill_before_outage(void)
{
	test_clock_has = true;
	test_clock_unix = F28_T0;
	for (int i = 0; i < 7; i++) {
		app_history_capture();
		test_clock_unix += 60;
	}
	zassert_equal(app_history_count(), 7, "pre-outage count");
}

/* Time the host reconstructs for record `ord`: the HistoryFrame t0 of an
 * export starting at that record (time(j) = t0 + j * interval_s). */
static uint32_t f28_frame_t0(size_t ord)
{
	uint8_t buf[64];
	uint32_t t0 = 0;
	uint16_t n = 0;
	size_t next = 0;

	(void)app_history_export_page(0, UINT32_MAX, ord, buf, sizeof(buf), &t0, &n, &next);
	zassert_true(n > 0, "no record exported at ord %zu", ord);
	return t0;
}

/* F28, RTC kept across the outage (backup domain, e.g. a watchdog reset): the
 * first record after the reboot is captured at T0 + 7 * 60 + outage.
 *
 * CHARACTERIZATION of the unfixed code: the new page's base is the ordinal
 * continuation of the old ring (m_base_time + (m_abs_ord - tail.first_ord) *
 * interval), so the record claims T0 + 7 * 60, i.e. it is shifted back by the
 * whole outage. The fix (RTC base per page) flips this assertion to 0. */
ZTEST(history_flash, test_f28_reboot_rtc_kept_shift)
{
	f28_fill_before_outage();
	reboot();

	test_clock_unix += F28_OUTAGE;
	uint32_t t_capture = test_clock_unix;
	app_history_capture();
	zassert_equal(app_history_count(), 8, "post-boot count");

	struct app_history_record r;
	zassert_equal(app_history_get(7, &r), 0, "get(7)");
	int32_t shift_get = (int32_t)(r.time_unix - t_capture);
	int32_t shift_frame = (int32_t)(f28_frame_t0(7) - t_capture);

	printk("F28 (RTC kept): record time shift %d s (get), %d s (frame t0), synced=%d\n",
	       shift_get, shift_frame, r.time_synced);
	zassert_equal(shift_get, -(int32_t)F28_OUTAGE, "F28 shift (get) %d", shift_get);
	zassert_equal(shift_frame, -(int32_t)F28_OUTAGE, "F28 shift (frame) %d", shift_frame);
}

/* F28, power loss: the RTC is unset after the reboot until the network
 * DeviceTimeAns (app_clock_set_unix -> app_history_on_clock_sync) arrives 30 s
 * after the first post-boot capture.
 *
 * CHARACTERIZATION of the unfixed code: the restored base is "synced" (from the
 * tail page), so the post-boot page is stamped synced by ordinal continuation
 * and on_clock_sync() leaves it alone -> shifted back by the outage. */
ZTEST(history_flash, test_f28_power_loss_shift)
{
	f28_fill_before_outage();
	reboot();

	uint32_t t_capture = test_clock_unix + F28_OUTAGE;
	test_clock_has = false; /* RTC lost with the supply */
	app_history_capture();
	zassert_equal(app_history_count(), 8, "post-boot count");

	k_sleep(K_SECONDS(30)); /* join + DeviceTimeAns */
	test_clock_has = true;
	test_clock_unix = t_capture + 30;
	app_history_on_clock_sync(test_clock_unix);

	struct app_history_record r;
	zassert_equal(app_history_get(7, &r), 0, "get(7)");
	int32_t shift_get = (int32_t)(r.time_unix - t_capture);
	int32_t shift_frame = (int32_t)(f28_frame_t0(7) - t_capture);

	printk("F28 (power loss): record time shift %d s (get), %d s (frame t0), synced=%d\n",
	       shift_get, shift_frame, r.time_synced);
	zassert_true(r.time_synced, "unfixed code claims the record synced");
	zassert_equal(shift_get, -(int32_t)F28_OUTAGE, "F28 shift (get) %d", shift_get);
	zassert_equal(shift_frame, -(int32_t)F28_OUTAGE, "F28 shift (frame) %d", shift_frame);
}
