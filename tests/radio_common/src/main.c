/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The common uplink path of app_radio.c (doc/plan/460 F4) against a fake
 * backend. The scenarios that both radios share run twice: once with a
 * LoRaWAN-like backend (nothing sent confirmed, 3 s between the frames of a
 * report, 51 B budget) and once with a P2P-like one (answers, alarms and
 * history confirmed, no frame gap, 239 B budget) -- one implementation, the
 * same behaviour on either radio (decision #23). Link supervision (F2) runs
 * the same way: the fake reports link-check outcomes and records the rungs and
 * rejoins app_radio asks for.
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
	bool in_flight; /* a confirmed frame waits for its Ack retries (P2P) */
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

struct profile {
	const struct app_radio_backend *be;
	uint8_t budget;
	uint8_t queued_flags; /* flags of a queued answer or alarm */
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

static bool fake_replay_active(void)
{
	return fk.replay;
}

static bool fake_in_flight(void)
{
	return fk.in_flight;
}

static const struct app_radio_backend be_lrw = {
	.send = fake_send,
	.budget = fake_budget,
	.tx_ready = fake_tx_ready,
	.report_flags = fake_report_flags,
	.replay_active = fake_replay_active,
	.get_state = fake_get_state,
	.warning_step = fake_warning_step,
	.rejoin = fake_rejoin,
	.confirm_kinds = 0,
	.frame_gap_ms = 3000,
	.cmd_transport = APP_CMD_TRANSPORT_LRW,
};

static const struct app_radio_backend be_p2p = {
	.send = fake_send,
	.budget = fake_budget,
	.tx_ready = fake_tx_ready,
	.report_flags = fake_report_flags,
	.replay_active = fake_replay_active,
	.get_state = fake_get_state,
	.warning_step = fake_warning_step,
	.rejoin = fake_rejoin,
	.in_flight = fake_in_flight,
	.confirm_kinds = BIT(APP_RADIO_FRAME_ANSWER) | BIT(APP_RADIO_FRAME_ALARM) |
			 BIT(APP_RADIO_FRAME_HISTORY),
	.frame_gap_ms = 0,
	.cmd_transport = APP_CMD_TRANSPORT_P2P,
};

static const struct profile PROFILE_LRW = {&be_lrw, 51, 0, APP_RADIO_FRAME_LINK_CHECK,
					   APP_CMD_TRANSPORT_LRW};
static const struct profile PROFILE_P2P = {&be_p2p, 239, APP_RADIO_FRAME_CONFIRMED,
					   APP_RADIO_FRAME_CONFIRMED, APP_CMD_TRANSPORT_P2P};

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

static void before(void *f)
{
	ARG_UNUSED(f);
	app_radio_test_tx_reset();
	app_radio_test_link_reset();
	app_radio_test_cmd_reset();
	app_radio_test_air_reset();
	stubs_reset();
	memset(&fk, 0, sizeof(fk));
	fk.ready = true;
	fk.state = APP_RADIO_STATE_HEALTHY;
	memset(&g_app_config, 0, sizeof(g_app_config));
	g_app_config.interval_report = 60;
	use_profile(&PROFILE_LRW);
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

/* A confirmed frame still retrying holds the action where the backend has
 * one in flight (P2P); LoRaWAN's send covers the whole exchange. */
static void action_waits_for_a_frame_in_flight(void)
{
	bool waits = m_prof->be->in_flight != NULL;

	fk.in_flight = true;
	g_cmd_action = APP_CMD_ACTION_COUNTERS_SAVE;
	app_radio_downlink(m_cmd, sizeof(m_cmd));
	k_sleep(K_SECONDS(9));
	zassert_equal(g_run_action_calls, waits ? 0 : 1);
	fk.in_flight = false;
	k_sleep(K_SECONDS(8));
	zassert_equal(g_run_action_calls, 1);
	zassert_equal(g_run_action_last, APP_CMD_ACTION_COUNTERS_SAVE);
}
BOTH_PROFILES(action_waits_for_a_frame_in_flight)

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

/* A frame the answer queue refused goes on the 5 s retry. */
static void announce_retries_when_the_queue_is_full(void)
{
	g_app_config.interval_report = 2;
	fk.ready = false;
	for (uint8_t i = 0; i < APP_RADIO_TX_QUEUE_DEPTH; i++) {
		queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 8, 0xb0 + i);
	}
	app_radio_announce();
	k_sleep(K_SECONDS(3));
	zassert_true(app_radio_announce_pending(), "nothing fit yet");

	fk.ready = true;
	app_radio_tx_kick();
	k_sleep(K_SECONDS(7));
	zassert_equal(fk.n, APP_RADIO_TX_QUEUE_DEPTH + 2, "the answers, then the announce");
	zassert_equal(fk.log[APP_RADIO_TX_QUEUE_DEPTH].tag, APP_RADIO_TAG_INFO);
	zassert_equal(fk.log[APP_RADIO_TX_QUEUE_DEPTH + 1].tag, APP_RADIO_TAG_SETTINGS);
	zassert_false(app_radio_announce_pending());
}
BOTH_PROFILES(announce_retries_when_the_queue_is_full)

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
