/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for the report-cadence slot grid (app_slot.c, F27): the
 * periodic report is scheduled on wall-clock slots anchor + k * interval, so a
 * drifting kernel clock or late runs never accumulate, the grid keeps its phase
 * across the first RTC sync (uptime -> unix), and RTC steps / interval changes
 * / early timers are handled without double or skipped slots.
 */

#include "app_slot.h"

#include <zephyr/ztest.h>

#include <stdint.h>

#define I   60u         /* interval_report (s) */
#define UNX 1750000000u /* some unix time */

ZTEST_SUITE(slot, NULL, NULL, NULL, NULL, NULL);

ZTEST(slot, test_round_nearest)
{
	zassert_equal(app_slot_round(100, 60, 100), 100);
	zassert_equal(app_slot_round(100, 60, 129), 100);
	zassert_equal(app_slot_round(100, 60, 130), 160, "ties round up");
	zassert_equal(app_slot_round(100, 60, 159), 160);
	zassert_equal(app_slot_round(100, 60, 71), 100);
	zassert_equal(app_slot_round(100, 60, 70), 100, "tie below the anchor rounds up");
	zassert_equal(app_slot_round(100, 60, 69), 40);
	zassert_equal(app_slot_round(100, 60, 0), (uint32_t)-20, "negative k wraps");
	zassert_equal(app_slot_round(UNX, 900, UNX - 900 * 3 + 1), UNX - 900 * 3);
	zassert_equal(app_slot_round(5, 0, 9), 9, "interval 0 treated as 1");
}

ZTEST(slot, test_boot_arm_fresh_grid)
{
	struct app_slot s = {0};
	uint32_t slot = 0;

	/* Boot: same as the old fixed timer — first report one interval out. */
	uint32_t d = app_slot_next(&s, I, 1000, false, 1000, false, &slot);

	zassert_equal(d, I, "first delay %u", d);
	zassert_equal(slot, 1000);
	zassert_true(s.valid && !s.synced && s.anchor == 1000, "grid laid at now");
}

ZTEST(slot, test_periodic_on_time_early_late)
{
	struct app_slot s = {.anchor = 1000, .interval = I, .synced = false, .valid = true};
	uint32_t slot;

	/* On time. */
	zassert_equal(app_slot_next(&s, I, 1060, false, 1060, true, &slot), I);
	zassert_equal(slot, 1060);

	/* Late 12 s (busy work queue, within the tolerance): same slot, next one
	 * pulled in. */
	zassert_equal(app_slot_next(&s, I, 1132, false, 1132, true, &slot), 48);
	zassert_equal(slot, 1120);

	/* Early 5 s (fast kernel clock): still slot 1180, not a second 1120. */
	zassert_equal(app_slot_next(&s, I, 1175, false, 1175, true, &slot), 65);
	zassert_equal(slot, 1180);

	/* due == now at 1 s resolution never yields a zero delay. */
	zassert_equal(app_slot_next(&s, I, 1240, false, 1240, true, &slot), I);
	zassert_equal(slot, 1240);
}

/* HIL T4 (0413, 2026-09-25): a 150 s debug halt froze the kernel with ~27 s of
 * the report timer left, so the run fired 30 s off the grid. Borrowing the
 * nearest slot stamped that record 30 s away from its sampling time (up to
 * 450 s at a 900 s interval). A run beyond the tolerance re-anchors instead. */
ZTEST(slot, test_run_far_off_grid_reanchors)
{
	struct app_slot s = {.anchor = UNX, .interval = I, .synced = true, .valid = true};
	uint32_t slot;
	uint32_t now = UNX + 3 * I + 30; /* exactly between two slots */

	zassert_equal(app_slot_next(&s, I, now, true, 0, true, &slot), I);
	zassert_equal(slot, now, "record must keep its sampling time");
	zassert_equal(s.anchor, now, "grid re-laid at the late run");

	/* Next run on the new grid is on time again. */
	zassert_equal(app_slot_next(&s, I, now + I, true, 0, true, &slot), I);
	zassert_equal(slot, now + I);

	/* Just inside the tolerance keeps the old slot; just outside re-anchors. */
	s = (struct app_slot){.anchor = UNX, .interval = I, .synced = true, .valid = true};
	(void)app_slot_next(&s, I, UNX + I + APP_SLOT_TOLERANCE_S, true, 0, true, &slot);
	zassert_equal(slot, UNX + I);
	s = (struct app_slot){.anchor = UNX, .interval = I, .synced = true, .valid = true};
	(void)app_slot_next(&s, I, UNX + I + APP_SLOT_TOLERANCE_S + 1, true, 0, true, &slot);
	zassert_equal(slot, UNX + I + APP_SLOT_TOLERANCE_S + 1);

	/* A long interval uses the absolute tolerance, not interval / 2. */
	s = (struct app_slot){.anchor = UNX, .interval = 900, .synced = true, .valid = true};
	(void)app_slot_next(&s, 900, UNX + 900 + 200, true, 0, true, &slot);
	zassert_equal(slot, UNX + 1100, "200 s late at 900 s must not borrow a slot");
}

/* Debug SysTick on the free-running MSI ran 1.22 % slow (F27). With a fixed
 * re-arm the lag grows ~44 s/h; on the grid it stays bounded by one delay's
 * worth of drift and every run lands on the next consecutive slot. */
ZTEST(slot, test_slow_kernel_clock_does_not_accumulate)
{
	struct app_slot s = {0};
	uint32_t slot;
	/* RTC time in ms, the kernel delay stretched by 1.22 %. */
	uint64_t rtc_ms = (uint64_t)UNX * 1000;
	uint32_t d = app_slot_next(&s, I, UNX, true, 0, false, &slot);
	uint32_t expect = UNX;

	for (int k = 0; k < 360; k++) { /* 6 h */
		rtc_ms += (uint64_t)d * 1000 * 10122 / 10000;
		uint32_t now = (uint32_t)(rtc_ms / 1000);

		d = app_slot_next(&s, I, now, true, 0, true, &slot);
		expect += I;
		zassert_equal(slot, expect, "run %d: slot %u want %u", k, slot, expect);
		zassert_true(now - slot <= 1, "run %d late by %u s", k, now - slot);
	}
	zassert_true(rtc_ms / 1000 - UNX <= 360u * I + 1, "cadence lost time");
}

/* First RTC sync: the anchor moves by the (unix - uptime) offset, so the next
 * run is still one interval after the previous one. */
ZTEST(slot, test_uptime_to_unix_keeps_phase)
{
	struct app_slot s = {0};
	uint32_t slot;

	(void)app_slot_next(&s, I, 100, false, 100, false, &slot);
	zassert_equal(app_slot_next(&s, I, 160, false, 160, true, &slot), I);
	zassert_equal(slot, 160);

	/* DeviceTimeAns at uptime 170 -> unix UNX (offset UNX - 170). The run at
	 * uptime 220 now reads unix. */
	uint32_t off = UNX - 170;
	uint32_t d = app_slot_next(&s, I, 220 + off, true, 220, true, &slot);

	zassert_equal(slot, 220 + off, "slot %u want %u", slot, 220 + off);
	zassert_equal(d, I, "delay %u", d);
	zassert_true(s.synced && s.anchor == 100 + off, "anchor carried over");
}

ZTEST(slot, test_rtc_steps)
{
	struct app_slot s = {.anchor = UNX, .interval = I, .synced = true, .valid = true};
	uint32_t slot;

	/* Weekly resync corrects the RTC by +5 s: same slot, shorter delay. */
	zassert_equal(app_slot_next(&s, I, UNX + I + 5, true, 0, true, &slot), I - 5);
	zassert_equal(slot, UNX + I);

	/* Clock set forward by 1 h 10 s (within the tolerance): grid phase kept. */
	uint32_t now = UNX + 3600 + 10;

	zassert_equal(app_slot_next(&s, I, now, true, 0, true, &slot), I - 10);
	zassert_equal(slot, UNX + 3600);

	/* Clock set forward by 1 h 17 s (beyond it): the grid is re-laid at the
	 * run, so the record keeps its true time. */
	now = UNX + 7200 + 17;
	zassert_equal(app_slot_next(&s, I, now, true, 0, true, &slot), I);
	zassert_equal(slot, now);

	/* Clock set back to a slot before the anchor (negative distance): still
	 * on the grid. */
	now = s.anchor - 1000 * I;
	uint32_t d = app_slot_next(&s, I, now, true, 0, true, &slot);

	zassert_equal(slot, now);
	zassert_equal(d, I, "delay %u", d);
}

ZTEST(slot, test_interval_change_lays_fresh_grid)
{
	struct app_slot s = {.anchor = UNX, .interval = I, .synced = true, .valid = true};
	uint32_t slot;

	uint32_t d = app_slot_next(&s, 900, UNX + 17, true, 0, true, &slot);

	zassert_equal(d, 900);
	zassert_equal(slot, UNX + 17);
	zassert_equal(s.anchor, UNX + 17);
	zassert_equal(s.interval, 900);
}

ZTEST(slot, test_rtc_lost_lays_fresh_grid)
{
	struct app_slot s = {.anchor = UNX, .interval = I, .synced = true, .valid = true};
	uint32_t slot;

	uint32_t d = app_slot_next(&s, I, 5000, false, 5000, true, &slot);

	zassert_equal(d, I);
	zassert_equal(slot, 5000);
	zassert_false(s.synced);
}

/* Arming outside the cadence with a valid grid: first slot strictly after now,
 * even when now sits exactly on a slot. */
ZTEST(slot, test_arm_strictly_after_now)
{
	struct app_slot s = {.anchor = 1000, .interval = I, .synced = false, .valid = true};
	uint32_t slot;

	zassert_equal(app_slot_next(&s, I, 1120, false, 1120, false, &slot), I);
	zassert_equal(app_slot_next(&s, I, 1130, false, 1130, false, &slot), 50);
	zassert_equal(app_slot_next(&s, I, 1150, false, 1150, false, &slot), 30);
}
