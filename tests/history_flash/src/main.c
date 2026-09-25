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
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <math.h>
#include <stddef.h>
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
	app_history_set_work_queue(NULL);

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

	(void)app_history_export_page(0, UINT32_MAX, ord, buf, sizeof(buf), &t0, NULL, &n, &next);
	zassert_true(n > 0, "no record exported at ord %zu", ord);
	return t0;
}

/* F28, RTC kept across the outage (backup domain, e.g. a watchdog reset): the
 * first record after the reboot is captured at T0 + 7 * 60 + outage. The
 * post-boot page is stamped from the RTC, not by ordinal continuation of the
 * old ring (which put it at T0 + 7 * 60, -18000 s off before the fix), and the
 * export splits at the page boundary so each frame's t0 is right. */
ZTEST(history_flash, test_f28_reboot_rtc_kept)
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
	zassert_equal(shift_get, 0, "F28 shift (get) %d", shift_get);
	zassert_equal(shift_frame, 0, "F28 shift (frame) %d", shift_frame);
	zassert_true(r.time_synced, "RTC-stamped page is synced");

	/* Pre-outage records keep their times. */
	zassert_equal(app_history_get(6, &r), 0, "get(6)");
	zassert_equal(r.time_unix, F28_T0 + 6 * 60, "pre-outage time %u", r.time_unix);

	/* The whole window takes two frames: one per page, never across the gap. */
	uint8_t buf[64];
	uint32_t t0;
	uint16_t n;
	size_t next;

	zassert_equal(app_history_count_frames(0, UINT32_MAX, sizeof(buf)), 2, "frames");
	(void)app_history_export_page(0, UINT32_MAX, 0, buf, sizeof(buf), &t0, NULL, &n, &next);
	zassert_equal(n, 7, "first frame ends at the page boundary (%u)", n);
	zassert_equal(t0, F28_T0);
	zassert_equal(next, 7);

	/* A window around the outage end finds the post-boot record. */
	zassert_equal(app_history_count_frames(t_capture - 30, t_capture + 30, sizeof(buf)), 1);
}

/* F28, power loss: the RTC is unset after the reboot until the network
 * DeviceTimeAns (app_clock_set_unix -> app_history_on_clock_sync) arrives 30 s
 * after the first post-boot capture. The post-boot page is stamped on uptime
 * (base_synced=0) and re-based by the (unix - uptime) offset at the sync, so the
 * record lands at its capture time (before the fix: -18000 s and wrongly
 * claimed synced). */
ZTEST(history_flash, test_f28_power_loss)
{
	f28_fill_before_outage();
	reboot();

	uint32_t t_capture = test_clock_unix + F28_OUTAGE;
	test_clock_has = false; /* RTC lost with the supply */
	app_history_capture();
	zassert_equal(app_history_count(), 8, "post-boot count");

	struct app_history_record r;
	zassert_equal(app_history_get(7, &r), 0, "get(7)");
	zassert_false(r.time_synced, "no RTC yet: the post-boot page is unsynced");

	k_sleep(K_SECONDS(30)); /* join + DeviceTimeAns */
	test_clock_has = true;
	test_clock_unix = t_capture + 30;
	app_history_on_clock_sync(test_clock_unix);

	zassert_equal(app_history_get(7, &r), 0, "get(7)");
	int32_t shift_get = (int32_t)(r.time_unix - t_capture);
	int32_t shift_frame = (int32_t)(f28_frame_t0(7) - t_capture);

	printk("F28 (power loss): record time shift %d s (get), %d s (frame t0), synced=%d\n",
	       shift_get, shift_frame, r.time_synced);
	zassert_true(r.time_synced, "re-based at the clock sync");
	zassert_equal(shift_get, 0, "F28 shift (get) %d", shift_get);
	zassert_equal(shift_frame, 0, "F28 shift (frame) %d", shift_frame);

	zassert_equal(app_history_get(6, &r), 0, "get(6)");
	zassert_equal(r.time_unix, F28_T0 + 6 * 60, "pre-outage time %u", r.time_unix);
}

/* A page stamped on uptime that never saw a clock sync before the next reboot:
 * its uptime epoch is gone, so it stays unsynced (time_synced=false frames)
 * instead of the old "newest record = now" guess (#191). A bounded window on a
 * synced device skips it; an open window still returns it. */
ZTEST(history_flash, test_unsynced_page_from_earlier_boot_stays_unsynced)
{
	f28_fill_before_outage();
	reboot();

	test_clock_has = false;
	capture_n(7); /* one durable post-boot page on uptime */
	reboot();     /* power lost again before any DeviceTimeAns */

	test_clock_has = true;
	test_clock_unix = F28_T0 + 2 * F28_OUTAGE;
	app_history_on_clock_sync(test_clock_unix);
	capture_n(1);
	zassert_equal(app_history_count(), 15);

	struct app_history_record r;
	zassert_equal(app_history_get(7, &r), 0);
	zassert_false(r.time_synced, "earlier-boot uptime page must stay unsynced");
	zassert_equal(app_history_get(14, &r), 0);
	zassert_true(r.time_synced);
	zassert_equal(r.time_unix, test_clock_unix, "new page from the RTC");

	uint8_t buf[64];

	/* Open window: all three pages, the unsynced one as its own frame. */
	zassert_equal(app_history_count_frames(0, UINT32_MAX, sizeof(buf)), 3);
	uint32_t t0;
	bool synced = true;
	uint16_t n;
	size_t next;

	(void)app_history_export_page(0, UINT32_MAX, 7, buf, sizeof(buf), &t0, &synced, &n, &next);
	zassert_equal(n, 7);
	zassert_false(synced, "frame of the unsynced page carries time_synced=false");
	zassert_equal(next, 14);

	/* Bounded window: only the synced pages qualify. */
	zassert_equal(app_history_count_frames(F28_T0, test_clock_unix, sizeof(buf)), 2);
	(void)app_history_export_page(F28_T0, test_clock_unix, 7, buf, sizeof(buf), &t0, &synced,
				      &n, &next);
	zassert_equal(n, 1);
	zassert_true(synced);
	zassert_equal(t0, test_clock_unix);
}

/* C: while a replay streams the ring, writes within the current page go on but
 * the page rollover (a ~20 ms erase) is held off: a record that needs the next
 * page is dropped, and the first capture after the replay opens it. */
ZTEST(history_flash, test_replay_holds_off_page_rollover)
{
	size_t rpp = app_history_capacity() / 4; /* 4 pages in app.overlay */

	capture_n(rpp - 2);
	app_history_set_replay_active(true);
	capture_n(5); /* 2 fill the head page, 3 would need the next one */
	zassert_equal(app_history_count(), rpp, "count %zu want %zu", app_history_count(), rpp);
	app_history_set_replay_active(false);

	app_history_capture();
	zassert_equal(app_history_count(), rpp + 1, "rollover after the replay");
}

/* ---- Page header v2: clock-sync fix-up double word ---------------------- */

/* Power loss, then the post-boot page (uptime base) learns the unix time; the
 * (unix - uptime) offset goes into the page's fix-up double word, written from
 * the registered work queue (never from the downlink callback, #96), so the
 * page's times survive the next reboot. */
ZTEST(history_flash, test_fixup_dw_survives_reboot)
{
	f28_fill_before_outage();
	reboot();

	uint32_t t_capture = test_clock_unix + F28_OUTAGE;
	test_clock_has = false;
	capture_n(7); /* durable post-boot page on uptime */

	app_history_set_work_queue(&k_sys_work_q);
	k_sleep(K_SECONDS(30));
	test_clock_has = true;
	test_clock_unix = t_capture + 30;
	app_history_on_clock_sync(test_clock_unix);
	k_sleep(K_MSEC(10)); /* let the fix-up work run */
	app_history_set_work_queue(NULL);

	reboot();
	zassert_equal(app_history_count(), 14);

	struct app_history_record r;
	zassert_equal(app_history_get(7, &r), 0);
	zassert_true(r.time_synced, "fix-up must survive the reboot");
	zassert_equal(r.time_unix, t_capture, "time %u want %u", r.time_unix, t_capture);
	zassert_equal(app_history_get(13, &r), 0);
	zassert_equal(r.time_unix, t_capture + 6 * 60);
	zassert_equal(app_history_get(6, &r), 0);
	zassert_equal(r.time_unix, F28_T0 + 6 * 60, "old page untouched");

	/* A second reboot re-reads the same fix-up. */
	reboot();
	zassert_equal(app_history_get(7, &r), 0);
	zassert_true(r.time_synced);
	zassert_equal(r.time_unix, t_capture);
}

/* No work queue registered: the next capture (report work queue in the app)
 * writes the pending fix-up before appending. */
ZTEST(history_flash, test_fixup_dw_written_by_next_capture)
{
	test_clock_has = false;
	capture_n(7);
	k_sleep(K_SECONDS(5));
	test_clock_has = true;
	test_clock_unix = F28_T0;
	app_history_on_clock_sync(test_clock_unix);

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0);
	zassert_true(r.time_synced);
	uint32_t t_first = r.time_unix;
	zassert_true(t_first <= F28_T0 - 5 && t_first >= F28_T0 - 6 - 7 * 60, "t %u", t_first);

	capture_n(7); /* writes the fix-up, then appends (grid continues in unix) */
	reboot();
	zassert_equal(app_history_count(), 14);
	zassert_equal(app_history_get(0, &r), 0);
	zassert_true(r.time_synced, "fix-up written by the capture");
	zassert_equal(r.time_unix, t_first);
	zassert_equal(app_history_get(13, &r), 0);
	zassert_equal(r.time_unix, t_first + 13 * 60, "same page, same grid");
}

/* v1 pages (32 B header, firmware before the fix-up double word) written by the
 * old firmware stay readable: same stream layout, own base per page. */
struct v1_hdr {
	uint32_t magic;
	uint32_t seq;
	uint32_t mask;
	uint32_t interval;
	uint32_t base_time;
	uint32_t first_ord;
	uint16_t sample_size;
	uint8_t base_synced;
	uint8_t rsv;
	uint16_t crc;
	uint16_t rsv2;
} __packed;

/* Write a v1 page with `n` temp+hum records (temp = t0 + i, hum 50 %). */
static void write_v1_page(uint16_t phys, uint32_t seq, uint32_t first_ord, uint32_t base,
			  bool synced, int n, int t0)
{
	const struct flash_area *fa;
	off_t off = (off_t)phys * 2048;

	zassert_equal(flash_area_open(FIXED_PARTITION_ID(history_partition), &fa), 0);
	zassert_equal(flash_area_erase(fa, off, 2048), 0);

	struct v1_hdr h = {
		.magic = 0x48524e47,
		.seq = seq,
		.mask = BIT(APP_HISTORY_TEMPERATURE) | BIT(APP_HISTORY_HUMIDITY),
		.interval = 60,
		.base_time = base,
		.first_ord = first_ord,
		.sample_size = 3,
		.base_synced = synced ? 1 : 0,
	};
	h.crc = crc16_ccitt(0xffff, (const uint8_t *)&h, offsetof(struct v1_hdr, crc));
	zassert_equal(flash_area_write(fa, off, &h, sizeof(h)), 0);

	uint8_t stream[7 * 3 * 4] = {0};
	for (int i = 0; i < n; i++) {
		sys_put_le16((uint16_t)((t0 + i) * 100), &stream[i * 3]);
		stream[i * 3 + 2] = 100;
	}
	for (int dw = 0; dw < (n * 3) / 7; dw++) {
		uint8_t buf[8];
		memcpy(buf, &stream[dw * 7], 7);
		buf[7] = 0xA5;
		zassert_equal(flash_area_write(fa, off + 32 + dw * 8, buf, 8), 0);
	}
	flash_area_close(fa);
}

ZTEST(history_flash, test_v1_pages_still_readable)
{
	/* Old firmware: a synced page, then an unsynced one (power loss). */
	write_v1_page(0, 5, 0, F28_T0, true, 7, 10);
	write_v1_page(1, 6, 7, 500, false, 7, 20);
	reboot();
	zassert_equal(app_history_count(), 14, "v1 pages mounted");

	struct app_history_record r;
	zassert_equal(app_history_get(0, &r), 0);
	zassert_true(r.time_synced);
	zassert_equal(r.time_unix, F28_T0);
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], 10.0, 0.01);
	zassert_equal(app_history_get(8, &r), 0);
	zassert_false(r.time_synced);
	zassert_equal(r.time_unix, 560);
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], 21.0, 0.01);

	/* A v1 page has no fix-up double word: an old-boot unsynced page stays so. */
	test_clock_has = true;
	test_clock_unix = F28_T0 + 3600;
	app_history_on_clock_sync(test_clock_unix);
	zassert_equal(app_history_get(8, &r), 0);
	zassert_false(r.time_synced);

	/* New pages continue the chain as v2 and everything survives a reboot. */
	capture_n(7);
	reboot();
	zassert_equal(app_history_count(), 21);
	zassert_equal(app_history_get(14, &r), 0);
	zassert_true(r.time_synced);
	zassert_equal(r.time_unix, F28_T0 + 3600);
	zassert_equal(app_history_get(13, &r), 0);
	zassert_within(r.value[APP_HISTORY_TEMPERATURE], 26.0, 0.01, "v1 tail record");
	zassert_equal(app_history_count_frames(0, UINT32_MAX, 64), 3, "one frame per page");
}

/* Header v2 costs one double word (7 data bytes) per page: 1757 B of records,
 * 585 temp+hum records instead of 588. */
ZTEST(history_flash, test_v2_page_capacity)
{
	zassert_equal(app_history_capacity(), 4 * (1757 / 3), "capacity %zu",
		      app_history_capacity());
}
