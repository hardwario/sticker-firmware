/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The common uplink path of app_radio.c (doc/plan/460 F4) against a fake
 * backend. The scenarios that both radios share run twice: once with a
 * LoRaWAN-like backend (nothing sent confirmed, 3 s between the frames of a
 * report, 51 B budget) and once with a P2P-like one (answers, alarms and
 * history confirmed, no frame gap, 239 B budget) -- one implementation, the
 * same behaviour on either radio (decision #23).
 */

#include "stubs.h"

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
	uint8_t head[4];
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
	bool replay;
	uint8_t report_flags;
	int report_flags_calls;
	int report_done_calls;
	/* After this many send() calls (0 = never): */
	size_t drop_link_after;
	size_t zero_budget_after;
	size_t request_after;
	size_t queue_after;
	struct sent log[LOG_MAX];
	size_t n;
} fk;

static int fake_send(const struct app_radio_frame *f, struct app_radio_tx_result *res)
{
	int ret = fk.n < fk.n_script ? fk.script[fk.n] : 0;

	if (fk.n < LOG_MAX) {
		struct sent *s = &fk.log[fk.n];

		s->kind = f->kind;
		s->tag = f->tag;
		s->port = f->port;
		s->flags = f->flags;
		s->len = f->len;
		memcpy(s->head, f->buf, MIN(f->len, sizeof(s->head)));
		s->ret = ret;
		s->at_ms = k_uptime_get();
	}
	fk.n++;
	res->wait_ms = fk.wait_ms;
	res->budget = fk.res_budget;

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

static uint8_t fake_report_flags(void)
{
	fk.report_flags_calls++;
	return fk.report_flags;
}

static void fake_report_done(void)
{
	fk.report_done_calls++;
}

static bool fake_replay_active(void)
{
	return fk.replay;
}

static const struct app_radio_backend be_lrw = {
	.send = fake_send,
	.budget = fake_budget,
	.tx_ready = fake_tx_ready,
	.report_flags = fake_report_flags,
	.report_done = fake_report_done,
	.replay_active = fake_replay_active,
	.confirm_kinds = 0,
	.frame_gap_ms = 3000,
};

static const struct app_radio_backend be_p2p = {
	.send = fake_send,
	.budget = fake_budget,
	.tx_ready = fake_tx_ready,
	.report_flags = fake_report_flags,
	.report_done = fake_report_done,
	.replay_active = fake_replay_active,
	.confirm_kinds = BIT(APP_RADIO_FRAME_ANSWER) | BIT(APP_RADIO_FRAME_ALARM) |
			 BIT(APP_RADIO_FRAME_HISTORY),
	.frame_gap_ms = 0,
};

struct profile {
	const struct app_radio_backend *be;
	uint8_t budget;
	uint8_t queued_flags; /* flags of a queued answer or alarm */
};

static const struct profile PROFILE_LRW = {&be_lrw, 51, 0};
static const struct profile PROFILE_P2P = {&be_p2p, 239, APP_RADIO_FRAME_CONFIRMED};
static const struct profile *m_prof;

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

static void assert_spacing(size_t from, size_t to, int64_t min_ms)
{
	for (size_t i = from + 1; i <= to; i++) {
		zassert_true(fk.log[i].at_ms - fk.log[i - 1].at_ms >= min_ms,
			     "attempt %zu came %lld ms after the one before, want >= %lld", i,
			     (long long)(fk.log[i].at_ms - fk.log[i - 1].at_ms), (long long)min_ms);
	}
}

static void before(void *f)
{
	ARG_UNUSED(f);
	app_radio_test_tx_reset();
	stubs_reset();
	memset(&fk, 0, sizeof(fk));
	fk.ready = true;
	memset(&g_app_config, 0, sizeof(g_app_config));
	g_app_config.interval_report = 60;
	use_profile(&PROFILE_LRW);
}

static void after(void *f)
{
	ARG_UNUSED(f);
	fk.ready = false;
	k_sleep(K_MSEC(10)); /* a report request still on the system work queue */
	app_radio_test_tx_reset();
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
	zassert_equal(fk.log[1].flags, m_prof->queued_flags);
	zassert_equal(fk.log[2].kind, APP_RADIO_FRAME_TELEMETRY);
	zassert_equal(fk.log[2].len, 20);
	zassert_equal(fk.log[2].flags, 0);
	zassert_equal(g_compose_last_budget, m_prof->budget);
	zassert_equal(fk.report_flags_calls, 1);
	zassert_equal(fk.report_done_calls, 1);
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
	zassert_equal(fk.report_done_calls, 1);
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

/* -EBUSY (a confirmed uplink in flight) and -ENOTCONN (no session) keep the
 * frame until the backend kicks. */
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
 * backend's frame gap separates them, and report_flags() / report_done() run
 * once per report. */
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
	zassert_equal(fk.report_done_calls, 1);
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
	zassert_equal(fk.report_done_calls, 1);
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
	zassert_equal(fk.report_done_calls, 2);
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
	zassert_equal(fk.report_done_calls, 0);

	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 10);
	zassert_ok(fk.log[9].ret);
	zassert_equal(fk.report_done_calls, 1);
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
	zassert_equal(fk.report_done_calls, 0);
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
	zassert_equal(fk.report_done_calls, 1);
	zassert_equal(g_compose_reset_calls, 0);
}
BOTH_PROFILES(telemetry_over_budget_frame_is_skipped)

/* ... the last frame included: no report_done() for a report that did not
 * fully leave, but no snapshot reset either. */
ZTEST(radio_common, test_telemetry_over_budget_last_frame)
{
	const size_t lens[] = {20, 30};
	const int r[] = {0, -EMSGSIZE};

	frames(lens, 2);
	script(r, 2);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 2);
	zassert_equal(fk.report_done_calls, 0);
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
	zassert_equal(fk.report_done_calls, 0);
	zassert_equal(g_compose_reset_calls, 0, "nothing of the snapshot left yet");

	fk.budget = m_prof->budget;
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 2);
	zassert_equal(fk.log[1].len, 20);
	zassert_equal(fk.report_done_calls, 1);
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
	zassert_equal(fk.report_done_calls, 0);
}

/* A history replay owns the radio: the report waits for its end (the kick),
 * alarms do not. */
static void replay_holds_telemetry_not_alarms(void)
{
	const size_t lens[] = {20};

	frames(lens, 1);
	fk.replay = true;
	app_radio_send_telemetry_now();
	queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 8, 0xa1);
	k_sleep(K_SECONDS(10));
	zassert_equal(fk.n, 1);
	zassert_equal(fk.log[0].kind, APP_RADIO_FRAME_ALARM);

	fk.replay = false;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 2);
	zassert_equal(fk.log[1].kind, APP_RADIO_FRAME_TELEMETRY);
}
BOTH_PROFILES(replay_holds_telemetry_not_alarms)

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
	zassert_equal(fk.report_done_calls, 0);

	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(10));
	zassert_equal(fk.n, 1, "the abandoned report was continued");

	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(30));
	zassert_equal(fk.n, 4);
	zassert_equal(fk.log[1].head[0], 1, "not a fresh snapshot");
	zassert_equal(fk.log[1].head[1], 0);
	zassert_equal(fk.report_done_calls, 1);
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
	zassert_equal(fk.report_done_calls, 1);

	fk.n = 0;
	script(notconn, 1);
	app_radio_send_telemetry_now();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 1);
	zassert_equal(g_compose_reset_calls, 1);
	app_radio_tx_kick();
	k_sleep(K_SECONDS(1));
	zassert_equal(fk.n, 1);
	zassert_equal(fk.report_done_calls, 1);
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

	for (int i = 0; i < APP_RADIO_TX_QUEUE_DEPTH; i++) {
		zassert_ok(app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, 0, 0, big,
					      APP_RADIO_TX_SLOT_SIZE));
		zassert_ok(app_radio_tx_queue(APP_RADIO_FRAME_ALARM, 0, 0, big, 8));
	}
	zassert_equal(app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, 0, 0, big, 8), -ENOMEM);
	zassert_equal(app_radio_tx_queue(APP_RADIO_FRAME_ALARM, 0, 0, big, 8), -ENOMEM);
	zassert_equal(app_radio_tx_answer_free(), 0);
	zassert_true(app_radio_tx_answer_pending());
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
