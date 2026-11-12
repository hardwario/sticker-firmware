/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_eventlog on the native_sim flash simulator. A "reboot" is a fresh
 * app_eventlog_init(): the simulator flash persists in-process. The ztest
 * thread is cooperative, so edges added without a sleep pile up in the
 * pending queue exactly as a burst from a higher-priority thread does.
 */

#include "app_cmd.h"
#include "app_config.h"
#include "app_eventlog.h"
#include "test_stubs.h"

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <string.h>

#define RAW_F_SYNC  BIT(0)
#define RAW_F_RP    BIT(1)
#define RAW_F_DELTA BIT(2)

struct parsed {
	bool synced;
	uint32_t base;
	uint16_t rel[32];
	bool replay;
	bool delta;
	uint32_t seq_first;
	uint32_t oldest;
	uint8_t n;
	uint32_t seq[32];
	uint8_t rule[32];
	uint8_t edge[32];
};

/* Decode one captured fPort-4 frame (raw 0x03, or the stub's full layout). */
static void parse(const struct test_frame *f, struct parsed *p)
{
	memset(p, 0, sizeof(*p));
	zassert_equal(f->port, APP_EVENTLOG_PORT);
	if (f->b[0] == 0x03) {
		uint8_t fl = f->b[5];
		size_t i = 10;

		p->synced = fl & RAW_F_SYNC;
		p->base = sys_get_le32(&f->b[1]);
		p->replay = fl & RAW_F_RP;
		p->delta = fl & RAW_F_DELTA;
		p->seq_first = sys_get_le32(&f->b[6]);
		if (p->replay) {
			p->oldest = sys_get_le32(&f->b[10]);
			i = 14;
		}
		size_t rec = p->delta ? 4 : 3;
		uint32_t sq = p->seq_first;

		zassert_equal((f->len - i) % rec, 0, "raw frame length");
		for (; i + rec <= f->len; i += rec) {
			if (p->n) {
				sq += p->delta ? f->b[i + 3] : 1;
			}
			p->rel[p->n] = sys_get_le16(&f->b[i]);
			p->rule[p->n] = f->b[i + 2] & 0x7f;
			p->edge[p->n] = f->b[i + 2] >> 7;
			p->seq[p->n++] = sq;
		}
		return;
	}
	zassert_equal(f->b[0], 0x01, "unknown frame");
	memcpy(&p->seq_first, &f->b[1], 4);
	p->replay = f->b[5];
	memcpy(&p->oldest, &f->b[6], 4);
	uint32_t sq = p->seq_first;

	for (uint8_t k = 0; k < f->b[10]; k++) {
		uint32_t own;

		memcpy(&own, &f->b[13 + k * 6], 4);
		if (k) {
			sq = own ? own : sq + 1;
		}
		p->rule[k] = f->b[11 + k * 6];
		p->edge[k] = f->b[12 + k * 6];
		p->seq[p->n++] = sq;
	}
}

/* All event seqs of frames [from, test_nframes). */
static int collect(int from, uint32_t *out, int max)
{
	int n = 0;

	for (int i = from; i < test_nframes; i++) {
		struct parsed p;

		parse(&test_frames[i], &p);
		for (uint8_t k = 0; k < p.n && n < max; k++) {
			out[n++] = p.seq[k];
		}
	}
	return n;
}

static void add(uint8_t rule, uint8_t edge)
{
	struct app_cmd_alarm_event ev = {.rule = rule, .edge = edge, .type = 3};

	app_eventlog_add(&ev, 0);
}

/* Let the writer / flush works run. */
static void settle(void)
{
	k_sleep(K_MSEC(200));
}

/* Add n edges in bursts the pending queue (32) can hold. */
static void add_n(int n)
{
	for (int i = 0; i < n; i++) {
		add(i % 4, (i / 4) % 2);
		if (i % 20 == 19) {
			settle();
		}
	}
	settle();
}

static void reboot(void)
{
	zassert_equal(app_eventlog_init(), 0, "init");
}

static void set_split(int split)
{
	g_app_config.log_split = split;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	const struct flash_area *fa;

	memset(&g_app_config, 0, sizeof(g_app_config));
	g_app_config.radio_mode = APP_CONFIG_RADIO_MODE_LORAWAN;
	g_app_config.event_format = APP_CONFIG_EVENT_FORMAT_RAW;
	set_split(25);
	test_clock_has = false;
	test_budget = 0;

	zassert_ok(flash_area_open(FIXED_PARTITION_ID(history_partition), &fa));
	zassert_ok(flash_area_erase(fa, 0, fa->fa_size));
	flash_area_close(fa);
	reboot();
	test_nframes = 0;
}

ZTEST_SUITE(eventlog, NULL, NULL, before, NULL, NULL);

ZTEST(eventlog, test_pages_by_split)
{
	zassert_equal(app_eventlog_flash_pages(), 2, "25 %% of 8 pages");
	set_split(100);
	zassert_equal(app_eventlog_flash_pages(), 8);
	set_split(10);
	zassert_equal(app_eventlog_flash_pages(), 0, "rounded down to 0 %%");
}

/* A flush carries consecutive seqs from 1, seq_first in the raw header. */
ZTEST(eventlog, test_seq_basic)
{
	add_n(5);
	zassert_equal(app_eventlog_pending(), 5);
	app_eventlog_flush();
	settle();

	zassert_equal(test_nframes, 1);
	struct parsed p;

	parse(&test_frames[0], &p);
	zassert_false(p.replay);
	zassert_false(p.delta);
	zassert_equal(p.seq_first, 1);
	zassert_equal(p.n, 5);
	zassert_equal(p.seq[4], 5);
	zassert_equal(p.rule[1], 1);
	zassert_equal(app_eventlog_pending(), 0);
}

/* Unsent events survive a reboot and the seq goes on after them; what was
 * uploaded is not sent again after the next reboot. */
ZTEST(eventlog, test_seq_survives_reboot)
{
	add_n(3);
	reboot();
	zassert_equal(app_eventlog_pending(), 3, "unsent kept");
	add_n(2);
	app_eventlog_flush();
	settle();

	uint32_t s[16];
	int n = collect(0, s, ARRAY_SIZE(s));

	zassert_equal(n, 5);
	for (int i = 0; i < n; i++) {
		zassert_equal(s[i], i + 1, "seq %d", i);
	}

	reboot();
	zassert_equal(app_eventlog_pending(), 0, "SENT mark survives");
	int before_n = test_nframes;

	add_n(1);
	app_eventlog_flush();
	settle();
	n = collect(before_n, s, ARRAY_SIZE(s));
	zassert_equal(n, 1);
	zassert_equal(s[0], 6, "seq 6 after the second reboot");
}

/* Edges dropped by a full pending queue leave their seqs out. */
ZTEST(eventlog, test_overflow_gap)
{
	for (int i = 0; i < 40; i++) {
		add(0, i % 2); /* no sleep: the writer cannot run */
	}
	settle();
	zassert_equal(app_eventlog_lost(), 8, "32 queued, 8 dropped");
	add_n(1);
	app_eventlog_flush();
	settle();

	uint32_t s[64];
	int n = collect(0, s, ARRAY_SIZE(s));

	zassert_equal(n, 33);
	zassert_equal(s[31], 32);
	zassert_equal(s[32], 41, "seqs 33..40 missing");
}

/* A full ring evicts its oldest page: lost counts the unsent events, a
 * replay of evicted seqs answers empty with the oldest retained seq. */
ZTEST(eventlog, test_wrap_evicts_and_reports_oldest)
{
	add_n(300); /* 2 pages hold 252 */
	zassert_equal(app_eventlog_lost(), 126, "one page dropped");
	zassert_equal(app_eventlog_pending(), 300 - 126);

	uint32_t rq[] = {1, 5};

	zassert_ok(app_eventlog_request_ranges(rq, 1));
	settle();
	zassert_equal(test_nframes, 1);
	struct parsed p;

	parse(&test_frames[0], &p);
	zassert_true(p.replay);
	zassert_equal(p.n, 0, "nothing retained");
	zassert_equal(p.oldest, 127);
	zassert_equal(p.seq_first, 301, "next seq");
}

/* Replay by ranges and by single seqs: only the selected events, replay flag,
 * per-event seq deltas where the run breaks. */
ZTEST(eventlog, test_replay_ranges_and_seqs)
{
	add_n(20);
	app_eventlog_flush();
	settle();
	int base = test_nframes;

	uint32_t rq[] = {3, 2, 10, 3};

	zassert_ok(app_eventlog_request_ranges(rq, 2));
	settle();
	struct parsed p;

	zassert_equal(test_nframes, base + 1);
	parse(&test_frames[base], &p);
	zassert_true(p.replay);
	zassert_true(p.delta);
	zassert_equal(p.oldest, 1);
	zassert_equal(p.n, 5);
	uint32_t want1[] = {3, 4, 10, 11, 12};

	zassert_mem_equal(p.seq, want1, sizeof(want1));

	base = test_nframes;
	uint32_t sq[] = {1, 7, 19, 500};

	zassert_ok(app_eventlog_request_seqs(sq, ARRAY_SIZE(sq)));
	settle();
	parse(&test_frames[base], &p);
	uint32_t want2[] = {1, 7, 19};

	zassert_equal(p.n, 3, "500 does not exist");
	zassert_mem_equal(p.seq, want2, sizeof(want2));

	/* Bad counts */
	zassert_equal(app_eventlog_request_ranges(rq, 0), -EINVAL);
	zassert_equal(app_eventlog_request_ranges(rq, 9), -EINVAL);
	zassert_equal(app_eventlog_request_seqs(sq, 21), -EINVAL);
}

/* Raw stores a seq delta in one byte: a wider jump starts a new frame. */
ZTEST(eventlog, test_raw_delta_over_255_splits)
{
	set_split(100);
	reboot();
	add_n(300);
	app_eventlog_flush();
	settle();
	int base = test_nframes;

	uint32_t sq[] = {1, 2, 300};

	zassert_ok(app_eventlog_request_seqs(sq, ARRAY_SIZE(sq)));
	settle();
	zassert_equal(test_nframes, base + 2, "delta 298 does not fit");
	struct parsed a, b;

	parse(&test_frames[base], &a);
	parse(&test_frames[base + 1], &b);
	zassert_equal(a.n, 2);
	zassert_equal(a.seq[1], 2);
	zassert_equal(b.seq_first, 300);
	zassert_equal(b.n, 1);
}

/* Time window replay on a synced page. */
ZTEST(eventlog, test_replay_time_window)
{
	test_clock_has = true;
	test_clock_unix = 1000000;
	add_n(3);
	test_clock_unix = 2000000;
	add_n(3);

	zassert_ok(app_eventlog_request(1500000, 2500000));
	settle();
	uint32_t s[16];
	int n = collect(0, s, ARRAY_SIZE(s));

	zassert_equal(n, 3);
	zassert_equal(s[0], 4);
	zassert_equal(s[2], 6);
}

/* Full / compact (AlarmReport): seq_first, replay, oldest and the event's own
 * seq where the run breaks. */
ZTEST(eventlog, test_full_format_seq)
{
	g_app_config.event_format = APP_CONFIG_EVENT_FORMAT_FULL;
	add_n(4);
	uint32_t sq[] = {1, 3, 4};

	zassert_ok(app_eventlog_request_seqs(sq, ARRAY_SIZE(sq)));
	settle();
	struct parsed p;

	zassert_equal(test_nframes, 1);
	parse(&test_frames[0], &p);
	zassert_true(p.replay);
	zassert_equal(p.seq_first, 1);
	zassert_equal(p.oldest, 1);
	uint32_t want[] = {1, 3, 4};

	zassert_equal(p.n, 3);
	zassert_mem_equal(p.seq, want, sizeof(want));
}

/* Big uploads page into 64 B raw frames (17 events) without a gap. */
ZTEST(eventlog, test_batch_paging)
{
	add_n(100);
	app_eventlog_flush();
	settle();
	uint32_t s[128];
	int n = collect(0, s, ARRAY_SIZE(s));

	zassert_equal(n, 100);
	for (int i = 0; i < n; i++) {
		zassert_equal(s[i], i + 1);
	}
	for (int i = 0; i < test_nframes; i++) {
		zassert_true(test_frames[i].len <= 64);
	}
	zassert_true(test_nframes >= 6, "17 per frame");
}

/* Outside LoRaWAN a replay is refused. */
ZTEST(eventlog, test_request_needs_lorawan)
{
	g_app_config.radio_mode = 0;
	zassert_equal(app_eventlog_request(0, UINT32_MAX), -ENOTSUP);
}

/* NFC export: pages of `max` events from a seq cursor; next_seq / more walk
 * the whole log once, seq gaps are flagged per event. */
ZTEST(eventlog, test_export_paging)
{
	for (int i = 0; i < 40; i++) {
		add(0, i % 2); /* 8 dropped: seqs 33..40 missing */
	}
	settle();
	add_n(5); /* seqs 41..45 */

	struct app_cmd_alarm_event ev[6];
	struct app_eventlog_page pg;
	uint32_t cur = 0, seen = 0, last = 0;
	int pages = 0;

	do {
		int n = app_eventlog_export(cur, 0, UINT32_MAX, ev, ARRAY_SIZE(ev), &pg);

		zassert_true(n >= 0);
		uint32_t sq = pg.seq_first;

		for (int i = 0; i < n; i++) {
			if (i) {
				sq = ev[i].seq ? ev[i].seq : sq + 1;
			}
			zassert_true(sq > last, "ascending");
			if (last == 32) {
				zassert_equal(sq, 41, "gap kept");
			}
			last = sq;
			seen++;
		}
		cur = pg.next_seq;
		pages++;
	} while (pg.more && pages < 20);

	zassert_equal(seen, 37);
	zassert_equal(last, 45);
	zassert_equal(pg.oldest, 1);
	zassert_equal(pg.next_seq, 46, "cursor ends at the next seq");

	/* Cursor in the middle, and past the end */
	int n = app_eventlog_export(44, 0, UINT32_MAX, ev, ARRAY_SIZE(ev), &pg);

	zassert_equal(n, 2);
	zassert_false(pg.more);
	n = app_eventlog_export(100, 0, UINT32_MAX, ev, ARRAY_SIZE(ev), &pg);
	zassert_equal(n, 0);
	zassert_equal(pg.seq_first, 46);
}

/* Budget below one event (US915 DR0, 11 B): nothing is sent and nothing is
 * marked sent; a larger budget later uploads every event. */
ZTEST(eventlog, test_budget_too_small_defers)
{
	add_n(5);
	test_budget = 11;
	app_eventlog_flush();
	settle();
	zassert_equal(test_nframes, 0, "13 B raw page cannot fit 11 B");

	test_budget = 0;
	app_eventlog_flush();
	settle();
	uint32_t s[16];
	int n = collect(0, s, ARRAY_SIZE(s));

	zassert_equal(n, 5, "all 5 sent later");
	zassert_equal(s[0], 1);
	zassert_equal(s[4], 5);
}

/* Small DR budget lays out small pages (51 B: (51 - 10) / 3 = 13 events). */
ZTEST(eventlog, test_budget_limits_page)
{
	add_n(30);
	test_budget = 51;
	app_eventlog_flush();
	settle();
	for (int i = 0; i < test_nframes; i++) {
		zassert_true(test_frames[i].len <= 51, "frame %d: %zu B", i, test_frames[i].len);
	}
	zassert_equal(test_nframes, 3, "13 + 13 + 4");
}

/* The radio drops pages over budget (the DR fell): they come back as one
 * replay with both ranges. */
ZTEST(eventlog, test_dropped_pages_replayed)
{
	add_n(30);
	app_eventlog_flush();
	settle();
	zassert_equal(test_nframes, 2, "18 + 12");
	struct test_frame f0 = test_frames[0], f1 = test_frames[1];

	test_budget = 51;
	app_eventlog_frame_dropped(f0.b, f0.len);
	app_eventlog_frame_dropped(f1.b, f1.len);
	app_eventlog_frame_dropped(f1.b, f1.len); /* already handled */
	settle();

	uint32_t s[40];
	int n = collect(2, s, ARRAY_SIZE(s));
	struct parsed p;

	parse(&test_frames[2], &p);
	zassert_true(p.replay);
	zassert_equal(n, 30, "all 30 again");
	zassert_equal(s[0], 1);
	zassert_equal(s[29], 30);
	for (int i = 2; i < test_nframes; i++) {
		zassert_true(test_frames[i].len <= 51);
	}
}

#define T_UNIX 1794000000U

static uint32_t up_s(void)
{
	return (uint32_t)(k_uptime_get() / 1000);
}

/* Sync the clock now: unix = T_UNIX at this uptime. */
static uint32_t clock_sync(void)
{
	uint32_t off = T_UNIX - up_s();

	test_clock_has = true;
	test_clock_unix = T_UNIX;
	app_eventlog_on_clock_sync(T_UNIX);
	settle();
	return off;
}

/* Never synced: the batch goes out unsynced, stamped with this boot's uptime. */
ZTEST(eventlog, test_unsynced_batch_uses_uptime)
{
	uint32_t up0 = up_s();

	add_n(3);
	app_eventlog_flush();
	settle();
	zassert_equal(test_nframes, 1);
	struct parsed p;

	parse(&test_frames[0], &p);
	zassert_false(p.synced, "flag bit0 clear");
	zassert_true(p.base >= up0 && p.base <= up_s(), "base %u = uptime", p.base);
	zassert_equal(p.n, 3);
}

/* Edges before the sync, uploaded after it: unix times by the fix-up. */
ZTEST(eventlog, test_sync_fixes_this_boot)
{
	uint32_t up0 = up_s();

	add_n(3);
	k_sleep(K_SECONDS(2));
	uint32_t off = clock_sync();

	add_n(2);
	app_eventlog_flush();
	settle();
	struct parsed p;

	zassert_equal(test_nframes, 1, "one domain, one frame");
	parse(&test_frames[0], &p);
	zassert_true(p.synced, "fixed page goes out synced");
	zassert_equal(p.n, 5);
	zassert_true(p.base >= up0 + off && p.base <= up0 + off + 1, "base %u", p.base);
	zassert_true(p.rel[3] >= 2, "post-sync edges 2 s later");
}

/* The fix-up is in flash: after a reboot the old page keeps unix time. */
ZTEST(eventlog, test_fixup_survives_reboot)
{
	add_n(3);
	uint32_t off = clock_sync();
	uint32_t want = (uint32_t)(k_uptime_get() / 1000) + off;

	reboot();
	test_clock_has = false; /* the new boot is not synced yet */
	app_eventlog_flush();
	settle();
	struct parsed p;

	zassert_equal(test_nframes, 1);
	parse(&test_frames[0], &p);
	zassert_true(p.synced, "fix-up read back from flash");
	zassert_true(p.base + 2 >= want && p.base <= want, "base %u want ~%u", p.base, want);
}

/* A boot that never synced keeps its uptime; the next boot's synced edges go
 * in another frame (domains never mix), seq continuous. */
ZTEST(eventlog, test_unsynced_boot_then_synced_boot)
{
	add_n(3);
	reboot();
	clock_sync();
	add_n(2);
	app_eventlog_flush();
	settle();
	zassert_equal(test_nframes, 2, "two time domains");
	struct parsed a, b;

	parse(&test_frames[0], &a);
	parse(&test_frames[1], &b);
	zassert_false(a.synced, "old boot stays uptime");
	zassert_true(b.synced);
	zassert_equal(a.seq_first, 1);
	zassert_equal(a.n, 3);
	zassert_equal(b.seq_first, 4, "seq continues");
	zassert_equal(b.n, 2);
}

/* A seq jump (pending queue overflow) after a late clock sync opens a synced
 * page: its records must carry unix time, not this boot's uptime. */
ZTEST(eventlog, test_seq_jump_after_sync_keeps_unix)
{
	add_n(3); /* unsynced head page */
	clock_sync();
	for (int i = 0; i < 40; i++) {
		add(0, i % 2); /* 8 dropped: the next edge jumps */
	}
	settle();
	add_n(2);
	app_eventlog_flush();
	settle();
	zassert_true(test_nframes > 0);
	for (int i = 0; i < test_nframes; i++) {
		struct parsed p;

		parse(&test_frames[i], &p);
		zassert_true(p.synced, "frame %d synced", i);
		zassert_true(p.base >= T_UNIX - 1000, "frame %d base %u is unix", i, p.base);
	}
}

/* A req_events downlink does not replace the re-send of dropped frames. */
ZTEST(eventlog, test_req_keeps_dropped_replay)
{
	add_n(30);
	app_eventlog_flush();
	settle();
	struct test_frame f0 = test_frames[0];
	int base = test_nframes;
	uint32_t want = 25;

	app_eventlog_frame_dropped(f0.b, f0.len);
	zassert_ok(app_eventlog_request_seqs(&want, 1));
	k_sleep(K_MSEC(6000));

	uint32_t s[40];
	int n = collect(base, s, ARRAY_SIZE(s));

	zassert_equal(n, 19, "18 dropped + 1 requested, got %d", n);
	zassert_equal(s[0], 1);
	zassert_equal(s[17], 18);
	zassert_equal(s[18], 25);
}

/* A re-send that is dropped again is not re-sent (no loop on a dead link). */
ZTEST(eventlog, test_dropped_resend_not_looped)
{
	add_n(5);
	app_eventlog_flush();
	settle();
	struct test_frame f0 = test_frames[0];

	app_eventlog_frame_dropped(f0.b, f0.len);
	settle();
	zassert_equal(test_nframes, 2, "one re-send");
	struct test_frame f1 = test_frames[1];

	app_eventlog_frame_dropped(f1.b, f1.len);
	k_sleep(K_MSEC(6000));
	zassert_equal(test_nframes, 2, "not re-sent again");
}

/* event_ack: a sent batch stays unsent until ack_events; an ack moves the
 * upload position only up to its seq. */
ZTEST(eventlog, test_ack_moves_position)
{
	g_app_config.event_ack = true;
	g_app_config.interval_report = 900;
	add_n(10);
	app_eventlog_flush();
	settle();
	zassert_equal(test_nframes, 1);
	zassert_equal(app_eventlog_pending(), 10, "no ack yet");

	zassert_ok(app_eventlog_ack(6));
	settle();
	zassert_equal(app_eventlog_pending(), 4);
	zassert_ok(app_eventlog_ack(3));
	settle();
	zassert_equal(app_eventlog_pending(), 4, "an older ack does not go back");
	zassert_ok(app_eventlog_ack(10));
	settle();
	zassert_equal(app_eventlog_pending(), 0);

	/* Nothing re-sent once acknowledged. */
	k_sleep(K_SECONDS(2000));
	zassert_equal(test_nframes, 1);
}

/* An ack never reaches past the last batch sent. */
ZTEST(eventlog, test_ack_capped_at_sent_batch)
{
	g_app_config.event_ack = true;
	g_app_config.interval_report = 900;
	add_n(5);
	app_eventlog_flush();
	settle();
	add_n(3);
	zassert_ok(app_eventlog_ack(100));
	settle();
	zassert_equal(app_eventlog_pending(), 3);
}

/* No ack: the batch is re-sent from the first unacknowledged event up to
 * EVENTLOG_ACK_RETRIES (3) times, then the log waits for the next flush. */
ZTEST(eventlog, test_ack_retries_then_gives_up)
{
	g_app_config.event_ack = true;
	g_app_config.interval_report = 60;
	add_n(4);
	app_eventlog_flush();
	settle();
	zassert_equal(test_nframes, 1);
	zassert_ok(app_eventlog_ack(2));
	settle();

	k_sleep(K_SECONDS(10 * 120));
	zassert_equal(test_nframes, 4, "1 + 3 re-sends");
	struct parsed p;

	parse(&test_frames[1], &p);
	zassert_equal(p.seq_first, 3, "from the first unacknowledged");
	zassert_equal(p.n, 2);
	zassert_equal(app_eventlog_pending(), 2);
}

/* Unacknowledged events go again after a reboot; ack off keeps the old
 * fire-and-forget behaviour. */
ZTEST(eventlog, test_ack_unacked_resent_after_reboot)
{
	g_app_config.event_ack = true;
	g_app_config.interval_report = 900;
	add_n(3);
	app_eventlog_flush();
	settle();
	reboot();
	zassert_equal(app_eventlog_pending(), 3);

	g_app_config.event_ack = false;
	app_eventlog_flush();
	settle();
	zassert_equal(app_eventlog_pending(), 0);
}

ZTEST(eventlog, test_ack_needs_lorawan)
{
	g_app_config.radio_mode = 0;
	zassert_equal(app_eventlog_ack(1), -ENOTSUP);
}
