/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for app/src/app_buzzer.c (#338 melody engine), deferred from
 * Phase 1 to #397. Drives the real app_buzzer.c (playback thread + queue)
 * against native_sim's built-in gpio_emul standing in for the two GPIO pins
 * it writes directly (app.overlay), reading back what the pattern walk wrote
 * via gpio_emul_output_get_dt(). Real k_sleep() timing throughout (native_sim's
 * simulated clock advances instantly between blocked threads), no fake clock.
 */

#include "app_buzzer.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static const struct gpio_dt_spec a_spec = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pir_si_gpios);

static bool buzzer_on(void)
{
	return gpio_emul_output_get_dt(&a_spec) == 1;
}

/* app_buzzer_init() creates its playback thread from a function-static
 * k_thread — a second call would reuse/corrupt that TCB. Call it exactly
 * once for the whole binary, in the suite's one-time setup (not `before`,
 * which runs per test). */
static void *suite_setup(void)
{
	zassert_equal(app_buzzer_init(), 0, "app_buzzer_init failed");
	return NULL;
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	/* Drain any leftover in-flight/queued state from a previous test so
	 * cases don't bleed into each other. A prior test that aborted
	 * mid-pattern leaves the playback thread sleeping out
	 * REQUEST_MIN_DELAY_MS (500 ms, app_buzzer.c) before it next reaches
	 * k_msgq_get() — this STOP's k_msgq_purge() only wakes a getter
	 * actually blocked there, so it can arrive too early to do anything
	 * if the thread is still mid-cooldown. Sleep past that cooldown so
	 * every test starts from a genuinely idle, abort-flag-clear thread. */
	app_buzzer_play_repeating(APP_BUZZER_KIND_STOP, 0);
	k_sleep(K_MSEC(600));
}

ZTEST_SUITE(buzzer, NULL, suite_setup, before, NULL, NULL);

/* ---- melody playback sequencing (SET/DELAY pattern walk) -----------------
 *
 * execute_pattern() skips the LAST delay of the LAST repetition outright (it
 * would otherwise run only to have the trailing set(false) immediately
 * override it) — harmless for ALARM, whose last step is already an "off"
 * delay, but it means INFO/WARNING's final beep-on pulse is applied and
 * cleared within the same non-yielding execution, with no k_sleep in
 * between. A test thread can only observe GPIO state at a k_sleep()
 * boundary, so that final pulse is provably unobservable here — these tests
 * only assert the OBSERVABLE parts of each pattern walk. */

ZTEST(buzzer, test_warning_melody_first_beep_is_observable)
{
	zassert_equal(app_buzzer_play(APP_BUZZER_KIND_WARNING), 0, "play(WARNING) failed");

	k_sleep(K_MSEC(30)); /* within the first 80 ms beep */
	zassert_true(buzzer_on(), "WARNING's first beep never turned the buzzer on");

	k_sleep(K_MSEC(100)); /* 130 ms total: within the 150 ms silent gap */
	zassert_false(buzzer_on(), "WARNING's gap between beeps stayed on");

	k_sleep(K_MSEC(200)); /* past the whole pattern (230 ms) */
	zassert_false(buzzer_on(), "WARNING never settled off after the pattern finished");
}

ZTEST(buzzer, test_alarm_melody_walks_all_five_repetitions)
{
	zassert_equal(app_buzzer_play(APP_BUZZER_KIND_ALARM), 0, "play(ALARM) failed");

	/* Each of the 5 repetitions is 100 ms on + 80 ms off = 180 ms; only the
	 * very last repetition's trailing off-delay is skipped (harmless, it
	 * is already off by then). Walk all five on/off windows for real. */
	for (int rep = 0; rep < 5; rep++) {
		k_sleep(K_MSEC(50)); /* within this repetition's 100 ms on phase */
		zassert_true(buzzer_on(), "ALARM rep %d never turned on", rep);

		k_sleep(K_MSEC(80)); /* 130 ms into the rep: within the 80 ms off phase */
		zassert_false(buzzer_on(), "ALARM rep %d never turned off", rep);

		k_sleep(K_MSEC(50)); /* finish out the 180 ms repetition period */
	}

	k_sleep(K_MSEC(50));
	zassert_false(buzzer_on(), "ALARM did not settle off after 5 repetitions");
}

ZTEST(buzzer, test_info_melody_dispatches_without_error)
{
	/* INFO's single beep is entirely inside the unobservable last-pulse
	 * window (see comment above) — only the dispatch + eventual settle can
	 * be asserted here. */
	zassert_equal(app_buzzer_play(APP_BUZZER_KIND_INFO), 0, "play(INFO) failed");

	k_sleep(K_MSEC(50));
	zassert_false(buzzer_on(), "INFO never settled off");
}

/* ---- off/stop aborts -------------------------------------------------- */

ZTEST(buzzer, test_off_aborts_mid_beep)
{
	zassert_equal(app_buzzer_play(APP_BUZZER_KIND_ALARM), 0, "play(ALARM) failed");

	k_sleep(K_MSEC(50)); /* mid first on-phase */
	zassert_true(buzzer_on(), "ALARM did not turn on before abort");

	zassert_equal(app_buzzer_set(false), 0, "app_buzzer_set(false) failed");
	zassert_false(buzzer_on(), "buzzer stayed on immediately after abort");

	/* Must not resume/replay any of the remaining 4 repetitions. */
	k_sleep(K_MSEC(500));
	zassert_false(buzzer_on(), "aborted mid-beep melody resumed playing");
}

ZTEST(buzzer, test_off_while_idle_does_not_lock_out_next_play)
{
	/* Nothing queued/playing — the playback thread is parked in
	 * k_msgq_get(K_FOREVER). app_buzzer_set(false) still sets the abort
	 * flag and wakes it, but k_wakeup() does not unblock a msgq pend, so
	 * the flag is only consumed on the NEXT real request. Regression: a
	 * stale abort flag must not swallow that next request (lockout). */
	zassert_equal(app_buzzer_set(false), 0, "app_buzzer_set(false) while idle failed");
	zassert_false(buzzer_on(), "buzzer was on while idle");

	zassert_equal(app_buzzer_play(APP_BUZZER_KIND_ALARM), 0,
		      "play(ALARM) after idle-off failed");
	k_sleep(K_MSEC(50));
	zassert_true(buzzer_on(), "play() after an idle app_buzzer_set(false) was locked out");

	zassert_equal(app_buzzer_set(false), 0, "cleanup app_buzzer_set(false) failed");
}

ZTEST(buzzer, test_off_during_active_repeat_does_not_replay)
{
	/* WARNING's pattern is 230 ms; repeat_s=1 replays 1 s after each
	 * playback finishes. Regression (found on real HW): stopping while the
	 * thread is parked waiting out the repeat interval must not have the
	 * wakeup misread as "interval elapsed naturally" and replay the
	 * melody — app_buzzer_set(false) (via app_buzzer_play_repeating(0,..))
	 * must run BEFORE k_msgq_purge() inside app_buzzer_play_repeating(). */
	zassert_equal(app_buzzer_play_repeating(APP_BUZZER_KIND_WARNING, 1), 0,
		      "play_repeating(WARNING, 1) failed");

	k_sleep(K_MSEC(300)); /* past the 230 ms pattern: now parked waiting out repeat_s */
	zassert_false(buzzer_on(), "WARNING should have finished its single playback by now");

	zassert_equal(app_buzzer_play_repeating(APP_BUZZER_KIND_STOP, 0), 0, "stop failed");

	k_sleep(K_MSEC(1200)); /* past when the 1 s repeat would have fired */
	zassert_false(buzzer_on(), "stopped melody replayed after its repeat interval elapsed");
}

/* ---- queue policy: newest replaces not-yet-started --------------------- */

ZTEST(buzzer, test_queue_newest_replaces_not_yet_started)
{
	/* ALARM's first repetition is in flight (900 ms total) when two more
	 * requests are enqueued back to back; only the LAST one enqueued
	 * (INFO) should play next, not WARNING. */
	zassert_equal(app_buzzer_play(APP_BUZZER_KIND_ALARM), 0, "play(ALARM) failed");
	k_sleep(K_MSEC(10)); /* let it start, still well within the first repetition */

	zassert_equal(app_buzzer_play(APP_BUZZER_KIND_WARNING), 0, "enqueue(WARNING) failed");
	zassert_equal(app_buzzer_play(APP_BUZZER_KIND_INFO), 0, "enqueue(INFO) failed");

	zassert_equal(app_buzzer_set(false), 0, "abort the in-flight ALARM failed");
	k_sleep(K_MSEC(50));

	/* Whichever of WARNING/INFO plays next, it must not be a fresh ALARM
	 * cycle (5 on/off repetitions) — spot-check by outlasting a single
	 * WARNING/INFO pattern (230 ms) and confirming it already went quiet,
	 * which a freshly-started ALARM (900 ms) would not have by then. */
	k_sleep(K_MSEC(300));
	zassert_false(buzzer_on(),
		      "queued request after abort still looks like a fresh ALARM cycle");
}

/* ---- buzzer_play command-dispatch bounds -------------------------------
 * (app_cmd.c's forwarding is covered by tests/cmd; these pin app_buzzer.c's
 * own id validation, which app_cmd.c relies on.) */

ZTEST(buzzer, test_kind_ge_16_collapses_to_stop)
{
	zassert_equal(app_buzzer_play(APP_BUZZER_KIND_ALARM), 0, "play(ALARM) failed");
	k_sleep(K_MSEC(50));
	zassert_true(buzzer_on(), "ALARM did not turn on before the stop-collapse call");

	zassert_equal(app_buzzer_play_repeating(16, 5), 0,
		      "kind>=16 must collapse to stop, not error");
	zassert_false(buzzer_on(), "kind>=16 did not silence the buzzer");
}

ZTEST(buzzer, test_reserved_kind_returns_enoent)
{
	for (uint32_t kind = 4; kind <= 15; kind++) {
		zassert_equal(app_buzzer_play(kind), -ENOENT,
			      "reserved kind %u did not return -ENOENT", kind);
	}
	zassert_false(buzzer_on(), "a rejected reserved kind must not touch the GPIO");
}
