/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The common uplink path of app_radio.c (doc/plan/460 F4) against a fake
 * backend. The scenarios that both radios share run twice: once with a
 * LoRaWAN-like backend (nothing sent confirmed, 3 s between the frames of a
 * report, 51 B budget) and once with a P2P-like one (answers and history
 * confirmed, no frame gap, 239 B budget) -- one implementation, the same
 * behaviour on either radio (decision #23); so does the retry ladder of a
 * confirmed frame without its Ack (T2c), alarms confirmed by radio-alarm-ack. Link supervision (F2)
 * runs the same way: the fake reports link-check outcomes and records the rungs and rejoins
 * app_radio asks for; so does the history replay (F3c), over a stub store of fixed-size records.
 * The duty ledger (T2d) is tested pure, and through the fake's air per frame.
 */

#include "stubs.h"

#include "app_cmd.h"
#include "app_config.h"
#include "app_radio.h"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

#define RETRY_MS 15000 /* app_radio.c TX_RETRY_MS */
#define LOG_MAX  64

struct sent {
	uint8_t kind;
	uint8_t tag;
	uint8_t port;
	uint8_t flags;
	uint16_t len;
	uint8_t attempt;
	uint8_t head[8];
	int ret;
	int64_t at_ms;
};

/* Fake backend state. The work queue writes it, the test thread reads it
 * after a sleep. */
static struct {
	int script[LOG_MAX]; /* send() result per attempt; 0 past the end */
	size_t n_script;
	uint32_t wait_ms;   /* res->wait_ms of every send */
	uint8_t res_budget; /* res->budget of every send */
	uint8_t budget;
	bool ready;
	uint8_t report_flags; /* flags of every report, on top of the due flag */
	bool suppress_due;    /* LoRaWAN: a LinkCheckReq is already pending */
	int report_flags_calls;
	/* Link supervision: */
	enum app_radio_state state;
	int steps_left; /* warning_step() rungs left */
	int step_calls;
	int rejoin_ret;
	int rejoin_calls;
	bool rejoin_forced;
	/* After this many send() calls (0 = never): */
	size_t drop_link_after;
	size_t zero_budget_after;
	size_t request_after;
	size_t queue_after;
	size_t budget_after; /* ... the budget becomes budget_to */
	uint8_t budget_to;
	size_t replay_after;  /* ... a ReqHistory (seq 77) arrives mid-send */
	int replay_ret;       /* what that nested start returned */
	uint8_t flush_budget; /* an empty frame (MAC flush) sets the budget to it */
	int time_requests;    /* time_request() calls (a clock_sync asked for a time) */
	uint32_t air_ms;      /* airtime_ms() of every frame, charged when it goes */
	struct sent log[LOG_MAX];
	size_t n;
} fk;

static int fake_send(const struct app_radio_frame *f, struct app_radio_tx_result *res)
{
	int ret = fk.n < fk.n_script ? fk.script[fk.n] : 0;

	/* The send contract: no Ack is missing where none was asked for. */
	if (ret == -ETIMEDOUT && !(f->flags & APP_RADIO_FRAME_CONFIRMED)) {
		ret = 0;
	}

	if (fk.n < LOG_MAX) {
		struct sent *s = &fk.log[fk.n];

		s->kind = f->kind;
		s->tag = f->tag;
		s->port = f->port;
		s->flags = f->flags;
		s->len = f->len;
		s->attempt = f->attempt;
		memcpy(s->head, f->buf, MIN(f->len, sizeof(s->head)));
		s->ret = ret;
		s->at_ms = k_uptime_get();
	}
	fk.n++;
	res->wait_ms = fk.wait_ms;
	res->budget = fk.res_budget;
	if (ret == 0 || ret == -ETIMEDOUT) {
		app_radio_duty_charge(fk.air_ms); /* as the backends do, at the physical TX */
	}

	if (fk.n == fk.drop_link_after) {
		fk.ready = false;
	}
	if (fk.n == fk.zero_budget_after) {
		fk.budget = 0;
	}
	if (fk.n == fk.request_after) {
		app_radio_send_telemetry_now();
	}
	if (fk.n == fk.queue_after) {
		const uint8_t b[] = {0xb9, 0x00};

		(void)app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 0, b,
					 sizeof(b));
	}
	if (fk.n == fk.budget_after) {
		fk.budget = fk.budget_to;
	}
	if (fk.n == fk.replay_after) {
		fk.replay_ret = app_radio_history_replay_start(900, 1000, 77);
	}
	if (f->len == 0 && fk.flush_budget) {
		fk.budget = fk.flush_budget;
	}
	return ret;
}

static uint8_t fake_budget(void)
{
	return fk.budget;
}

static bool fake_tx_ready(void)
{
	return fk.ready;
}

struct profile {
	const struct app_radio_backend *be;
	uint8_t budget;
	uint8_t queued_flags; /* flags of a queued answer and a history frame */
	uint8_t due_flag;     /* how a link check rides a report */
	int transport;        /* enum app_cmd_transport of its downlinks */
};

static const struct profile *m_prof;

static uint8_t fake_report_flags(bool due)
{
	fk.report_flags_calls++;
	return fk.report_flags | ((due && !fk.suppress_due) ? m_prof->due_flag : 0);
}

static enum app_radio_state fake_get_state(void)
{
	return fk.state;
}

static bool fake_warning_step(void)
{
	fk.step_calls++;
	if (fk.steps_left > 0) {
		fk.steps_left--;
		return true;
	}
	return false;
}

static int fake_rejoin(bool forced)
{
	fk.rejoin_calls++;
	fk.rejoin_forced = forced;
	return fk.rejoin_ret;
}

static void fake_time_request(void)
{
	fk.time_requests++;
}

static uint32_t fake_airtime_ms(size_t len)
{
	ARG_UNUSED(len);
	return fk.air_ms;
}

static const struct app_radio_backend be_lrw = {
	.send = fake_send,
	.budget = fake_budget,
	.tx_ready = fake_tx_ready,
	.report_flags = fake_report_flags,
	.get_state = fake_get_state,
	.warning_step = fake_warning_step,
	.rejoin = fake_rejoin,
	.time_request = fake_time_request,
	.airtime_ms = fake_airtime_ms,
	.confirm_kinds = 0,
	.frame_gap_ms = 3000,
	.cmd_transport = APP_CMD_TRANSPORT_LRW,
};

static const struct app_radio_backend be_p2p = {
	.send = fake_send,
	.budget = fake_budget,
	.tx_ready = fake_tx_ready,
	.report_flags = fake_report_flags,
	.get_state = fake_get_state,
	.warning_step = fake_warning_step,
	.rejoin = fake_rejoin,
	.time_request = fake_time_request,
	.airtime_ms = fake_airtime_ms,
	.confirm_kinds = BIT(APP_RADIO_FRAME_ANSWER) | BIT(APP_RADIO_FRAME_HISTORY),
	.frame_gap_ms = 0,
	.cmd_transport = APP_CMD_TRANSPORT_P2P,
};

static const struct profile PROFILE_LRW = {&be_lrw, 51, 0, APP_RADIO_FRAME_LINK_CHECK,
					   APP_CMD_TRANSPORT_LRW};
static const struct profile PROFILE_P2P = {&be_p2p, 239, APP_RADIO_FRAME_CONFIRMED,
					   APP_RADIO_FRAME_CONFIRMED | APP_RADIO_FRAME_LINK_CHECK,
					   APP_CMD_TRANSPORT_P2P};

static void use_profile(const struct profile *p)
{
	m_prof = p;
	fk.budget = p->budget;
	app_radio_test_set_backend(p->be);
}

#define BOTH_PROFILES(name)                                                                        \
	ZTEST(radio_common, test_##name##_lrw)                                                     \
	{                                                                                          \
		use_profile(&PROFILE_LRW);                                                         \
		name();                                                                            \
	}                                                                                          \
	ZTEST(radio_common, test_##name##_p2p)                                                     \
	{                                                                                          \
		use_profile(&PROFILE_P2P);                                                         \
		name();                                                                            \
	}

static void script(const int *r, size_t n)
{
	zassert_true(n <= LOG_MAX);
	memcpy(fk.script, r, n * sizeof(*r));
	fk.n_script = n;
}

static void script_fill(int r, size_t n)
{
	zassert_true(n <= LOG_MAX);
	for (size_t i = 0; i < n; i++) {
		fk.script[i] = r;
	}
	fk.n_script = n;
}

/* Queue a frame of `len` bytes whose first byte is `id`. */
static void queue(enum app_radio_frame_kind kind, enum app_radio_frame_tag tag, size_t len,
		  uint8_t id)
{
	uint8_t buf[APP_RADIO_TX_SLOT_SIZE];

	memset(buf, 0x55, sizeof(buf));
	buf[0] = id;
	zassert_ok(app_radio_tx_queue(kind, tag, 0, buf, len));
}

static void frames(const size_t *lens, size_t n)
{
	memcpy(g_compose_frames, lens, n * sizeof(*lens));
	g_compose_n_frames = n;
}

static uint32_t retries(void)
{
	struct app_radio_status st;

	app_radio_get_status(&st);
	return st.cnt[APP_RADIO_CNT_RETRY];
}

/* Reports completed since the last link-up (app_radio counts them for the
 * link-check cadence after a report's last frame). */
static uint32_t reports_done(void)
{
	struct app_radio_link l;

	app_radio_get_link(&l);
	return l.reports;
}

static void assert_spacing(size_t from, size_t to, int64_t min_ms)
{
	for (size_t i = from + 1; i <= to; i++) {
		zassert_true(fk.log[i].at_ms - fk.log[i - 1].at_ms >= min_ms,
			     "attempt %zu came %lld ms after the one before, want >= %lld", i,
			     (long long)(fk.log[i].at_ms - fk.log[i - 1].at_ms), (long long)min_ms);
	}
}

static int m_ready_calls;

static void count_ready(void)
{
	m_ready_calls++;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	app_radio_test_tx_reset();
	app_radio_test_link_reset();
	app_radio_test_cmd_reset();
	app_radio_test_air_reset();
	app_radio_test_clock_sync_reset();
	stubs_reset();
	memset(&fk, 0, sizeof(fk));
	fk.ready = true;
	fk.state = APP_RADIO_STATE_HEALTHY;
	memset(&g_app_config, 0, sizeof(g_app_config));
	g_app_config.interval_report = 60;
	use_profile(&PROFILE_LRW);
	m_ready_calls = 0;
	app_radio_register_ready_cb(count_ready);
}

static void after(void *f)
{
	ARG_UNUSED(f);
	fk.ready = false;
	k_sleep(K_MSEC(10)); /* a report request still on the system work queue */
	app_radio_test_cmd_reset();
	app_radio_test_tx_reset();
	app_radio_test_link_reset();
	app_radio_test_air_reset();
	app_radio_test_clock_sync_reset();
}

ZTEST_SUITE(radio_common, NULL, NULL, before, after, NULL);

/* ---- Order ---------------------------------------------------------------- */

/* After a link-up: answers, then alarms, then the report -- whatever order
 * they were requested in (the Info and settings-info lead the held alarms and
 * the first telemetry, on either radio). */
static void order_answer_alarm_telemetry(void)
{
	const size_t lens[] = {20};

	frames(lens, 1);
	fk.ready = false;
	app_radio_send_telemetry_now();
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 10, 0xa1);
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 12, 0xb1);
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 0, "sent while the link is down");

	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.n, 3);
	zassert_equal(fk.log[0].kind, APP_RADIO_FRAME_ANSWER);
	zassert_equal(fk.log[0].head[0], 0xb1);
	zassert_equal(fk.log[0].len, 12);
	zassert_equal(fk.log[0].flags, m_prof->queued_flags);
	zassert_equal(fk.log[1].kind, APP_RADIO_FRAME_ALARM);
	zassert_equal(fk.log[1].head[0], 0xa1);
	zassert_equal(fk.log[1].flags, 0, "alarms unconfirmed unless radio-alarm-ack");
	zassert_equal(fk.log[2].kind, APP_RADIO_FRAME_TELEMETRY);
	zassert_equal(fk.log[2].len, 20);
	zassert_equal(fk.log[2].flags, 0);
	zassert_equal(g_compose_last_budget, m_prof->budget);
	zassert_equal(fk.report_flags_calls, 1);
	zassert_equal(reports_done(), 1);
}
BOTH_PROFILES(order_answer_alarm_telemetry)

/* An answer queued while a report is on the air goes between its frames; the
 * rest of the report keeps its flags and never asks for a second link check. */
static void answer_goes_between_report_frames(void)
{
	const size_t lens[] = {20, 20};

	frames(lens, 2);
	fk.report_flags = APP_RADIO_FRAME_LINK_CHECK;
	fk.queue_after = 1;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(20));
	zassert_equal(fk.n, 3);
	zassert_equal(fk.log[0].kind, APP_RADIO_FRAME_TELEMETRY);
	zassert_equal(fk.log[0].flags, APP_RADIO_FRAME_LINK_CHECK | APP_RADIO_FRAME_MORE);
	zassert_equal(fk.log[1].kind, APP_RADIO_FRAME_ANSWER);
	zassert_equal(fk.log[1].head[0], 0xb9);
	zassert_equal(fk.log[2].kind, APP_RADIO_FRAME_TELEMETRY);
	zassert_equal(fk.log[2].head[1], 1);
	zassert_equal(fk.log[2].flags, 0);
	zassert_equal(fk.report_flags_calls, 1);
	zassert_equal(reports_done(), 1);
}
BOTH_PROFILES(answer_goes_between_report_frames)

/* Each queue drains in its own order. */
ZTEST(radio_common, test_queues_are_fifo)
{
	fk.ready = false;
	for (uint8_t i = 0; i < APP_RADIO_TX_QUEUE_DEPTH; i++) {
		queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa0 + i);
		queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb0 + i);
	}
	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.n, 2 * APP_RADIO_TX_QUEUE_DEPTH);
	for (uint8_t i = 0; i < APP_RADIO_TX_QUEUE_DEPTH; i++) {
		zassert_equal(fk.log[i].head[0], 0xb0 + i);
		zassert_equal(fk.log[APP_RADIO_TX_QUEUE_DEPTH + i].head[0], 0xa0 + i);
	}
}

/* ---- Link and in-flight waits --------------------------------------------- */

/* Frames wait while the link is down; nothing polls -- the backend's link-up
 * kick sends them. */
static void link_down_waits_for_the_kick(void)
{
	fk.ready = false;
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	k_sleep(K_SECONDS(1));
	fk.ready = true;
	k_sleep(K_SECONDS(60));
	zassert_equal(fk.n, 0);
	app_radio_tx_kick();
	k_sleep(K_MSEC(100));
	zassert_equal(fk.n, 1);
	zassert_equal(fk.log[0].head[0], 0xb1);
}
BOTH_PROFILES(link_down_waits_for_the_kick)

static void park_until_kick(int err)
{
	const int r[] = {err};
	uint32_t base = retries();

	script(r, 1);
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa1);
	k_sleep(K_SECONDS(60));
	zassert_equal(fk.n, 1, "%d is retried without a kick", err);
	app_radio_tx_kick();
	k_sleep(K_MSEC(100));
	zassert_equal(fk.n, 2);
	zassert_equal(fk.log[1].head[0], 0xa1);
	zassert_ok(fk.log[1].ret);
	zassert_equal(retries(), base, "a wait is not a retry");
}

/* -EBUSY (the MAC busy) and -ENOTCONN (no session) keep the frame until the
 * backend kicks. */
static void busy_and_notconn_wait_for_the_kick(void)
{
	park_until_kick(-EBUSY);
	fk.n = 0;
	park_until_kick(-ENOTCONN);
}
BOTH_PROFILES(busy_and_notconn_wait_for_the_kick)

/* A kick does not cut short a wait already scheduled (a retry here). */
ZTEST(radio_common, test_kick_keeps_a_pending_retry_time)
{
	const int r[] = {-EIO};

	script(r, 1);
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 1);
	app_radio_tx_kick();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 1, "the kick resent before the retry time");
	k_sleep(K_SECONDS(14));
	zassert_equal(fk.n, 2);
}

/* ---- Queued frame results ------------------------------------------------- */

/* A queued frame held by the duty cycle waits as long as the hold lasts:
 * resent after res->wait_ms, never counted, never dropped. */
static void queued_duty_hold_is_not_a_failure(void)
{
	uint32_t base = retries();

	script_fill(-EAGAIN, 12);
	fk.wait_ms = 1000;
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 13);
	zassert_ok(fk.log[12].ret);
	zassert_equal(fk.log[12].head[0], 0xb1);
	assert_spacing(0, 12, 1000);
	zassert_equal(retries(), base);
}
BOTH_PROFILES(queued_duty_hold_is_not_a_failure)

/* No wait given: the default 15 s. */
ZTEST(radio_common, test_queued_duty_hold_default_wait)
{
	const int r[] = {-EAGAIN};

	script(r, 1);
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	k_sleep(K_SECONDS(14));
	zassert_equal(fk.n, 1);
	k_sleep(K_SECONDS(2));
	zassert_equal(fk.n, 2);
}

/* A refused frame is retried every 15 s, 8 times, then dropped; the next
 * frame goes. */
static void queued_error_retried_then_dropped(void)
{
	uint32_t base = retries();

	script_fill(-EIO, 9);
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa1);
	k_sleep(K_SECONDS(150));
	zassert_equal(fk.n, 10);
	for (size_t i = 0; i < 9; i++) {
		zassert_equal(fk.log[i].head[0], 0xb1, "attempt %zu", i);
	}
	assert_spacing(0, 8, RETRY_MS);
	zassert_equal(fk.log[9].head[0], 0xa1);
	zassert_ok(fk.log[9].ret);
	zassert_equal(retries(), base + 8);
}
BOTH_PROFILES(queued_error_retried_then_dropped)

/* While an answer is being retried it is off its queue but still pending (the
 * post-command drain waits for it). */
ZTEST(radio_common, test_answer_pending_while_retried)
{
	const int r[] = {-EIO};

	script(r, 1);
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	k_sleep(K_SECONDS(1));
	zassert_equal(app_radio_tx_answer_free(), APP_RADIO_TX_QUEUE_DEPTH);
	zassert_true(app_radio_tx_answer_pending());
	k_sleep(K_SECONDS(15));
	zassert_equal(fk.n, 2);
	zassert_false(app_radio_tx_answer_pending());
}

/* ---- Confirmed frames (doc/plan/460 §2.6, T2c) --------------------------- */

/* The spread before retry n is a random [1, 2^n) s -- LoRaWAN's ACK_TIMEOUT
 * window, doubled per retry so that nodes which lost the same Ack part. */
ZTEST(radio_common, test_ack_backoff_spreads_within_its_window)
{
	for (uint32_t n = 1; n <= APP_RADIO_ACK_MAX_RETRIES; n++) {
		uint32_t max_ms = APP_RADIO_ACK_BACKOFF_MIN_MS << n;

		zassert_equal(app_radio_ack_backoff_ms(n, 0), APP_RADIO_ACK_BACKOFF_MIN_MS);
		zassert_equal(
			app_radio_ack_backoff_ms(n, max_ms - APP_RADIO_ACK_BACKOFF_MIN_MS - 1),
			max_ms - 1);
		zassert_true(app_radio_ack_backoff_ms(n, UINT32_MAX) < max_ms, "retry %u", n);
	}
	zassert_equal(app_radio_ack_backoff_ms(0, 777), app_radio_ack_backoff_ms(1, 777));
	zassert_equal(app_radio_ack_backoff_ms(9, 12345),
		      app_radio_ack_backoff_ms(APP_RADIO_ACK_MAX_RETRIES, 12345), "capped");
}

/* A confirmed frame without its Ack goes again, confirmed and byte for byte,
 * after the spread; the backend learns the attempt (P2P resends its counter).
 * Every other frame waits: the report asked for meanwhile goes after the Ack. */
static void confirmed_alarm_retried_until_acked(void)
{
	const int r[] = {-ETIMEDOUT, -ETIMEDOUT, 0};
	const size_t lens[] = {20};
	uint32_t base = retries();
	struct app_radio_link l;

	g_app_config.radio_alarm_ack = true;
	frames(lens, 1);
	script(r, ARRAY_SIZE(r));
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa1);
	k_sleep(K_MSEC(100));
	zassert_equal(fk.n, 1);
	zassert_true(app_radio_ack_pending());
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(8));
	zassert_equal(fk.n, 4, "2 retries, then the report");
	for (size_t i = 0; i < 3; i++) {
		zassert_equal(fk.log[i].kind, APP_RADIO_FRAME_ALARM, "attempt %zu", i);
		zassert_equal(fk.log[i].head[0], 0xa1);
		zassert_equal(fk.log[i].len, 8);
		zassert_equal(fk.log[i].flags, APP_RADIO_FRAME_CONFIRMED);
		zassert_equal(fk.log[i].attempt, i);
	}
	zassert_within(fk.log[1].at_ms - fk.log[0].at_ms, 1500, 510, "retry 1 after %lld ms",
		       (long long)(fk.log[1].at_ms - fk.log[0].at_ms));
	zassert_within(fk.log[2].at_ms - fk.log[1].at_ms, 2500, 1510, "retry 2 after %lld ms",
		       (long long)(fk.log[2].at_ms - fk.log[1].at_ms));
	zassert_equal(fk.log[3].kind, APP_RADIO_FRAME_TELEMETRY, "the report waited for the Ack");
	zassert_equal(retries(), base + 2);
	zassert_false(app_radio_ack_pending());
	app_radio_get_link(&l);
	zassert_equal(l.fail_streak, 0, "acknowledged: no failed link check");
}
BOTH_PROFILES(confirmed_alarm_retried_until_acked)

/* Unacknowledged after its retries, the frame is given up as sent -- it went
 * out -- and the next frame goes then. An alarm is no link check: the streak
 * stays (only a link-check report judges the link, Hynek 2026-10-06). */
static void confirmed_frame_given_up_after_its_retries(void)
{
	struct app_radio_link l;

	g_app_config.radio_alarm_ack = true;
	script_fill(-ETIMEDOUT, 1 + APP_RADIO_ACK_MAX_RETRIES);
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa1);
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa2);
	k_sleep(K_SECONDS(20)); /* > 2 + 4 + 8 s of spread */
	zassert_equal(fk.n, 2 + APP_RADIO_ACK_MAX_RETRIES);
	for (size_t i = 0; i <= APP_RADIO_ACK_MAX_RETRIES; i++) {
		zassert_equal(fk.log[i].head[0], 0xa1, "attempt %zu", i);
		zassert_equal(fk.log[i].attempt, i);
	}
	zassert_equal(fk.log[4].head[0], 0xa2, "the next alarm once the first is given up");
	zassert_equal(fk.log[4].attempt, 0);
	zassert_false(app_radio_ack_pending());
	app_radio_get_link(&l);
	zassert_equal(l.fail_streak, 0, "an alarm is no link check");
}
BOTH_PROFILES(confirmed_frame_given_up_after_its_retries)

/* P2P (F6): every report goes confirmed, but only the link-check one, lost
 * after its retries, is a failed check. */
ZTEST(radio_common, test_only_a_lost_link_check_report_fails_the_link)
{
	const size_t lens[] = {20};
	struct app_radio_link l;

	use_profile(&PROFILE_P2P);
	fk.report_flags = APP_RADIO_FRAME_CONFIRMED;
	g_app_config.radio_link_check_interval = 5;
	app_radio_link_up(); /* report #1 is a check, #2 is not */

	script_fill(-ETIMEDOUT, 1 + APP_RADIO_ACK_MAX_RETRIES);
	frames(lens, 1);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(20)); /* > 2 + 4 + 8 s of spread */
	zassert_true(fk.log[0].flags & APP_RADIO_FRAME_LINK_CHECK);
	app_radio_get_link(&l);
	zassert_equal(l.fail_streak, 1, "the link-check report was lost");

	size_t n = fk.n;

	script_fill(-ETIMEDOUT, n + 1 + APP_RADIO_ACK_MAX_RETRIES);
	frames(lens, 1);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(20));
	zassert_equal(fk.n, n + 1 + APP_RADIO_ACK_MAX_RETRIES, "a confirmed report, retried");
	zassert_false(fk.log[n].flags & APP_RADIO_FRAME_LINK_CHECK);
	app_radio_get_link(&l);
	zassert_equal(l.fail_streak, 1, "a lost plain report is no failed check");
	g_app_config.radio_link_check_interval = 0;
}

/* A duty-cycle hold of a retry takes a fresh spread on top of its wait, or the
 * nodes it held would retry together again; the held send is no retry. */
static void retry_held_by_the_duty_cycle_spreads_again(void)
{
	const int r[] = {-ETIMEDOUT, -EAGAIN, 0};
	uint32_t base = retries();

	g_app_config.radio_alarm_ack = true;
	script(r, ARRAY_SIZE(r));
	fk.wait_ms = 5000;
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa1);
	k_sleep(K_SECONDS(12));
	zassert_equal(fk.n, 3);
	zassert_equal(fk.log[1].attempt, 1);
	zassert_equal(fk.log[2].attempt, 1, "the same retry, sent now");
	zassert_within(fk.log[2].at_ms - fk.log[1].at_ms, 6500, 510, "held %lld ms",
		       (long long)(fk.log[2].at_ms - fk.log[1].at_ms));
	zassert_equal(retries(), base + 1);
}
BOTH_PROFILES(retry_held_by_the_duty_cycle_spreads_again)

/* A new session starts the frame afresh -- P2P: a new counter under the new
 * key, never a retry of the old session's. */
static void link_up_starts_a_waiting_retry_afresh(void)
{
	const int r[] = {-ETIMEDOUT};
	uint32_t base = retries();

	g_app_config.radio_alarm_ack = true;
	script(r, ARRAY_SIZE(r));
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa1);
	k_sleep(K_MSEC(100));
	zassert_true(app_radio_ack_pending());
	app_radio_link_up();
	k_sleep(K_SECONDS(3));
	zassert_equal(fk.n, 2);
	zassert_equal(fk.log[1].head[0], 0xa1);
	zassert_equal(fk.log[1].attempt, 0, "a new frame");
	zassert_equal(retries(), base);
}
BOTH_PROFILES(link_up_starts_a_waiting_retry_afresh)

/* A confirmed report frame without its Ack goes again before the report's
 * next frame and before any queued one. */
static void report_frame_retried_before_the_next(void)
{
	const int r[] = {-ETIMEDOUT};
	const size_t lens[] = {20, 12};

	fk.report_flags = APP_RADIO_FRAME_CONFIRMED;
	frames(lens, 2);
	script(r, ARRAY_SIZE(r));
	app_radio_send_telemetry_now();
	k_sleep(K_MSEC(100));
	zassert_equal(fk.n, 1);
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	k_sleep(K_SECONDS(10));
	zassert_equal(fk.n, 4);
	zassert_equal(fk.log[1].kind, APP_RADIO_FRAME_TELEMETRY, "the retry goes first");
	zassert_equal(fk.log[1].len, 20);
	zassert_equal(fk.log[1].attempt, 1);
	zassert_mem_equal(fk.log[1].head, fk.log[0].head, sizeof(fk.log[0].head));
	zassert_equal(fk.log[2].head[0], 0xb1, "then the queued answer");
	zassert_equal(fk.log[3].kind, APP_RADIO_FRAME_TELEMETRY);
	zassert_equal(fk.log[3].len, 12);
	zassert_equal(fk.log[3].attempt, 0);
	zassert_equal(reports_done(), 1);
}
BOTH_PROFILES(report_frame_retried_before_the_next)

/* ---- Over-budget recovery (#409 3g) --------------------------------------- */

static void cmd_response_over_budget(const uint8_t *resp, size_t len, uint32_t want_seq)
{
	const int r[] = {-EMSGSIZE};

	script(r, 1);
	fk.res_budget = 20;
	zassert_ok(app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_CMD_RESPONSE, 0, resp,
				      len));
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 2);
	zassert_equal(fk.log[0].tag, APP_RADIO_TAG_CMD_RESPONSE);
	zassert_equal(fk.log[0].len, len);
	zassert_equal(g_budget_error_seq, want_seq);
	zassert_equal(g_budget_error_cap, 20);
	zassert_equal(fk.log[1].tag, APP_RADIO_TAG_OTHER, "the Error is never recovered again");
	zassert_equal(fk.log[1].len, 4);
	zassert_equal(fk.log[1].head[3], 0x2a);
	zassert_ok(fk.log[1].ret);
}

/* A command answer that no longer fits is replaced by Error BUDGET_TOO_SMALL
 * with the command's seq. */
static void cmd_response_replaced_by_budget_error(void)
{
	uint8_t resp[30] = {0x01, 0x08, 0x07};

	cmd_response_over_budget(resp, sizeof(resp), 7);
}
BOTH_PROFILES(cmd_response_replaced_by_budget_error)

ZTEST(radio_common, test_cmd_response_seq_varint_and_absent)
{
	uint8_t multi[30] = {0x01, 0x08, 0xac, 0x02}; /* seq 300 */
	uint8_t none[30] = {0x01, 0x12, 0x03};        /* field 2 first: seq 0 */

	cmd_response_over_budget(multi, sizeof(multi), 300);
	app_radio_test_tx_reset();
	memset(fk.log, 0, sizeof(fk.log));
	fk.n = 0;
	cmd_response_over_budget(none, sizeof(none), 0);
}

/* The announce frames are re-armed instead; the queue goes on. */
ZTEST(radio_common, test_announce_frames_over_budget_are_rearmed)
{
	const int r[] = {-EMSGSIZE, -EMSGSIZE};

	script(r, 2);
	fk.res_budget = 20;
	fk.ready = false;
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_INFO, 30, 0xc1);
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_SETTINGS, 30, 0xc2);
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 3);
	zassert_equal(fk.log[2].head[0], 0xb1);
	zassert_ok(fk.log[2].ret);
	zassert_true(app_radio_announce_pending());
	zassert_equal(g_budget_error_calls, 0);
}

/* Alarms, untagged answers and answers whose Error does not fit either are
 * dropped. */
ZTEST(radio_common, test_other_frames_over_budget_are_dropped)
{
	const int r[] = {-EMSGSIZE, -EMSGSIZE, -EMSGSIZE};
	uint8_t resp[30] = {0x01, 0x08, 0x05};

	script(r, 3);
	fk.res_budget = 3;
	fk.ready = false;
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 30, 0xb1);
	zassert_ok(app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_CMD_RESPONSE, 0, resp,
				      sizeof(resp)));
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 30, 0xa1);
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa2);
	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 4);
	zassert_equal(g_budget_error_calls, 1);
	zassert_equal(fk.log[3].head[0], 0xa2);
	zassert_ok(fk.log[3].ret);
}

/* ---- Telemetry ------------------------------------------------------------ */

/* A report is composed frame by frame against the budget of the moment: the
 * link check rides the first frame only, MORE marks all but the last, the
 * backend's frame gap separates them, report_flags() runs once per report and
 * the report counts once, after its last frame. */
static void report_frames_and_flags(void)
{
	const size_t lens[] = {20, 20, 10};

	frames(lens, 3);
	fk.report_flags = APP_RADIO_FRAME_CONFIRMED | APP_RADIO_FRAME_LINK_CHECK;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 3);
	zassert_equal(fk.log[0].flags, APP_RADIO_FRAME_CONFIRMED | APP_RADIO_FRAME_LINK_CHECK |
					       APP_RADIO_FRAME_MORE);
	zassert_equal(fk.log[1].flags, APP_RADIO_FRAME_CONFIRMED | APP_RADIO_FRAME_MORE);
	zassert_equal(fk.log[2].flags, APP_RADIO_FRAME_CONFIRMED);
	for (size_t i = 0; i < 3; i++) {
		zassert_equal(fk.log[i].kind, APP_RADIO_FRAME_TELEMETRY);
		zassert_equal(fk.log[i].len, lens[i]);
		zassert_equal(fk.log[i].head[0], 0, "frame %zu of another report", i);
		zassert_equal(fk.log[i].head[1], i);
	}
	assert_spacing(0, 2, m_prof->be->frame_gap_ms);
	zassert_equal(fk.report_flags_calls, 1);
	zassert_equal(reports_done(), 1);
	zassert_equal(g_compose_reset_calls, 0);
}
BOTH_PROFILES(report_frames_and_flags)

/* Requests before the report left fold into it (they carry the same data). */
ZTEST(radio_common, test_requests_fold_into_one_report)
{
	const size_t lens[] = {20};

	frames(lens, 1);
	fk.ready = false;
	for (int i = 0; i < 3; i++) {
		app_radio_send_telemetry_now();
		k_sleep(K_MSEC(10));
	}
	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(10));
	zassert_equal(fk.n, 1);
	zassert_equal(reports_done(), 1);
}

/* A request that arrives while a report is on the air is a report of its own,
 * sent after that one. */
static void request_during_a_report_follows_it(void)
{
	const size_t lens[] = {20, 20};

	frames(lens, 2);
	fk.request_after = 1;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 4);
	zassert_equal(fk.log[0].head[0], 0);
	zassert_equal(fk.log[1].head[0], 0);
	zassert_equal(fk.log[2].head[0], 1);
	zassert_equal(fk.log[3].head[0], 1);
	zassert_equal(fk.report_flags_calls, 2);
	zassert_equal(reports_done(), 2);
}
BOTH_PROFILES(request_during_a_report_follows_it)

/* A telemetry frame that keeps failing is given up after 8 retries; the
 * snapshot is reset so the next report takes fresh data (#340 M6). */
static void telemetry_error_abandons_the_report(void)
{
	const size_t lens[] = {20};
	uint32_t base = retries();

	frames(lens, 1);
	script_fill(-EIO, 9);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(150));
	zassert_equal(fk.n, 9);
	assert_spacing(0, 8, RETRY_MS);
	zassert_equal(retries(), base + 8);
	zassert_equal(g_compose_reset_calls, 1);
	zassert_equal(reports_done(), 0);

	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 10);
	zassert_ok(fk.log[9].ret);
	zassert_equal(reports_done(), 1);
}
BOTH_PROFILES(telemetry_error_abandons_the_report)

/* Unlike a queued frame, a duty-held telemetry frame counts: it is resent
 * after res->wait_ms, 8 times at most. */
static void telemetry_duty_hold_counts(void)
{
	const size_t lens[] = {20};
	uint32_t base = retries();

	frames(lens, 1);
	script_fill(-EAGAIN, 9);
	fk.wait_ms = 1000;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(20));
	zassert_equal(fk.n, 9);
	assert_spacing(0, 8, 1000);
	zassert_equal(retries(), base + 8);
	zassert_equal(g_compose_reset_calls, 1);
	zassert_equal(reports_done(), 0);
}
BOTH_PROFILES(telemetry_duty_hold_counts)

/* M-10: a frame over the budget on its own is dropped, the report goes on. */
static void telemetry_over_budget_frame_is_skipped(void)
{
	const size_t lens[] = {20, 30, 10};
	const int r[] = {0, -EMSGSIZE, 0};

	frames(lens, 3);
	script(r, 3);
	fk.res_budget = 25;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 3);
	zassert_equal(fk.log[2].head[1], 2);
	zassert_equal(reports_done(), 1);
	zassert_equal(g_compose_reset_calls, 0);
}
BOTH_PROFILES(telemetry_over_budget_frame_is_skipped)

/* ... the last frame included: a report that did not fully leave does not
 * count, but no snapshot reset either. */
ZTEST(radio_common, test_telemetry_over_budget_last_frame)
{
	const size_t lens[] = {20, 30};
	const int r[] = {0, -EMSGSIZE};

	frames(lens, 2);
	script(r, 2);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 2);
	zassert_equal(reports_done(), 0);
	zassert_equal(g_compose_reset_calls, 0);
}

/* H-1: no budget (pending MAC answers fill the frame) sends an empty frame so
 * the MAC drains them, instead of going mute. */
static void budget_zero_sends_an_empty_frame(void)
{
	const size_t lens[] = {20};

	frames(lens, 1);
	fk.budget = 0;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 1);
	zassert_equal(fk.log[0].kind, APP_RADIO_FRAME_TELEMETRY);
	zassert_equal(fk.log[0].len, 0);
	zassert_equal(fk.report_flags_calls, 0);
	zassert_equal(reports_done(), 0);
	zassert_equal(g_compose_reset_calls, 0, "nothing of the snapshot left yet");

	fk.budget = m_prof->budget;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 2);
	zassert_equal(fk.log[1].len, 20);
	zassert_equal(reports_done(), 1);
}
BOTH_PROFILES(budget_zero_sends_an_empty_frame)

/* ... mid-report: the flush ends the report and resets its snapshot. */
ZTEST(radio_common, test_budget_zero_mid_report_resets_the_snapshot)
{
	const size_t lens[] = {20, 20};

	frames(lens, 2);
	fk.zero_budget_after = 1;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(10));
	zassert_equal(fk.n, 2);
	zassert_equal(fk.log[1].len, 0);
	zassert_equal(g_compose_reset_calls, 1);
	zassert_equal(reports_done(), 0);
}

/* A link lost mid-report abandons it with a snapshot reset (#93.5); the next
 * report starts from its first frame. */
static void link_loss_mid_report_resets_the_snapshot(void)
{
	const size_t lens[] = {20, 20, 10};

	frames(lens, 3);
	fk.drop_link_after = 1;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 1);
	zassert_equal(g_compose_reset_calls, 1);
	zassert_equal(reports_done(), 0);

	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(10));
	zassert_equal(fk.n, 1, "the abandoned report was continued");

	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 4);
	zassert_equal(fk.log[1].head[0], 1, "not a fresh snapshot");
	zassert_equal(fk.log[1].head[1], 0);
	zassert_equal(reports_done(), 1);
}
BOTH_PROFILES(link_loss_mid_report_resets_the_snapshot)

/* -EBUSY holds a report frame until the kick; -ENOTCONN abandons the report. */
static void telemetry_busy_waits_notconn_abandons(void)
{
	const size_t lens[] = {20};
	const int busy[] = {-EBUSY};
	const int notconn[] = {-ENOTCONN};

	frames(lens, 1);
	script(busy, 1);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(60));
	zassert_equal(fk.n, 1);
	app_radio_tx_kick();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 2);
	zassert_equal(reports_done(), 1);

	fk.n = 0;
	script(notconn, 1);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 1);
	zassert_equal(g_compose_reset_calls, 1);
	app_radio_tx_kick();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 1);
	zassert_equal(reports_done(), 1);
}
BOTH_PROFILES(telemetry_busy_waits_notconn_abandons)

/* ---- API ------------------------------------------------------------------ */

ZTEST(radio_common, test_answer_cap_follows_the_budget)
{
	fk.budget = 51;
	zassert_equal(app_radio_tx_answer_cap(64), 51);
	zassert_equal(app_radio_tx_answer_cap(30), 30);
	fk.budget = 0; /* unknown now: encode against the slot */
	zassert_equal(app_radio_tx_answer_cap(64), APP_RADIO_TX_SLOT_SIZE);
	fk.budget = 239;
	zassert_equal(app_radio_tx_answer_cap(200), APP_RADIO_TX_SLOT_SIZE);
}

ZTEST(radio_common, test_queue_validation)
{
	uint8_t big[APP_RADIO_TX_SLOT_SIZE + 1] = {0};

	fk.ready = false;
	zassert_equal(app_radio_tx_queue(APP_RADIO_FRAME_TELEMETRY, 0, 0, big, 8), -EINVAL);
	zassert_equal(app_radio_tx_queue(APP_RADIO_FRAME_HISTORY, 0, 0, big, 8), -EINVAL);
	zassert_equal(app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, 0, 0, big, 0), -EINVAL);
	zassert_equal(app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, 0, 0, NULL, 8), -EINVAL);
	zassert_equal(app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, 0, 0, big, sizeof(big)),
		      -EMSGSIZE);
	zassert_false(app_radio_tx_answer_pending());
	zassert_false(app_radio_tx_alarm_pending());
	zassert_equal(app_radio_tx_alarm_free(), APP_RADIO_TX_QUEUE_DEPTH);

	for (int i = 0; i < APP_RADIO_TX_QUEUE_DEPTH; i++) {
		zassert_ok(app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, 0, 0, big,
					      APP_RADIO_TX_SLOT_SIZE));
		zassert_ok(app_radio_tx_queue(APP_RADIO_FRAME_ALARM, 0, 0, big, 8));
	}
	zassert_equal(app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, 0, 0, big, 8), -ENOMEM);
	zassert_equal(app_radio_tx_queue(APP_RADIO_FRAME_ALARM, 0, 0, big, 8), -ENOMEM);
	zassert_equal(app_radio_tx_answer_free(), 0);
	zassert_true(app_radio_tx_answer_pending());
	zassert_equal(app_radio_tx_alarm_free(), 0);
	zassert_true(app_radio_tx_alarm_pending());
}

/* Calibration mode transmits through its own path. */
ZTEST(radio_common, test_calibration_sends_nothing)
{
	g_app_config.calibration = true;
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.n, 0);
}

/* ---- Link supervision (doc/plan/460 §2.4, F2) ------------------------------ */

static void fail_n(int n)
{
	for (int i = 0; i < n; i++) {
		app_radio_link_result(false);
	}
}

static uint32_t fails(void)
{
	struct app_radio_status st;

	app_radio_get_status(&st);
	return st.cnt[APP_RADIO_CNT_FAIL];
}

static struct app_radio_link link(void)
{
	struct app_radio_link l;

	app_radio_get_link(&l);
	return l;
}

/* Three failed link checks in a row: WARNING, session kept, and the first
 * recovery rung taken at once. */
static void warning_after_three_failures(void)
{
	struct app_radio_status st;
	uint32_t f0 = fails();

	fk.steps_left = 5;
	app_radio_link_up();
	fail_n(2);
	zassert_equal(app_radio_get_state(), APP_RADIO_STATE_HEALTHY, "2 failures: healthy");
	zassert_equal(fk.step_calls, 0, "no rung before WARNING");
	zassert_equal(link().fail_streak, 2);

	fail_n(1);
	zassert_equal(app_radio_get_state(), APP_RADIO_STATE_WARNING, "3 failures: WARNING");
	zassert_true(link().warning);
	zassert_equal(fk.step_calls, 1, "entering WARNING takes the first rung");
	zassert_equal(fk.rejoin_calls, 0, "the session is kept");

	app_radio_get_status(&st);
	zassert_equal(st.state, APP_RADIO_STATE_WARNING, "status carries WARNING");
	zassert_equal(st.cnt[APP_RADIO_CNT_FAIL] - f0, 3, "every failure is counted");
	zassert_equal(st.fail_streak, 3, "status carries the streak");
}
BOTH_PROFILES(warning_after_three_failures)

/* One passed check anywhere: back to HEALTHY, streak cleared. */
static void success_ends_warning(void)
{
	app_radio_link_up();
	fail_n(4);
	zassert_equal(app_radio_get_state(), APP_RADIO_STATE_WARNING);
	app_radio_link_result(true);
	zassert_equal(app_radio_get_state(), APP_RADIO_STATE_HEALTHY);
	zassert_equal(link().fail_streak, 0);
	zassert_equal(link().warning_fails, 0);
	zassert_false(link().warning);

	/* A success also resets a streak short of WARNING. */
	fail_n(2);
	app_radio_link_result(true);
	fail_n(2);
	zassert_equal(app_radio_get_state(), APP_RADIO_STATE_HEALTHY, "streak restarted");
}
BOTH_PROFILES(success_ends_warning)

/* No rung left: radio-link-check-fail-rejoin more failures in WARNING, then a
 * rejoin (not forced -- the backend may refuse it). */
static void rejoin_after_the_budget(void)
{
	g_app_config.radio_link_check_fail_rejoin = 3;
	app_radio_link_up();
	fail_n(3);
	zassert_equal(app_radio_get_state(), APP_RADIO_STATE_WARNING);
	fail_n(2);
	zassert_equal(fk.rejoin_calls, 0, "2 of 3 in WARNING: kept");
	zassert_equal(link().warning_fails, 2);
	fail_n(1);
	zassert_equal(fk.rejoin_calls, 1, "3 of 3 in WARNING: rejoin");
	zassert_false(fk.rejoin_forced);

	/* A budget of 0 still takes one failure in WARNING. */
	app_radio_test_link_reset();
	fk.rejoin_calls = 0;
	g_app_config.radio_link_check_fail_rejoin = 0;
	app_radio_link_up();
	fail_n(3);
	zassert_equal(fk.rejoin_calls, 0);
	fail_n(1);
	zassert_equal(fk.rejoin_calls, 1, "budget 0 counts as 1");
}
BOTH_PROFILES(rejoin_after_the_budget)

/* A rejoin is never spent while a rung is still untried. */
static void rung_defers_the_rejoin(void)
{
	g_app_config.radio_link_check_fail_rejoin = 1;
	fk.steps_left = 3;
	app_radio_link_up();
	fail_n(3); /* WARNING, rung 1 */
	fail_n(2); /* rungs 2 and 3 */
	zassert_equal(fk.step_calls, 3);
	zassert_equal(fk.rejoin_calls, 0, "rungs left: no rejoin");
	fail_n(1);
	zassert_equal(fk.step_calls, 4, "the rung is tried first");
	zassert_equal(fk.rejoin_calls, 1, "no rung left: rejoin");
}
BOTH_PROFILES(rung_defers_the_rejoin)

/* A backend that cannot rejoin (LoRaWAN ABP, P2P unprovisioned) stays in
 * WARNING, checks every report and tries again after the next budget. */
static void refused_rejoin_stays_warning(void)
{
	g_app_config.radio_link_check_fail_rejoin = 2;
	fk.rejoin_ret = -ENOTSUP;
	app_radio_link_up();
	fail_n(5);
	zassert_equal(fk.rejoin_calls, 1);
	zassert_equal(app_radio_get_state(), APP_RADIO_STATE_WARNING, "kept in WARNING");
	zassert_equal(link().warning_fails, 0, "the budget starts again");
	fail_n(1);
	zassert_equal(fk.rejoin_calls, 1);
	fail_n(1);
	zassert_equal(fk.rejoin_calls, 2, "the next budget asks again");
}
BOTH_PROFILES(refused_rejoin_stays_warning)

/* Outcomes while the backend (re)joins belong to no session: ignored, but a
 * failure still counts. WARNING is an overlay on HEALTHY only. */
static void results_ignored_unless_healthy(void)
{
	uint32_t f0 = fails();

	app_radio_link_up();
	fail_n(3);
	fk.state = APP_RADIO_STATE_RECONNECT;
	zassert_equal(app_radio_get_state(), APP_RADIO_STATE_RECONNECT, "no WARNING overlay");
	fail_n(10);
	zassert_equal(fk.rejoin_calls, 0, "no rejoin during a rejoin");
	zassert_equal(fk.step_calls, 1, "only the WARNING entry's rung");
	zassert_equal(link().fail_streak, 3, "streak kept until the link-up");
	zassert_equal(fails() - f0, 13, "every failure counted");

	fk.state = APP_RADIO_STATE_HEALTHY;
	app_radio_link_up();
	zassert_equal(app_radio_get_state(), APP_RADIO_STATE_HEALTHY, "a link-up starts afresh");
	zassert_equal(link().fail_streak, 0);
	zassert_equal(link().reports, 0);
}
BOTH_PROFILES(results_ignored_unless_healthy)

/* One single-frame report; returns whether it rode as a link check. */
static bool report_is_check(void)
{
	size_t n = fk.n;
	const size_t lens[] = {20};

	frames(lens, 1);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(4)); /* past the LoRaWAN-like frame gap */
	zassert_equal(fk.n, n + 1, "one frame per report");
	return (fk.log[n].flags & m_prof->due_flag) != 0;
}

/* The cadence (PF-1): report #1 after a link-up, then every N-th; none with
 * N = 0; the count restarts at the next link-up. */
static void link_check_cadence(void)
{
	g_app_config.radio_link_check_interval = 3;
	app_radio_link_up();
	for (int i = 0; i < 7; i++) {
		zassert_equal(report_is_check(), (i % 3) == 0, "N = 3, report %d", i);
	}
	app_radio_link_up();
	zassert_true(report_is_check(), "the first report after a link-up");

	g_app_config.radio_link_check_interval = 0;
	app_radio_link_up();
	for (int i = 0; i < 3; i++) {
		zassert_false(report_is_check(), "N = 0, report %d", i);
	}
}
BOTH_PROFILES(link_check_cadence)

/* §3.4 / LoRaWAN #424: while WARNING every report is a link check. */
static void every_report_checks_in_warning(void)
{
	g_app_config.radio_link_check_interval = 0;
	app_radio_link_up();
	fail_n(3);
	for (int i = 0; i < 3; i++) {
		zassert_true(report_is_check(), "WARNING, report %d", i);
	}
	app_radio_link_result(true);
	zassert_false(report_is_check(), "HEALTHY again: back to the cadence");
}
BOTH_PROFILES(every_report_checks_in_warning)

/* `ats lrw check` / a forced check: the next report, once -- kept until one
 * actually rode (a LoRaWAN LinkCheckReq already pending carries none). */
static void forced_check_rides_once(void)
{
	g_app_config.radio_link_check_interval = 0;
	app_radio_link_up();
	app_radio_force_link_check();
	fk.suppress_due = true;
	zassert_false(report_is_check(), "no check could ride");
	fk.suppress_due = false;
	zassert_true(report_is_check(), "the forced check rides the next report");
	zassert_false(report_is_check(), "used up");
}
BOTH_PROFILES(forced_check_rides_once)

ZTEST(radio_common, test_link_check_due_rule)
{
	for (uint32_t i = 0; i < 12; i++) {
		zassert_equal(app_radio_link_check_due(i, 5, false), (i % 5) == 0, "N = 5, idx %u",
			      i);
		zassert_true(app_radio_link_check_due(i, 1, false), "N = 1, idx %u", i);
		zassert_false(app_radio_link_check_due(i, 0, false), "N = 0, idx %u", i);
		zassert_false(app_radio_link_check_due(i, -1, false), "N < 0, idx %u", i);
		zassert_true(app_radio_link_check_due(i, 0, true), "WARNING, N = 0, idx %u", i);
		zassert_true(app_radio_link_check_due(i, 5, true), "WARNING, N = 5, idx %u", i);
	}
}

/* ---- M-2 stale-uplink watchdog (F29), common to both radios ------------------ */

#define STALE_MS ((int64_t)APP_RADIO_STALE_FACTOR * 60 * 1000) /* interval_report 60 s */

/* No telemetry for > APP_RADIO_STALE_FACTOR report intervals: forced rejoin. */
static void stale_uplink_rejoins(void)
{
	app_radio_link_up();
	int64_t t0 = k_uptime_get();

	app_radio_test_stale_tick(t0 + STALE_MS - 1000);
	zassert_equal(fk.rejoin_calls, 0, "not stale yet");
	app_radio_test_stale_tick(t0 + STALE_MS + 1000);
	zassert_equal(fk.rejoin_calls, 1, "stale: rejoin");
	zassert_true(fk.rejoin_forced, "an M-2 rejoin is forced");
	app_radio_test_stale_tick(t0 + STALE_MS + 2000);
	zassert_equal(fk.rejoin_calls, 1, "not again on the next tick");

	/* Not HEALTHY (already rejoining): nothing. */
	fk.state = APP_RADIO_STATE_RECONNECT;
	app_radio_test_stale_tick(t0 + 3 * STALE_MS);
	zassert_equal(fk.rejoin_calls, 1);
}
BOTH_PROFILES(stale_uplink_rejoins)

/* The duty cycle explains a mute node: no rejoin. */
static void stale_duty_hold_waits(void)
{
	app_radio_link_up();
	int64_t t0 = k_uptime_get();

	k_sleep(K_SECONDS(1));
	app_radio_note_send(false, true);
	app_radio_test_stale_tick(k_uptime_get() + STALE_MS - 500);
	zassert_equal(fk.rejoin_calls, 0, "duty hold: no rejoin");

	app_radio_note_send(true, false);
	app_radio_test_stale_tick(t0 + 2 * STALE_MS);
	zassert_equal(fk.rejoin_calls, 1, "a send ended the hold: rejoin");
}
BOTH_PROFILES(stale_duty_hold_waits)

/* T2d on the bench (EU868 DR0): the ledger held a frame for 41 min and nothing
 * was tried meanwhile. The excuse must last as long as the known hold, not just
 * one interval + margin past the single held attempt. */
ZTEST(radio_common, test_stale_known_hold_lasts_until_its_end)
{
	const int64_t iv = 60 * 1000;
	const int64_t margin = iv + APP_RADIO_STALE_DC_RECENT_MARGIN_MS;
	const int64_t hold = 41 * 60 * 1000;
	const int64_t t0 = 10 * 60 * 1000; /* the hold starts 10 min after the uplink */
	struct app_radio_stale_dc dc = {0};

	app_radio_stale_note_hold(&dc, t0, hold);
	zassert_equal(app_radio_stale_check(t0 + margin + 1000, 1, &dc, 60),
		      APP_RADIO_STALE_HOLD_DC, "past one interval + margin, still held");
	zassert_equal(app_radio_stale_check(t0 + hold, 1, &dc, 60), APP_RADIO_STALE_HOLD_DC,
		      "the hold ends");
	zassert_equal(app_radio_stale_check(t0 + hold + margin, 1, &dc, 60),
		      APP_RADIO_STALE_HOLD_DC, "one interval + margin after its end");
	zassert_equal(app_radio_stale_check(t0 + hold + margin + 1000, 1, &dc, 60),
		      APP_RADIO_STALE_REJOIN, "nothing went after the hold: rejoin");

	/* A later, shorter hold never shortens the known end. */
	app_radio_stale_note_hold(&dc, t0 + 60 * 1000, 1000);
	zassert_equal(dc.until_ms, t0 + hold);

	/* A send ends it. */
	app_radio_stale_note(&dc, true, false, t0 + 2 * 60 * 1000);
	zassert_equal(app_radio_stale_check(t0 + margin + 1000, 1, &dc, 60), APP_RADIO_STALE_REJOIN,
		      "the streak is gone");

	/* The cap still bounds a hold longer than the window plus its margin. */
	app_radio_stale_note_hold(&dc, t0, 2 * APP_RADIO_STALE_DC_HOLD_MAX_MS);
	zassert_equal(app_radio_stale_check(t0 + APP_RADIO_STALE_DC_HOLD_MAX_MS - 1, 1, &dc, 60),
		      APP_RADIO_STALE_HOLD_DC);
	zassert_equal(app_radio_stale_check(t0 + APP_RADIO_STALE_DC_HOLD_MAX_MS, 1, &dc, 60),
		      APP_RADIO_STALE_REJOIN, "no hold explains more than the cap");
}

/* The same through the TX path: a frame the ledger holds for half an hour
 * keeps M-2 quiet the whole time; once the hold is past, a mute node rejoins. */
static void stale_ledger_hold_keeps_m2_quiet(void)
{
	const int64_t hold = 30 * 60 * 1000;

	app_radio_link_up();
	app_radio_duty_init(APP_RADIO_DUTY_1PCT_MS);
	fk.air_ms = 1000;
	/* A full hour's allowance that leaves the window in 30 min. */
	app_radio_test_duty_charge_at(k_uptime_get() - APP_RADIO_DUTY_WINDOW_MS + hold,
				      APP_RADIO_DUTY_1PCT_MS);

	int64_t t0 = k_uptime_get();

	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xd2);
	k_sleep(K_MSEC(500));
	zassert_equal(fk.n, 0, "held by the ledger");

	app_radio_test_stale_tick(t0 + STALE_MS + 60 * 1000 + APP_RADIO_STALE_DC_RECENT_MARGIN_MS);
	zassert_equal(fk.rejoin_calls, 0, "held: no rejoin past one interval + margin");
	app_radio_test_stale_tick(t0 + hold);
	zassert_equal(fk.rejoin_calls, 0, "held: no rejoin up to its end");
	app_radio_test_stale_tick(t0 + hold + 60 * 1000 + APP_RADIO_STALE_DC_RECENT_MARGIN_MS +
				  5000);
	zassert_equal(fk.rejoin_calls, 1, "mute after the hold: rejoin");
}
BOTH_PROFILES(stale_ledger_hold_keeps_m2_quiet)

/* A telemetry report that left, or a history frame, refreshes the clock. */
static void uplinks_refresh_the_stale_clock(void)
{
	app_radio_link_up();
	k_sleep(K_SECONDS(30));
	(void)report_is_check();
	int64_t t1 = k_uptime_get();

	app_radio_test_stale_tick(t1 + STALE_MS - 5000); /* sent 4 s before t1 */
	zassert_equal(fk.rejoin_calls, 0, "the report refreshed it");

	k_sleep(K_SECONDS(30));
	app_radio_note_uplink();
	int64_t t2 = k_uptime_get();

	app_radio_test_stale_tick(t2 + STALE_MS - 1000);
	zassert_equal(fk.rejoin_calls, 0, "a history uplink refreshed it");
	app_radio_test_stale_tick(t2 + STALE_MS + 1000);
	zassert_equal(fk.rejoin_calls, 1);

	/* No link-up yet: no clock, no rejoin. */
	app_radio_test_link_reset();
	app_radio_test_stale_tick(t2 + 10 * STALE_MS);
	zassert_equal(fk.rejoin_calls, 1);
}
BOTH_PROFILES(uplinks_refresh_the_stale_clock)

/* An outage: confirmed telemetry goes on air but no Ack comes back. Given up,
 * it still counts as sent, so M-2 stays quiet and supervision alone judges the
 * link by the reports that carried a link check: WARNING after 3, the rejoin
 * (not forced) radio-link-check-fail-rejoin later. */
static void outage_leaves_the_link_to_supervision(void)
{
	const size_t lens[] = {20};

	g_app_config.radio_link_check_interval = 1;
	g_app_config.radio_link_check_fail_rejoin = 5;
	fk.report_flags = APP_RADIO_FRAME_CONFIRMED;
	script_fill(-ETIMEDOUT, LOG_MAX);
	app_radio_link_up();

	for (int i = 0; i < 8; i++) {
		frames(lens, 1);
		app_radio_send_telemetry_now();
		k_sleep(K_SECONDS(60));
		zassert_equal(fk.n, 4 * (i + 1), "report %d: sent and retried 3 times", i);
		zassert_equal(app_radio_get_state(),
			      i < 2 ? APP_RADIO_STATE_HEALTHY : APP_RADIO_STATE_WARNING,
			      "report %d", i);
		app_radio_test_stale_tick(k_uptime_get());
		zassert_equal(fk.rejoin_calls, i < 7 ? 0 : 1, "report %d", i);
	}
	zassert_false(fk.rejoin_forced, "the supervision rejoin, not M-2");
	zassert_true(k_uptime_get() > 2 * STALE_MS, "M-2 had its chance");
}
BOTH_PROFILES(outage_leaves_the_link_to_supervision)

/* ats radio tx_mute: telemetry never reaches the air, M-2 rejoins. */
static void tx_mute_trips_m2(void)
{
	const size_t lens[] = {20};

	app_radio_link_up();
	int64_t t0 = k_uptime_get();

	app_radio_debug_tx_mute(true);
	frames(lens, 1);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(150));
	zassert_equal(fk.n, 0, "nothing on air");
	zassert_equal(g_compose_reset_calls, 1, "abandoned after its retries");
	app_radio_test_stale_tick(t0 + STALE_MS + 1000);
	zassert_equal(fk.rejoin_calls, 1, "mute: M-2 rejoin");
	zassert_true(fk.rejoin_forced);

	app_radio_debug_tx_mute(false);
	frames(lens, 1);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(4));
	zassert_equal(fk.n, 1, "unmuted: sent");
}
BOTH_PROFILES(tx_mute_trips_m2)

/* ---- Rejoin backoff (common) ------------------------------------------------ */

ZTEST(radio_common, test_rejoin_backoff_doubles_then_caps)
{
	/* base, 2x, 4x, ... capped at 1 h. */
	zassert_equal(app_radio_rejoin_backoff_ms(0), 60000u, "attempt 0 should be the 60 s base");
	zassert_equal(app_radio_rejoin_backoff_ms(1), 120000u, "attempt 1 should double to 120 s");
	zassert_equal(app_radio_rejoin_backoff_ms(2), 240000u, "attempt 2 should be 240 s");
	zassert_equal(app_radio_rejoin_backoff_ms(3), 480000u, "attempt 3 should be 480 s");

	/* Monotonic non-decreasing, and never above the 1 h cap, for any attempt. */
	uint32_t prev = 0;

	for (int a = 0; a <= 255; a++) {
		uint32_t ms = app_radio_rejoin_backoff_ms((uint32_t)a);

		zassert_true(ms >= prev, "backoff not monotonic at attempt %d (%u < %u)", a, ms,
			     prev);
		zassert_true(ms <= 3600000u, "backoff %u at attempt %d exceeds the 1 h cap", ms, a);
		prev = ms;
	}
	zassert_equal(app_radio_rejoin_backoff_ms(255), 3600000u,
		      "a large attempt must saturate at 1 h");
}

/* +/-25 % of the backoff around the wait, never under the floor. */
ZTEST(radio_common, test_backoff_jitter_spreads_and_keeps_the_floor)
{
	const uint32_t base = app_radio_rejoin_backoff_ms(0);

	zassert_equal(app_radio_backoff_jitter_ms(base, 0, base, 0), base - base / 4);
	zassert_equal(app_radio_backoff_jitter_ms(base, 0, base, base / 2), base + base / 4);
	for (uint32_t r = 0; r < 4 * base; r += 997) {
		int64_t d = app_radio_backoff_jitter_ms(base, 0, base, r);

		zassert_true(d >= base - base / 4 && d <= base + base / 4, "draw %u: %lld", r,
			     (long long)d);
	}
	zassert_equal(app_radio_backoff_jitter_ms(base, 70000, base, 0), 70000,
		      "the floor wins over a negative draw");
	zassert_equal(app_radio_backoff_jitter_ms(1000, 0, base, 0), 0, "never negative");
}

/* ---- Downlink commands (F3) ------------------------------------------------ */

static const uint8_t m_cmd[] = {0x42, 0x01};

/* A command runs through app_cmd with the radio's own transport, capped to the
 * uplink budget, and its answer goes on the command port. */
static void downlink_answer_rides_the_command_path(void)
{
	g_cmd_resp_len = 10;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	k_sleep(K_SECONDS(1));
	zassert_equal(g_cmd_handle_calls, 1);
	zassert_equal(g_cmd_transport, m_prof->transport, "the radio's own transport");
	zassert_equal(g_cmd_out_cap, MIN(m_prof->budget, APP_RADIO_TX_SLOT_SIZE),
		      "capped to the uplink budget");
	zassert_equal(fk.n, 1);
	zassert_equal(fk.log[0].kind, APP_RADIO_FRAME_ANSWER);
	zassert_equal(fk.log[0].tag, APP_RADIO_TAG_CMD_RESPONSE);
	zassert_equal(fk.log[0].port, 0, "the command port");
	zassert_equal(fk.log[0].len, 10);
	zassert_equal(fk.log[0].head[0], 0xa0);
	zassert_equal(fk.log[0].head[1], 0x42);
	zassert_equal(fk.log[0].flags, m_prof->queued_flags);
	k_sleep(K_SECONDS(9));
	zassert_equal(g_run_action_calls, 0, "no deferred action");
}
BOTH_PROFILES(downlink_answer_rides_the_command_path)

/* A deferred action waits for its answer: held by the duty cycle for 10 s, the
 * answer misses the first 8 s check and the action runs at the next one. */
static void action_runs_after_the_answer_left(void)
{
	const int r[] = {-EAGAIN};
	int64_t t0 = k_uptime_get();

	script(r, ARRAY_SIZE(r));
	fk.wait_ms = 10000;
	g_cmd_resp_len = 10;
	g_cmd_action = APP_CMD_ACTION_SETTINGS_SAVE;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	k_sleep(K_SECONDS(12));
	zassert_equal(fk.n, 2, "the answer left on its second try");
	zassert_equal(fk.log[1].ret, 0);
	zassert_equal(g_run_action_calls, 0, "deferred past the first check");
	k_sleep(K_SECONDS(6));
	zassert_equal(g_run_action_calls, 1);
	zassert_equal(g_run_action_last, APP_CMD_ACTION_SETTINGS_SAVE);
	zassert_true(g_run_action_at_ms > fk.log[1].at_ms, "after the answer");
	zassert_within(g_run_action_at_ms - t0, 16000, 500, "at the second check (%lld ms)",
		       (long long)(g_run_action_at_ms - t0));
}
BOTH_PROFILES(action_runs_after_the_answer_left)

/* An answer that never leaves postpones the action six times at most. */
static void action_waits_six_times_at_most(void)
{
	int64_t t0 = k_uptime_get();

	script_fill(-EAGAIN, LOG_MAX);
	fk.wait_ms = 5000;
	g_cmd_resp_len = 10;
	g_cmd_action = APP_CMD_ACTION_REBOOT;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	k_sleep(K_SECONDS(50));
	zassert_equal(g_run_action_calls, 0, "still deferring");
	k_sleep(K_SECONDS(10));
	zassert_equal(g_run_action_calls, 1, "ran after the last deferral");
	zassert_within(g_run_action_at_ms - t0, 56000, 500, "8 s + 6 x 8 s (%lld ms)",
		       (long long)(g_run_action_at_ms - t0));
}
BOTH_PROFILES(action_waits_six_times_at_most)

/* An action also waits for its answer's Ack where answers are confirmed (P2P):
 * held by the duty cycle on its retry, the answer is acknowledged at ~22 s and
 * the action runs at the next 8 s check. LoRaWAN sends answers unconfirmed:
 * the action runs at the first check. */
static void action_waits_for_the_answer_ack(void)
{
	const int r[] = {-ETIMEDOUT, -EAGAIN};
	bool confirmed = (m_prof->queued_flags & APP_RADIO_FRAME_CONFIRMED) != 0;

	script(r, ARRAY_SIZE(r));
	fk.wait_ms = 20000;
	g_cmd_resp_len = 10;
	g_cmd_action = APP_CMD_ACTION_COUNTERS_SAVE;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	k_sleep(K_SECONDS(9));
	zassert_equal(g_run_action_calls, confirmed ? 0 : 1);
	k_sleep(K_SECONDS(25));
	zassert_equal(g_run_action_calls, 1);
	zassert_equal(g_run_action_last, APP_CMD_ACTION_COUNTERS_SAVE);
	zassert_equal(fk.n, confirmed ? 3 : 1);
	zassert_true(g_run_action_at_ms > fk.log[fk.n - 1].at_ms, "after the answer left");
}
BOTH_PROFILES(action_waits_for_the_answer_ack)

/* #462: the action (mostly a reboot) also waits for the alarm frames still
 * queued. Held by the duty cycle for 10 s, the alarm misses the first check. */
static void action_waits_for_queued_alarms(void)
{
	const int r[] = {0, -EAGAIN};
	int64_t t0 = k_uptime_get();

	script(r, ARRAY_SIZE(r));
	fk.wait_ms = 10000;
	g_cmd_resp_len = 10;
	g_cmd_action = APP_CMD_ACTION_SETTINGS_SAVE;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa3);
	k_sleep(K_SECONDS(14));
	zassert_equal(fk.n, 3, "answer, then the alarm on its second try");
	zassert_equal(fk.log[2].kind, APP_RADIO_FRAME_ALARM);
	zassert_equal(fk.log[2].ret, 0);
	zassert_equal(g_run_action_calls, 0, "deferred past the first check");
	k_sleep(K_SECONDS(4));
	zassert_equal(g_run_action_calls, 1);
	zassert_true(g_run_action_at_ms > fk.log[2].at_ms, "after the alarm");
	zassert_within(g_run_action_at_ms - t0, 16000, 500, "at the second check (%lld ms)",
		       (long long)(g_run_action_at_ms - t0));
}
BOTH_PROFILES(action_waits_for_queued_alarms)

/* #462: an alarm batch still collecting is sent early and the action waits a
 * check for it. */
static void action_waits_for_a_collecting_alarm_batch(void)
{
	int64_t t0 = k_uptime_get();

	g_alarm_pending_left = 1;
	g_cmd_resp_len = 10;
	g_cmd_action = APP_CMD_ACTION_REBOOT;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	k_sleep(K_SECONDS(9));
	zassert_equal(g_alarm_pending_calls, 1, "asked at the first check");
	zassert_equal(g_run_action_calls, 0, "deferred for the batch");
	k_sleep(K_SECONDS(8));
	zassert_equal(g_run_action_calls, 1);
	zassert_within(g_run_action_at_ms - t0, 16000, 500, "at the second check (%lld ms)",
		       (long long)(g_run_action_at_ms - t0));
}
BOTH_PROFILES(action_waits_for_a_collecting_alarm_batch)

/* #462: taking an alarm frame off the queue frees a slot, so a batch held in
 * app_alarm for room may go. */
static void alarm_dequeue_releases_a_held_batch(void)
{
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa4);
	k_sleep(K_SECONDS(2));
	zassert_equal(fk.n, 1);
	zassert_equal(fk.log[0].kind, APP_RADIO_FRAME_ALARM);
	zassert_equal(g_alarm_flush_calls, 1, "released when the frame was taken");
	zassert_false(app_radio_tx_alarm_pending());
}
BOTH_PROFILES(alarm_dequeue_releases_a_held_batch)

/* The pages of an answer that did not fit follow page 0 by themselves, one
 * every 2 s; the stream is no action to run. */
static void page_stream_follows_page_0(void)
{
	g_cmd_resp_len = 10;
	g_cmd_action = APP_CMD_ACTION_PAGE_STREAM;
	g_stream_pages = 3;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	k_sleep(K_SECONDS(9));
	zassert_equal(fk.n, 4, "page 0 and three more");
	zassert_equal(fk.log[0].head[0], 0xa0);
	for (size_t i = 1; i < 4; i++) {
		zassert_equal(fk.log[i].kind, APP_RADIO_FRAME_ANSWER);
		zassert_equal(fk.log[i].tag, APP_RADIO_TAG_CMD_RESPONSE);
		zassert_equal(fk.log[i].head[0], 0xc0);
		zassert_equal(fk.log[i].head[1], i - 1);
	}
	assert_spacing(0, 3, 2000);
	zassert_equal(g_stream_cancel_calls, 0);
	k_sleep(K_SECONDS(8));
	zassert_equal(g_run_action_calls, 0, "PAGE_STREAM is not run");
}
BOTH_PROFILES(page_stream_follows_page_0)

/* The stream never takes the last two answer slots: an alarm answer and
 * another answer must always fit between its pages. */
static void page_stream_leaves_two_answer_slots(void)
{
	fk.ready = false;
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb1);
	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb2);
	g_cmd_resp_len = 10;
	g_cmd_action = APP_CMD_ACTION_PAGE_STREAM;
	g_stream_pages = 2;
	app_radio_downlink(m_cmd, sizeof(m_cmd)); /* page 0: one slot left */
	k_sleep(K_SECONDS(6));
	zassert_equal(g_stream_next_calls, 0, "no page while fewer than two slots are free");
	zassert_equal(app_radio_tx_answer_free(), 1);

	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(8));
	zassert_equal(fk.n, 5, "the queue drained, then both pages");
	zassert_equal(fk.log[3].head[0], 0xc0);
	zassert_equal(fk.log[4].head[0], 0xc0);
}
BOTH_PROFILES(page_stream_leaves_two_answer_slots)

/* A lost link ends the stream: the next link-up starts from scratch. */
static void page_stream_cancelled_by_a_lost_link(void)
{
	g_cmd_resp_len = 10;
	g_cmd_action = APP_CMD_ACTION_PAGE_STREAM;
	g_stream_pages = 3;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	k_sleep(K_MSEC(500));
	fk.state = APP_RADIO_STATE_JOINING;
	k_sleep(K_SECONDS(4));
	zassert_equal(g_stream_cancel_calls, 1);
	zassert_equal(g_stream_next_calls, 0);
	zassert_equal(fk.n, 1, "only page 0 left");
}
BOTH_PROFILES(page_stream_cancelled_by_a_lost_link)

/* ---- Boot/join announce (F3) ----------------------------------------------- */

/* Info, then settings-info, then the first telemetry that waited for them. */
static void announce_leads_the_first_report(void)
{
	const size_t lens[] = {20};

	frames(lens, 1);
	g_app_config.interval_report = 2; /* announce spread < 1 s */
	app_radio_announce();
	app_radio_send_telemetry(false);
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.n, 3);
	zassert_equal(fk.log[0].tag, APP_RADIO_TAG_INFO);
	zassert_equal(fk.log[1].tag, APP_RADIO_TAG_SETTINGS);
	zassert_equal(fk.log[2].kind, APP_RADIO_FRAME_TELEMETRY);
	zassert_equal(g_alarm_flush_calls, 1, "held alarms released before the report");
	zassert_false(app_radio_announce_pending());
}
BOTH_PROFILES(announce_leads_the_first_report)

/* An announce that meets a running page stream waits for its end, then goes
 * at once (the stream's end kicks it, not the 5 s retry). */
static void announce_waits_for_a_page_stream(void)
{
	g_app_config.interval_report = 2;
	g_cmd_resp_len = 10;
	g_cmd_action = APP_CMD_ACTION_PAGE_STREAM;
	g_stream_pages = 2;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	app_radio_announce();
	k_sleep(K_SECONDS(8));
	zassert_equal(fk.n, 5, "page 0, two pages, Info, settings-info");
	zassert_equal(fk.log[1].head[0], 0xc0);
	zassert_equal(fk.log[2].head[0], 0xc0);
	zassert_equal(fk.log[3].tag, APP_RADIO_TAG_INFO);
	zassert_equal(fk.log[4].tag, APP_RADIO_TAG_SETTINGS);
	zassert_true(fk.log[3].at_ms - fk.log[2].at_ms < 2500,
		     "the stream's end kicked the announce (%lld ms)",
		     (long long)(fk.log[3].at_ms - fk.log[2].at_ms));
}
BOTH_PROFILES(announce_waits_for_a_page_stream)

/* A full answer queue puts the announce on the 5 s retry, without building
 * the frames it would refuse (T2d: 470 drops in a 41 min hold). */
static void announce_retries_when_the_queue_is_full(void)
{
	g_app_config.interval_report = 2;
	fk.ready = false;
	for (uint8_t i = 0; i < APP_RADIO_TX_QUEUE_DEPTH; i++) {
		queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb0 + i);
	}
	app_radio_announce();
	k_sleep(K_SECONDS(12));
	zassert_true(app_radio_announce_pending(), "nothing fit yet");
	zassert_equal(g_announce_builds, 0, "no frame is built for a full queue (%d)",
		      g_announce_builds);

	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(7));
	zassert_equal(fk.n, APP_RADIO_TX_QUEUE_DEPTH + 2, "the answers, then the announce");
	zassert_equal(fk.log[APP_RADIO_TX_QUEUE_DEPTH].tag, APP_RADIO_TAG_INFO);
	zassert_equal(fk.log[APP_RADIO_TX_QUEUE_DEPTH + 1].tag, APP_RADIO_TAG_SETTINGS);
	zassert_false(app_radio_announce_pending());
	zassert_equal(g_announce_builds, 2, "each built once, when it fit");
}
BOTH_PROFILES(announce_retries_when_the_queue_is_full)

/* ---- History replay (F3c) --------------------------------------------------- */

#define HIST_GAP_MS 3000 /* app_radio.c HIST_FRAME_GAP_MS */

/* Records one frame holds on this profile once the frame_index bound is
 * tightened (frame counts below 128: a one-byte varint). */
static uint32_t hist_per_frame(void)
{
	return (m_prof->budget - stub_hist_overhead(0)) / STUB_HIST_REC_SIZE;
}

/* A store of 2 full frames and a short third one: [first, first + n). */
static uint32_t hist_fill(uint32_t first)
{
	uint32_t n = 2 * hist_per_frame() + 3;

	g_hist_first = first;
	g_hist_end = first + n;
	return n;
}

static uint32_t hist_recs(const struct sent *s)
{
	return (s->len - 4) / STUB_HIST_REC_SIZE;
}

/* The frames of one stream: kind, confirmation, seq, consecutive frame_index,
 * the records in order without a gap, the frame gap, and the stream's end. */
static void assert_stream(size_t from, size_t frames, uint8_t seq, uint32_t first, uint32_t n)
{
	uint32_t next = first;

	for (size_t i = from; i < from + frames; i++) {
		const struct sent *s = &fk.log[i];

		zassert_equal(s->kind, APP_RADIO_FRAME_HISTORY, "attempt %zu kind %u", i, s->kind);
		zassert_equal(s->flags, m_prof->queued_flags, "attempt %zu flags", i);
		zassert_equal(s->head[1], seq, "attempt %zu seq %u", i, s->head[1]);
		zassert_equal(s->head[2], i - from, "attempt %zu frame_index %u", i, s->head[2]);
		zassert_equal(s->head[3], frames, "attempt %zu frame_count %u", i, s->head[3]);
		zassert_equal(s->head[4], (uint8_t)next, "attempt %zu starts at record %u, want %u",
			      i, s->head[4], next);
		zassert_true(s->len <= m_prof->budget, "attempt %zu over the budget", i);
		next += hist_recs(s);
	}
	zassert_equal(next, first + n, "records sent up to %u, want %u", next, first + n);
	assert_spacing(from, from + frames - 1, HIST_GAP_MS);
	zassert_false(g_hist_replay_active, "the replay still holds the history ring");
	zassert_equal(m_ready_calls, 1, "the end kicks the report cadence once");
}

/* The window streams in order, frame_count sized with the tightened bound
 * (#409 3f: the worst-case bound would pack fewer records per frame), and the
 * frame with the last record ends it (H-4). */
static void replay_streams_the_window(void)
{
	uint32_t base = retries();
	uint32_t n = hist_fill(0);

	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	zassert_true(g_hist_replay_active, "the ring rollover is held during a replay");
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 3);
	zassert_equal(hist_recs(&fk.log[0]), hist_per_frame(), "a full first frame");
	assert_stream(0, 3, 42, 0, n);
	zassert_equal(retries(), base);
}
BOTH_PROFILES(replay_streams_the_window)

/* The cursor is an absolute ordinal (#436): a ring that evicted its oldest
 * records starts the stream at the oldest one still stored. */
static void replay_cursor_is_absolute(void)
{
	uint32_t n = hist_fill(40);

	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 5));
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 3);
	assert_stream(0, 3, 5, 40, n);
}
BOTH_PROFILES(replay_cursor_is_absolute)

/* One stream at a time: a request while one runs is answered by it (0) and
 * changes nothing -- also when it arrives from inside a frame's own send, as a
 * re-delivered P2P 0x56 does while the node waits for its Ack. */
static void replay_start_is_not_reentrant(void)
{
	uint32_t n = hist_fill(0);

	fk.replay_after = 1;
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	zassert_ok(app_radio_history_replay_start(900, 1000, 77), "a second request is answered");
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.replay_ret, 0, "the nested request is answered by the stream");
	zassert_equal(fk.n, 3, "one stream, not two interleaved");
	assert_stream(0, 3, 42, 0, n);
}
BOTH_PROFILES(replay_start_is_not_reentrant)

/* Nothing starts on a link down, an empty window, or records not one of which
 * fits the budget; nothing is sent then. */
static void replay_refused(void)
{
	fk.ready = false;
	hist_fill(0);
	zassert_equal(app_radio_history_replay_start(0, UINT32_MAX, 1), -EAGAIN);

	fk.ready = true;
	g_hist_end = g_hist_first;
	zassert_equal(app_radio_history_replay_start(0, UINT32_MAX, 1), -ENODATA);

	hist_fill(0);
	fk.budget = stub_hist_overhead(UINT32_MAX) + STUB_HIST_REC_SIZE - 1;
	zassert_equal(app_radio_history_replay_start(0, UINT32_MAX, 1), -EMSGSIZE);

	k_sleep(K_SECONDS(10));
	zassert_equal(fk.n, 0);
	zassert_false(g_hist_replay_active);
	zassert_equal(m_ready_calls, 0);
}
BOTH_PROFILES(replay_refused)

/* The replay owns the radio: the report waits for its end, alarms do not. */
static void replay_holds_telemetry_not_alarms(void)
{
	const size_t lens[] = {20};

	frames(lens, 1);
	hist_fill(0);
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	app_radio_send_telemetry_now();
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa1);
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 5);

	size_t alarm = LOG_MAX;
	size_t last_hist = 0;

	for (size_t i = 0; i < fk.n; i++) {
		if (fk.log[i].kind == APP_RADIO_FRAME_ALARM) {
			alarm = i;
		} else if (fk.log[i].kind == APP_RADIO_FRAME_HISTORY) {
			last_hist = i;
		}
	}
	zassert_true(alarm < last_hist, "the alarm waited for the replay (at %zu)", alarm);
	zassert_equal(fk.log[4].kind, APP_RADIO_FRAME_TELEMETRY, "the report goes after it");
}
BOTH_PROFILES(replay_holds_telemetry_not_alarms)

/* A refused frame is sent again, the same one, 8 times at most (#89); then
 * the replay is given up and the cadence handed back. */
static void replay_frame_retried_then_abandoned(void)
{
	uint32_t base = retries();
	hist_fill(0);
	script_fill(-EIO, LOG_MAX);
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(200));
	zassert_equal(fk.n, 9, "1 + 8 retries, got %zu", fk.n);
	for (size_t i = 0; i < fk.n; i++) {
		zassert_equal(fk.log[i].head[2], 0, "attempt %zu is not frame 0", i);
	}
	assert_spacing(0, 8, RETRY_MS);
	zassert_equal(retries(), base + 8);
	zassert_false(g_hist_replay_active);
	zassert_equal(m_ready_calls, 1);
}
BOTH_PROFILES(replay_frame_retried_then_abandoned)

/* A confirmed history frame (P2P) without its Ack goes again as it was -- the
 * same records, not rebuilt from the store -- before the stream goes on;
 * LoRaWAN sends the stream unconfirmed. */
static void replay_frame_resent_as_it_was(void)
{
	const int r[] = {-ETIMEDOUT};
	bool confirmed = (m_prof->queued_flags & APP_RADIO_FRAME_CONFIRMED) != 0;
	uint32_t n = hist_fill(0);
	size_t from = confirmed ? 1 : 0;

	script(r, ARRAY_SIZE(r));
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, from + 3);
	if (confirmed) {
		zassert_equal(fk.log[1].len, fk.log[0].len);
		zassert_mem_equal(fk.log[1].head, fk.log[0].head, sizeof(fk.log[0].head));
		zassert_equal(fk.log[1].attempt, 1);
	}
	assert_stream(from, 3, 42, 0, n);
	zassert_false(g_hist_replay_active);
	zassert_equal(m_ready_calls, 1);
}
BOTH_PROFILES(replay_frame_resent_as_it_was)

/* One confirmed frame in flight across both paths: while an alarm waits for
 * its Ack retry, the replay's next frame waits too (-EBUSY), and goes once
 * the alarm is acknowledged. */
static void replay_waits_for_a_confirmed_alarm(void)
{
	const int r[] = {-ETIMEDOUT};

	g_app_config.radio_alarm_ack = true;
	hist_fill(0);
	script(r, ARRAY_SIZE(r));
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa1);
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(15));
	zassert_equal(fk.n, 5);
	zassert_equal(fk.log[0].kind, APP_RADIO_FRAME_ALARM);
	zassert_equal(fk.log[1].kind, APP_RADIO_FRAME_ALARM, "the retry before any history frame");
	zassert_equal(fk.log[1].attempt, 1);
	zassert_equal(fk.log[2].kind, APP_RADIO_FRAME_HISTORY);
	zassert_true(fk.log[2].at_ms >= fk.log[1].at_ms);
}
BOTH_PROFILES(replay_waits_for_a_confirmed_alarm)

/* A duty-cycle hold waits res->wait_ms, then the same frame goes. */
static void replay_duty_hold_waits(void)
{
	uint32_t base = retries();
	const int r[] = {0, -EAGAIN};
	uint32_t n = hist_fill(0);

	script(r, ARRAY_SIZE(r));
	fk.wait_ms = 5000;
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 4);
	zassert_equal(fk.log[2].head[2], fk.log[1].head[2], "the held frame again");
	zassert_true(fk.log[2].at_ms - fk.log[1].at_ms >= 5000);
	zassert_true(fk.log[2].at_ms - fk.log[1].at_ms < RETRY_MS, "not the default wait");
	zassert_equal(retries(), base + 1);

	/* The stream as sent: the held attempt left out. */
	fk.log[1] = fk.log[2];
	fk.log[2] = fk.log[3];
	assert_stream(0, 3, 42, 0, n);
}
BOTH_PROFILES(replay_duty_hold_waits)

/* -EBUSY (a confirmed uplink in flight) waits for the kick, not a timer. */
static void replay_busy_waits_for_the_kick(void)
{
	uint32_t base = retries();
	const int r[] = {-EBUSY};

	hist_fill(0);
	script(r, ARRAY_SIZE(r));
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(60));
	zassert_equal(fk.n, 1, "retried without the kick");

	app_radio_tx_kick();
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 4);
	zassert_equal(fk.log[1].head[2], 0, "frame 0 again");
	zassert_equal(retries(), base, "a wait is not a failure");
}
BOTH_PROFILES(replay_busy_waits_for_the_kick)

/* The budget falls under one record mid-stream (a LoRaWAN DR drop, #409 3f):
 * the host gets BUDGET_TOO_SMALL with the request's seq instead of silence. */
static void replay_budget_drop_answers_budget_error(void)
{
	hist_fill(0);
	fk.budget_after = 1;
	fk.budget_to = stub_hist_overhead(0) + STUB_HIST_REC_SIZE - 1;
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 2);
	zassert_equal(fk.log[0].kind, APP_RADIO_FRAME_HISTORY);
	zassert_equal(fk.log[1].kind, APP_RADIO_FRAME_ANSWER);
	zassert_equal(g_budget_error_seq, 42);
	zassert_equal(fk.log[1].head[2], 42, "the Error carries the seq");
	zassert_false(g_hist_replay_active);
	zassert_equal(m_ready_calls, 1);
}
BOTH_PROFILES(replay_budget_drop_answers_budget_error)

/* Budget 0 (LoRaWAN MAC answers fill the frame, H-1): an empty frame flushes
 * the MAC and the stream goes on once the budget is back -- it used to end. */
ZTEST(radio_common, test_replay_budget_zero_flushes_and_goes_on)
{
	uint32_t base = retries();
	uint32_t n = hist_fill(0);

	fk.budget_after = 1;
	fk.budget_to = 0;
	fk.flush_budget = PROFILE_LRW.budget;
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(60));
	zassert_equal(fk.n, 4);
	zassert_equal(fk.log[1].kind, APP_RADIO_FRAME_HISTORY);
	zassert_equal(fk.log[1].len, 0, "the flush is an empty frame");
	zassert_equal(retries(), base + 1);

	fk.log[1] = fk.log[2];
	fk.log[2] = fk.log[3];
	assert_stream(0, 3, 42, 0, n);
}

/* A link lost mid-stream ends the replay (and hands the cadence back); a
 * link-up drops one still waiting to retry -- its frame would go under the
 * new session -- and leaves the kick to the link-up. */
static void replay_ends_with_the_link(void)
{
	const int r[] = {0, -ENOTCONN};

	hist_fill(0);
	fk.drop_link_after = 1;
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 1);
	zassert_false(g_hist_replay_active);
	zassert_equal(m_ready_calls, 1);

	fk.ready = true;
	fk.n = 0;
	fk.drop_link_after = 0;
	script(r, ARRAY_SIZE(r));
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 43));
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 2, "no session ends it too");
	zassert_false(g_hist_replay_active);
	zassert_equal(m_ready_calls, 2);

	fk.n = 0;
	script_fill(-EIO, 1);
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 44));
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 1);
	app_radio_link_up();
	zassert_false(g_hist_replay_active);
	k_sleep(K_SECONDS(60));
	zassert_equal(fk.n, 1, "the dropped replay's retry went out");
	zassert_equal(m_ready_calls, 2, "the link-up kicks the cadence, not the drop");
}
BOTH_PROFILES(replay_ends_with_the_link)

static size_t telemetry_frames(void)
{
	size_t n = 0;

	for (size_t i = 0; i < fk.n; i++) {
		n += fk.log[i].kind == APP_RADIO_FRAME_TELEMETRY;
	}
	return n;
}

/* T3a-F3: the end of a replay right after a report does not ask for the same
 * snapshot again; once READY_COALESCE_MS (10 s) has passed, it does. */
static void replay_end_right_after_a_report_does_not_kick(void)
{
	const size_t lens[] = {20};

	frames(lens, 1);
	hist_fill(0);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(1));
	zassert_equal(telemetry_frames(), 1);
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(8));
	zassert_false(g_hist_replay_active);
	zassert_equal(m_ready_calls, 0, "a report went < 10 s ago");

	k_sleep(K_SECONDS(10));
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 43));
	k_sleep(K_SECONDS(10));
	zassert_false(g_hist_replay_active);
	zassert_equal(m_ready_calls, 1, "> 10 s after the report: the end kicks");
}
BOTH_PROFILES(replay_end_right_after_a_report_does_not_kick)

/* T3a-F3: a report held back by the replay goes at its end, and the kick
 * folds into it instead of composing a second one. */
static void replay_end_folds_the_kick_into_a_held_report(void)
{
	const size_t lens[] = {20};

	frames(lens, 1);
	hist_fill(0);
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(1));
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(30));
	zassert_false(g_hist_replay_active);
	zassert_equal(telemetry_frames(), 1, "the held report went, once");
	zassert_equal(m_ready_calls, 0, "folded into the held report");
}
BOTH_PROFILES(replay_end_folds_the_kick_into_a_held_report)

/* M-2: a replay holds telemetry back, so its frames refresh the stale-uplink
 * clock; a long replay does not rejoin a healthy session. */
static void replay_frames_refresh_the_stale_clock(void)
{
	uint32_t per = hist_per_frame();

	app_radio_link_up();
	g_hist_first = 0;
	g_hist_end = 40 * per;             /* 40 frames, 3 s apart: 2 min */
	g_app_config.interval_report = 20; /* stale after 3 x 20 s */
	zassert_ok(app_radio_history_replay_start(0, UINT32_MAX, 42));
	k_sleep(K_SECONDS(110));
	app_radio_test_stale_tick(k_uptime_get());
	zassert_equal(fk.rejoin_calls, 0, "rejoined mid-replay");
}
BOTH_PROFILES(replay_frames_refresh_the_stale_clock)

/* ---- clock_sync (F3d) --------------------------------------------------------
 * An Info answers it: {0x01, seq} from the app_cmd_build_info_seq() stub. */

static size_t info_frames(uint8_t seq)
{
	size_t n = 0;

	for (size_t i = 0; i < fk.n && i < LOG_MAX; i++) {
		n += (fk.log[i].tag == APP_RADIO_TAG_INFO && fk.log[i].head[1] == seq) ? 1 : 0;
	}
	return n;
}

/* No fresh time: the backend is asked for one, nothing goes on air, and the
 * time event answers once with the request's seq. */
static void clock_sync_waits_for_the_time(void)
{
	app_radio_clock_sync(21);
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.time_requests, 1, "the backend is asked for a time");
	zassert_true(app_radio_clock_sync_pending());
	zassert_true(app_radio_time_wanted());
	zassert_equal(fk.n, 0, "no uplink of its own");

	app_radio_time_event();
	k_sleep(K_SECONDS(5));
	zassert_equal(info_frames(21), 1, "the time answers it");
	zassert_false(app_radio_clock_sync_pending());
	zassert_false(app_radio_time_wanted());

	app_radio_time_event();
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.n, 1, "answered once");
}
BOTH_PROFILES(clock_sync_waits_for_the_time)

/* A network time younger than 60 s answers at once, without asking (PF-2). */
static void clock_sync_fresh_time_answers_at_once(void)
{
	g_network_time_at_ms = k_uptime_get();
	app_radio_clock_sync(33);
	k_sleep(K_SECONDS(5));
	zassert_equal(info_frames(33), 1);
	zassert_equal(fk.time_requests, 0, "a fresh time needs no request");
	zassert_false(app_radio_clock_sync_pending());
}
BOTH_PROFILES(clock_sync_fresh_time_answers_at_once)

/* An older time is not fresh: the request waits for a new one. */
static void clock_sync_stale_time_asks(void)
{
	if (k_uptime_get() < 62 * MSEC_PER_SEC) {
		k_sleep(K_SECONDS(62));
	}
	g_network_time_at_ms = k_uptime_get() - 61 * MSEC_PER_SEC;
	app_radio_clock_sync(34);
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.time_requests, 1);
	zassert_equal(fk.n, 0, "a 61 s old time does not answer");
	zassert_true(app_radio_clock_sync_pending());
}
BOTH_PROFILES(clock_sync_stale_time_asks)

/* A request answered from a fresh time also ends an older pending one, so the
 * time event that follows (a LoRaWAN DeviceTimeAns lands in the callback
 * before app_radio_time_event() runs) answers nothing twice. */
static void clock_sync_fresh_answer_ends_a_pending_one(void)
{
	app_radio_clock_sync(7);
	k_sleep(K_MSEC(100));
	zassert_true(app_radio_clock_sync_pending());

	g_network_time_at_ms = k_uptime_get();
	app_radio_clock_sync(8);
	k_sleep(K_SECONDS(5));
	zassert_equal(info_frames(8), 1, "the fresh time answers the newer request");
	zassert_false(app_radio_clock_sync_pending(), "and ends the older one");

	app_radio_time_event();
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.n, 1, "no second answer");
}
BOTH_PROFILES(clock_sync_fresh_answer_ends_a_pending_one)

/* A newer request before the time lands takes over the seq: one answer. */
static void clock_sync_newer_request_takes_over(void)
{
	app_radio_clock_sync(5);
	k_sleep(K_MSEC(100));
	app_radio_clock_sync(6);
	k_sleep(K_MSEC(100));
	zassert_equal(fk.time_requests, 2, "each request asks the backend");

	app_radio_time_event();
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.n, 1, "one answer");
	zassert_equal(info_frames(6), 1, "with the newer seq");
}
BOTH_PROFILES(clock_sync_newer_request_takes_over)

/* A time with nothing pending answers nothing. */
static void time_event_without_a_request(void)
{
	app_radio_time_event();
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.n, 0);
}
BOTH_PROFILES(time_event_without_a_request)

/* ---- Network time request ---------------------------------------------------
 * app_radio asks the backend (LoRaWAN DeviceTimeReq, P2P TIME_REQ), whoever
 * wants the time: a link-up without one, the weekly re-sync, `clock sync`. */

/* A link-up with no network time since boot asks with the session's first
 * uplinks; the landed time ends the want and answers nothing. */
static void link_up_without_time_asks(void)
{
	app_radio_link_up();
	zassert_equal(fk.time_requests, 1, "the new session asks for the time");
	zassert_true(app_radio_time_wanted());
	zassert_false(app_radio_clock_sync_pending(), "no clock_sync to answer");

	app_radio_time_event();
	zassert_false(app_radio_time_wanted(), "the time landed");
	k_sleep(K_SECONDS(5));
	zassert_equal(fk.n, 0, "nothing to answer, no uplink of its own");
}
BOTH_PROFILES(link_up_without_time_asks)

/* A link-up after a network time (a rejoin later in the boot) keeps it; the
 * weekly re-sync refreshes it. */
static void link_up_with_time_keeps_it(void)
{
	g_network_time_at_ms = 1;
	app_radio_link_up();
	zassert_equal(fk.time_requests, 0);
	zassert_false(app_radio_time_wanted());
}
BOTH_PROFILES(link_up_with_time_keeps_it)

/* app_radio_time_request() (weekly re-sync, shell `clock sync`), from any
 * thread: the backend is asked on the radio work queue, even with a time. */
static void time_request_asks_the_backend(void)
{
	g_network_time_at_ms = 1;
	app_radio_time_request();
	k_sleep(K_MSEC(100));
	zassert_equal(fk.time_requests, 1);
	zassert_true(app_radio_time_wanted());
	zassert_equal(fk.n, 0, "no uplink of its own");

	app_radio_time_event();
	zassert_false(app_radio_time_wanted());
}
BOTH_PROFILES(time_request_asks_the_backend)

/* ---- Flash writes vs radio exchanges ----------------------------------------
 * A writer thread (shell, NFC, report queue in the firmware) holds the flash
 * for `hold_ms`; the test thread plays the radio work queue's exchanges. */

static K_THREAD_STACK_DEFINE(m_writer_stack, 1024);
static struct k_thread m_writer;
static volatile int64_t m_writer_held_at;
static volatile int64_t m_writer_released_at;
static int32_t m_writer_hold_ms;

static void writer_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	app_radio_flash_hold();
	m_writer_held_at = k_uptime_get();
	if (m_writer_hold_ms > 0) {
		k_sleep(K_MSEC(m_writer_hold_ms));
	}
	m_writer_released_at = k_uptime_get();
	app_radio_flash_release();
}

static void writer_start(int32_t hold_ms)
{
	m_writer_held_at = -1;
	m_writer_released_at = -1;
	m_writer_hold_ms = hold_ms;
	k_thread_create(&m_writer, m_writer_stack, K_THREAD_STACK_SIZEOF(m_writer_stack), writer_fn,
			NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
}

static void writer_join(void)
{
	zassert_ok(k_thread_join(&m_writer, K_SECONDS(30)), "writer thread stuck");
}

ZTEST(radio_common, test_flash_write_waits_for_the_exchange)
{
	app_radio_air_begin();
	int64_t t0 = k_uptime_get();

	writer_start(0);
	k_sleep(K_MSEC(500));
	zassert_equal(m_writer_held_at, -1, "no write while the radio is on air");

	app_radio_air_end();
	k_sleep(K_MSEC(10));
	zassert_true(m_writer_held_at >= t0 + 500, "the write follows the exchange");
	writer_join();
}

ZTEST(radio_common, test_exchange_waits_for_a_running_write)
{
	writer_start(300);
	k_sleep(K_MSEC(10));
	zassert_not_equal(m_writer_held_at, -1, "an idle radio lets the write go at once");

	app_radio_air_begin();
	zassert_not_equal(m_writer_released_at, -1, "no TX before the write ended");
	zassert_true(k_uptime_get() >= m_writer_held_at + 300);
	app_radio_air_end();
	writer_join();
}

/* A busy radio cannot starve a writer: the next exchange lets the one already
 * waiting write first. */
ZTEST(radio_common, test_waiting_write_goes_before_the_next_exchange)
{
	app_radio_air_begin();
	writer_start(100);
	k_sleep(K_MSEC(50));
	app_radio_air_end();

	app_radio_air_begin(); /* the next frame, straight away */
	zassert_not_equal(m_writer_released_at, -1, "the waiting write went first");
	app_radio_air_end();
	writer_join();
}

ZTEST(radio_common, test_flash_write_waits_10_s_at_most)
{
	app_radio_air_begin();
	int64_t t0 = k_uptime_get();

	writer_start(0);
	k_sleep(K_MSEC(9900));
	zassert_equal(m_writer_held_at, -1, "still waiting at 9.9 s");
	k_sleep(K_MSEC(200));
	zassert_true(m_writer_held_at >= t0 + 10000 && m_writer_held_at <= t0 + 10100,
		     "the write goes ahead at 10 s (%lld ms)", m_writer_held_at - t0);
	app_radio_air_end();
	writer_join();
}

ZTEST(radio_common, test_exchange_waits_1_s_at_most)
{
	writer_start(5000);
	k_sleep(K_MSEC(10));

	int64_t t0 = k_uptime_get();

	app_radio_air_begin();

	int64_t waited = k_uptime_get() - t0;

	zassert_true(waited >= 1000 && waited <= 1100, "TX goes ahead at 1 s (%lld ms)", waited);
	zassert_equal(m_writer_released_at, -1, "the write is still running");
	app_radio_air_end();
	writer_join();
}

/* The radio work queue runs the exchange itself, the system one LoRaMacProcess()
 * and the DIO1 work: a write there goes ahead even on air. */
static volatile int64_t m_wq_write_at;

static void wq_write_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	app_radio_flash_hold();
	m_wq_write_at = k_uptime_get();
	app_radio_flash_release();
}

static K_WORK_DEFINE(m_radio_wq_write, wq_write_handler);
static K_WORK_DEFINE(m_sys_wq_write, wq_write_handler);

ZTEST(radio_common, test_work_queue_writes_never_wait_for_the_air)
{
	app_radio_air_begin();

	int64_t t0 = k_uptime_get();

	m_wq_write_at = -1;
	k_work_submit_to_queue(app_radio_work_q(), &m_radio_wq_write);
	k_sleep(K_MSEC(20));
	zassert_true(m_wq_write_at >= t0 && m_wq_write_at <= t0 + 20, "radio queue wrote at once");

	m_wq_write_at = -1;
	k_work_submit(&m_sys_wq_write);
	k_sleep(K_MSEC(20));
	zassert_true(m_wq_write_at >= t0 && m_wq_write_at <= t0 + 40, "system queue wrote at once");
	app_radio_air_end();
}

/* ---- Duty ledger (T2d) ---------------------------------------------------- */

/* The property under test is stronger than a token bucket's: not "the
 * long-run average is 1%" but "EVERY sliding one-hour window stays within the
 * allowance". The pure tests drive a ledger on a virtual clock, so an hour
 * costs no time. */

#define WINDOW APP_RADIO_DUTY_WINDOW_MS
#define PCT1   APP_RADIO_DUTY_1PCT_MS
#define SLOT   APP_RADIO_DUTY_SLOT_MS

ZTEST(radio_common, test_duty_empty_ledger_admits)
{
	struct app_radio_duty d;

	app_radio_ledger_init(&d, PCT1);

	/* Boot is never blocked, and the whole allowance is available at once. */
	zassert_equal(app_radio_ledger_wait_ms(&d, 0, 1), 0,
		      "an empty ledger admits a small frame");
	zassert_equal(app_radio_ledger_wait_ms(&d, 0, PCT1), 0,
		      "an empty ledger admits the whole hourly allowance");
}

ZTEST(radio_common, test_duty_sum_enforced)
{
	struct app_radio_duty d;
	const uint32_t air = PCT1 / 4; /* 9 s: four fill the hour */

	app_radio_ledger_init(&d, PCT1);
	for (int i = 0; i < 4; i++) {
		int64_t now = i * 1000;

		zassert_equal(app_radio_ledger_wait_ms(&d, now, air), 0, "frame %d is admitted", i);
		app_radio_ledger_charge(&d, now, air);
	}

	/* The allowance is exactly spent -- not one further millisecond of air. */
	zassert_true(app_radio_ledger_wait_ms(&d, 4000, 1) > 0,
		     "1 ms of air is refused once the allowance is spent");
	zassert_equal(app_radio_ledger_wait_ms(&d, 4000, 0), 0, "no air is always affordable");
	zassert_equal(app_radio_ledger_used_ms(&d, 4000), PCT1);
}

ZTEST(radio_common, test_duty_expiry_after_hour)
{
	struct app_radio_duty d;

	app_radio_ledger_init(&d, PCT1);
	app_radio_ledger_charge(&d, 0, PCT1); /* spend it all at t=0 */

	zassert_true(app_radio_ledger_wait_ms(&d, 1000, 1) > 0, "still held one second in");

	/* One ms before the entry leaves the window: still held, and the wait
	 * is exactly the time left. */
	int64_t wait = app_radio_ledger_wait_ms(&d, WINDOW - 1, 1);

	zassert_equal(wait, 1, "wait is 1 ms at the window edge, got %lld", wait);

	/* The instant it does, the full allowance is back. */
	zassert_equal(app_radio_ledger_wait_ms(&d, WINDOW, PCT1), 0,
		      "the allowance returns when the entry leaves the window");
	zassert_equal(app_radio_ledger_used_ms(&d, WINDOW), 0);
}

/* A status read does not expire entries first (it only reads), so used_ms()
 * itself must leave out the ones the window has passed. */
ZTEST(radio_common, test_duty_used_skips_expired_without_expiry)
{
	struct app_radio_duty d;

	app_radio_ledger_init(&d, PCT1);
	app_radio_ledger_charge(&d, 0, 700);
	app_radio_ledger_charge(&d, SLOT, 300); /* the next slot: its own entry */

	zassert_equal(app_radio_ledger_used_ms(&d, WINDOW - 1), 1000);
	zassert_equal(app_radio_ledger_used_ms(&d, WINDOW), 300, "the first entry has left");
	zassert_equal(app_radio_ledger_used_ms(&d, WINDOW + SLOT), 0, "both have left");
	zassert_equal(d.count, 2, "a read never drops entries");
}

/* The wait frees just enough: the oldest entries until the frame fits, so
 * the frame goes on its first wake-up, and not one ms earlier. */
ZTEST(radio_common, test_duty_wait_is_exact)
{
	struct app_radio_duty d;

	app_radio_ledger_init(&d, PCT1);
	for (int i = 0; i < 3; i++) {
		/* One frame per slot: 30 s used, 6 s left. */
		app_radio_ledger_charge(&d, i * SLOT, 10000);
	}

	const int64_t now = 2 * SLOT + 1000;

	/* 16 s of air is 10 s over: the oldest entry alone frees that. */
	int64_t w1 = app_radio_ledger_wait_ms(&d, now, 16000);

	zassert_equal(w1, WINDOW - now, "waits for the oldest entry, got %lld", w1);

	/* 26 s is 20 s over: the two oldest, so until the second leaves. */
	int64_t w2 = app_radio_ledger_wait_ms(&d, now, 26000);

	zassert_equal(w2, WINDOW - (now - SLOT), "waits for the second entry, got %lld", w2);
	zassert_true(app_radio_ledger_wait_ms(&d, now + w2 - 1, 26000) > 0, "not a ms early");
	zassert_equal(app_radio_ledger_wait_ms(&d, now + w2, 26000), 0, "one wait is enough");
}

/* A frame alone over the allowance (no supported PHY setting makes one at
 * 1 %) waits until the ledger is empty, then goes: never held for ever. */
ZTEST(radio_common, test_duty_frame_over_allowance_waits_for_empty)
{
	struct app_radio_duty d;

	app_radio_ledger_init(&d, PCT1);
	app_radio_ledger_charge(&d, 0, 1000);
	app_radio_ledger_charge(&d, 5000, 1000);

	int64_t wait = app_radio_ledger_wait_ms(&d, 6000, PCT1 + 1);

	zassert_equal(wait, WINDOW - 1000, "waits for the newest entry, got %lld", wait);
	zassert_equal(app_radio_ledger_wait_ms(&d, 5000 + WINDOW, PCT1 + 1), 0,
		      "an empty ledger lets it go");
}

/* Frames of one slot share an entry, and it ends with the last of them: the
 * first frame's air is held until the last one's leaves the window -- an
 * over-count of under a slot, never an under-count. */
ZTEST(radio_common, test_duty_slot_coalesces_conservatively)
{
	struct app_radio_duty d;

	app_radio_ledger_init(&d, PCT1);
	app_radio_ledger_charge(&d, 0, 100);
	app_radio_ledger_charge(&d, SLOT - 1, 200); /* same slot */
	zassert_equal(d.count, 1, "one entry per slot");
	app_radio_ledger_charge(&d, SLOT, 400); /* the next slot */
	zassert_equal(d.count, 2);

	zassert_equal(app_radio_ledger_used_ms(&d, WINDOW), 700,
		      "the first frame's air still counts after its own hour");
	zassert_equal(app_radio_ledger_used_ms(&d, WINDOW + SLOT - 1), 400,
		      "the slot leaves with its last frame");
}

/* A slot on a 10 % sub-band sums past 65 s in one entry: no clamp, or the
 * ledger would forget air. */
ZTEST(radio_common, test_duty_slot_10pct_keeps_all_air)
{
	struct app_radio_duty d;
	const uint32_t budget = app_radio_duty_budget_ms(869525000);

	zassert_equal(budget, 10 * PCT1);
	app_radio_ledger_init(&d, budget);

	/* 62 frames of 5 s, all in the first slot. */
	for (int i = 0; i < 62; i++) {
		zassert_equal(app_radio_ledger_wait_ms(&d, i * 100, 5000), 0, "frame %d", i);
		app_radio_ledger_charge(&d, i * 100, 5000);
	}
	zassert_equal(d.count, 1);
	zassert_equal(d.entries[d.head].air_ms, 310000u, "the slot holds 310 s");
	zassert_equal(app_radio_ledger_used_ms(&d, 6200), 62u * 5000u, "no air is lost");

	/* 1 ms over the allowance: the slot (last frame at 6.1 s) has to leave. */
	uint32_t over = budget - 62u * 5000u + 1;

	zassert_equal(app_radio_ledger_wait_ms(&d, 6200, over), WINDOW - 100,
		      "waits for the slot's last frame");
}

/* The TOWER bench cadence (2026-10-07): a 60 s report plus a link check every
 * 5th and the odd control frame -- 72 frames/h of 78 ms, 5.6 s of the 36 s
 * allowance -- may never wait. The per-frame ring folded at > 48 frames/h
 * into an entry that never aged out, and blocked ~21 min every ~6 h. */
ZTEST(radio_common, test_duty_72_frames_per_hour_never_waits)
{
	struct app_radio_duty d;

	app_radio_ledger_init(&d, PCT1);
	for (int64_t now = 0; now <= 24 * (int64_t)WINDOW; now += 50000) {
		zassert_equal(app_radio_ledger_wait_ms(&d, now, 78), 0,
			      "frame at t=%lld s waits -- only air-time may refuse", now / 1000);
		app_radio_ledger_charge(&d, now, 78);
		zassert_true(d.count <= APP_RADIO_DUTY_LEDGER_ENTRIES);
	}
	zassert_true(app_radio_ledger_used_ms(&d, 24 * (int64_t)WINDOW) <= 73u * 78u,
		     "an hour's worth of air, not a day's");
}

/* Deterministic pseudo-random frame sizes: a failure has to be reproducible. */
static uint32_t duty_rand(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/* Static, not on the stack: hours of sends are a few thousand records. */
static struct {
	uint32_t end_ms;
	uint32_t air_ms;
} m_sent[4096];

/* Hammer a ledger for `hours` simulated hours, always trying to send frames
 * of 330..2296 ms (the SF10 range), and check every sliding hour against the
 * record rather than the ledger's own arithmetic. */
static void duty_hammer(uint32_t budget, int hours)
{
	struct app_radio_duty d;
	uint32_t state = 0xC0FFEEu;
	size_t n = 0;

	app_radio_ledger_init(&d, budget);
	for (int64_t now = 0; now <= hours * (int64_t)WINDOW; now += 1000) {
		uint32_t air = 330 + (duty_rand(&state) % 1967);

		while (app_radio_ledger_wait_ms(&d, now, air) == 0) {
			app_radio_ledger_charge(&d, now, air);
			zassert_true(n < ARRAY_SIZE(m_sent), "test record overflow");
			m_sent[n].end_ms = (uint32_t)now;
			m_sent[n].air_ms = air;
			n++;
			air = 330 + (duty_rand(&state) % 1967);
		}
	}
	zassert_true(n > 100, "the simulation sent plenty, got %zu", n);

	/* For every transmission, the air of the hour ending at it -- itself
	 * included -- is within the allowance. */
	for (size_t j = 0; j < n; j++) {
		uint32_t sum = 0;

		for (size_t i = 0; i <= j; i++) {
			if (m_sent[j].end_ms - m_sent[i].end_ms < WINDOW) {
				sum += m_sent[i].air_ms;
			}
		}
		zassert_true(sum <= budget,
			     "the hour ending at send %zu (t=%u) radiated %u ms > %u", j,
			     m_sent[j].end_ms, sum, budget);
	}
}

ZTEST(radio_common, test_duty_sliding_hour_never_exceeds_1pct)
{
	duty_hammer(PCT1, 24);
}

ZTEST(radio_common, test_duty_sliding_hour_never_exceeds_10pct)
{
	duty_hammer(10 * PCT1, 6);
}

/* A stream faster than a slot (a 10 s HIL stream, a history replay) asking
 * for twice the allowance: every frame the ledger admits keeps the sliding
 * hour within it, and every wait it gives is the exact one -- computed from
 * the record of what went -- plus at most a slot. Moving one entry's end with
 * each frame would chain the stream into one entry that never left. */
ZTEST(radio_common, test_duty_fast_stream_waits_near_exact)
{
	struct app_radio_duty d;
	const uint32_t air = 200; /* 72 s of demand an hour */
	size_t n = 0;
	int held = 0;

	app_radio_ledger_init(&d, PCT1);
	for (int64_t now = 0; now <= 2 * (int64_t)WINDOW; now += 10000) {
		/* The exact wait: free the oldest recorded frames still in the
		 * hour until this one fits. */
		uint32_t used = 0;
		size_t first = n;

		for (size_t i = 0; i < n; i++) {
			if ((uint32_t)now - m_sent[i].end_ms < WINDOW) {
				used += m_sent[i].air_ms;
				first = MIN(first, i);
			}
		}

		int64_t exact = 0;

		if (used + air > PCT1) {
			uint32_t freed = 0;
			size_t i = first;

			for (; i < n - 1; i++) {
				freed += m_sent[i].air_ms;
				if (freed >= used + air - PCT1) {
					break;
				}
			}
			exact = WINDOW - ((uint32_t)now - m_sent[i].end_ms);
		}

		int64_t wait = app_radio_ledger_wait_ms(&d, now, air);

		zassert_true(wait >= exact && wait <= exact + SLOT,
			     "t=%lld s: wait %lld vs exact %lld", now / 1000, wait, exact);
		if (wait == 0) {
			zassert_true(used + air <= PCT1, "t=%lld s: admitted over the allowance",
				     now / 1000);
			app_radio_ledger_charge(&d, now, air);
			zassert_true(n < ARRAY_SIZE(m_sent), "test record overflow");
			m_sent[n].end_ms = (uint32_t)now;
			m_sent[n].air_ms = air;
			n++;
		} else {
			held++;
		}
	}
	zassert_true(n >= 3 * PCT1 / air / 2, "the allowance is used, sent %zu frames", n);
	zassert_true(held > 0, "the stream asked for more than the allowance");
}

/* A burst that spends the whole allowance, then quiet: the next frame goes no
 * later than an hour and a slot after the burst's last frame. */
ZTEST(radio_common, test_duty_burst_unblocks_within_hour_and_slot)
{
	struct app_radio_duty d;
	int64_t now = 70000; /* the burst straddles a slot edge (75 s) */

	app_radio_ledger_init(&d, PCT1);
	for (int i = 0; i < 36; i++) {
		zassert_equal(app_radio_ledger_wait_ms(&d, now, 1000), 0, "burst frame %d", i);
		now += 1000;
		app_radio_ledger_charge(&d, now, 1000);
	}

	const int64_t last = now;
	int64_t wait = app_radio_ledger_wait_ms(&d, last + 1, 1);

	zassert_true(wait > 0, "the allowance is spent");
	zassert_true(last + 1 + wait <= last + WINDOW + SLOT, "unblocked %lld ms after the burst",
		     wait + 1);
	zassert_true(app_radio_ledger_wait_ms(&d, last + 1 + wait, 1) == 0, "one wait is enough");
	zassert_equal(app_radio_ledger_wait_ms(&d, last + WINDOW, PCT1), 0,
		      "the whole allowance is back an hour after the burst");
}

/* Uptime is truncated to 32 bits in the ledger, so entries survive the
 * ~49.7-day wrap: a `now >= end_ms + WINDOW` formulation breaks here, the
 * unsigned-difference one does not. */
ZTEST(radio_common, test_duty_wrap_safe)
{
	struct app_radio_duty d;
	const int64_t t = 0xFFFFFF00LL; /* 256 ms before the u32 wrap */

	app_radio_ledger_init(&d, PCT1);
	app_radio_ledger_charge(&d, t, PCT1);

	int64_t wait = app_radio_ledger_wait_ms(&d, t + 1000, 1);

	zassert_equal(wait, WINDOW - 1000, "wait across the wrap: %lld", wait);
	zassert_equal(app_radio_ledger_wait_ms(&d, t + WINDOW, PCT1), 0,
		      "the entry expires on schedule across the wrap");
}

/* No limit (budget 0: LoRaWAN outside EU868) never holds, and still counts. */
ZTEST(radio_common, test_duty_no_limit_counts_only)
{
	struct app_radio_duty d;

	app_radio_ledger_init(&d, 0);
	app_radio_ledger_charge(&d, 0, 10 * WINDOW / 100);
	zassert_equal(app_radio_ledger_wait_ms(&d, 1000, PCT1), 0);
	zassert_equal(app_radio_ledger_used_ms(&d, 1000), 10 * WINDOW / 100);
}

/* The EU868 sub-bands of LoRaMac's RegionEU868 (ETSI EN 300 220). */
ZTEST(radio_common, test_duty_budget_by_sub_band)
{
	static const struct {
		uint32_t hz;
		uint32_t ms;
	} t[] = {
		{863000000, 3600},   {864999999, 3600},  {865000000, 36000},  {867100000, 36000},
		{868100000, 36000},  {868600000, 36000}, {868650000, 3600},   {868700000, 3600},
		{869200000, 3600},   {869300000, 3600},  {869400000, 360000}, {869525000, 360000},
		{869650000, 360000}, {869700000, 36000}, {870000000, 36000},  {870000001, 3600},
		{915000000, 3600},
	};

	for (size_t i = 0; i < ARRAY_SIZE(t); i++) {
		zassert_equal(app_radio_duty_budget_ms(t[i].hz), t[i].ms, "%u Hz: %u ms, want %u",
			      t[i].hz, app_radio_duty_budget_ms(t[i].hz), t[i].ms);
	}
}

/* LoRaMac's time on air (RadioTimeOnAir), which both radios charge. */
ZTEST(radio_common, test_lora_time_on_air)
{
	zassert_equal(app_radio_lora_toa_ms(12, 125000, 23), 1483, "EU868 DR0 JoinRequest");
	zassert_equal(app_radio_lora_toa_ms(7, 125000, 23), 62, "EU868 DR5 JoinRequest");
	zassert_equal(app_radio_lora_toa_ms(7, 250000, 23), 31, "EU868 DR6");
	zassert_equal(app_radio_lora_toa_ms(8, 500000, 23), 29, "US915 DR4");
	zassert_equal(app_radio_lora_toa_ms(12, 125000, 64), 2794, "DR0, a 51 B payload");
	zassert_equal(app_radio_lora_toa_ms(12, 125000, 255), 9020, "SF12 maximum");
	zassert_equal(app_radio_lora_toa_ms(10, 125000, 56), 658, "P2P SF10, 40 B body");
	zassert_equal(app_radio_lora_toa_ms(6, 125000, 20), 34, "SF6: 12 symbol preamble");
	zassert_equal(app_radio_lora_toa_ms(12, 125000, 300), 9020, "over 255 B: capped");
}

/* A frame the ledger has no room for is held until it fits, then sent: no
 * send on the backend meanwhile, no retry counted, and the status shows it. */
static void duty_hold_sends_when_it_fits(void)
{
	struct app_radio_status st;
	uint32_t base = retries();

	app_radio_duty_init(PCT1);
	fk.air_ms = 1000;
	/* A full hour's allowance that leaves the window in 2 s. */
	app_radio_test_duty_charge_at(k_uptime_get() - WINDOW + 2000, PCT1);

	int64_t t0 = k_uptime_get();

	queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xd1);
	k_sleep(K_MSEC(1000));
	zassert_equal(fk.n, 0, "held: the backend sends nothing");
	app_radio_get_status(&st);
	zassert_true(st.duty_blocked_s > 0, "the hold is visible");
	zassert_equal(st.airtime_hour_ms, PCT1, "the full hour is shown");

	k_sleep(K_MSEC(1500));
	zassert_equal(fk.n, 1, "sent once it fits");
	zassert_equal(fk.log[0].head[0], 0xd1);
	zassert_true(fk.log[0].at_ms - t0 >= 2000, "not before the entry left (%lld ms)",
		     fk.log[0].at_ms - t0);
	zassert_equal(retries(), base, "a hold is not a retry");

	app_radio_get_status(&st);
	zassert_true(st.has_airtime);
	zassert_equal(st.airtime_hour_ms, 1000, "the old hour left, the new frame counts");
	zassert_equal(st.duty_blocked_s, 0, "the send ended the hold");
}
BOTH_PROFILES(duty_hold_sends_when_it_fits)

/* The report path is held the same way, and its frames are charged. */
static void duty_hold_report(void)
{
	const size_t lens[] = {10, 10};

	app_radio_duty_init(PCT1);
	fk.air_ms = 1000;
	app_radio_test_duty_charge_at(k_uptime_get() - WINDOW + 2000, PCT1 - 500);

	/* 500 ms left: the first frame (1000 ms) waits for the old entry. */
	frames(lens, 2);
	app_radio_send_telemetry_now();
	k_sleep(K_MSEC(1500));
	zassert_equal(fk.n, 0, "the report is held");
	k_sleep(K_MSEC(5000));
	zassert_equal(fk.n, 2, "both frames went once the old air left");

	struct app_radio_status st;

	app_radio_get_status(&st);
	zassert_equal(st.airtime_hour_ms, 2000, "both frames are charged");
}
BOTH_PROFILES(duty_hold_report)

/* Without a limit nothing is held, whatever the air. */
static void duty_no_limit_never_holds(void)
{
	app_radio_duty_init(0);
	fk.air_ms = PCT1;
	for (int i = 0; i < 3; i++) {
		queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xe0 + i);
	}
	k_sleep(K_SECONDS(12));
	zassert_equal(fk.n, 3, "all three went");

	struct app_radio_status st;

	app_radio_get_status(&st);
	zassert_equal(st.airtime_hour_ms, 3 * PCT1, "the air is still shown");
}
BOTH_PROFILES(duty_no_limit_never_holds)
