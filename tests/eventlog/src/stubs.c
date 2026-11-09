/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stubs for app_eventlog: config, clock, radio queue (captures the fPort-4
 * frames) and the AlarmReport encoder (a flat test layout, see below).
 */

#include "app_cmd.h"
#include "app_config.h"
#include "app_radio.h"
#include "test_stubs.h"

#include <errno.h>
#include <string.h>

struct app_config g_app_config;

bool test_clock_has;
uint32_t test_clock_unix;

struct test_frame test_frames[TEST_FRAMES_MAX];
volatile int test_nframes;

int app_clock_get_unix(uint32_t *unix_s)
{
	if (!test_clock_has) {
		return -1;
	}
	*unix_s = test_clock_unix;
	return 0;
}

void app_radio_flash_hold(void)
{
}

void app_radio_flash_release(void)
{
}

uint8_t app_radio_get_max_payload(void)
{
	return 0; /* no DR limit: the 64 B slot */
}

uint32_t app_radio_tx_answer_free(void)
{
	return 8;
}

int app_radio_tx_queue(enum app_radio_frame_kind kind, enum app_radio_frame_tag tag, uint8_t port,
		       const uint8_t *data, size_t len)
{
	(void)kind;
	(void)tag;
	if (test_nframes >= TEST_FRAMES_MAX || len > sizeof(test_frames[0].b)) {
		return -ENOMEM;
	}
	struct test_frame *f = &test_frames[test_nframes];

	f->port = port;
	f->len = len;
	memcpy(f->b, data, len);
	test_nframes++;
	return 0;
}

/* Full/compact stand-in: u8 0x01, u32 seq_first, u8 replay, u32 oldest, u8 n,
 * then per event u8 rule, u8 edge, u32 own seq (0 = previous + 1). Max 8. */
int app_cmd_build_alarm_report_seq(uint32_t base_time, uint32_t total, bool time_synced,
				   const struct app_cmd_alarm_event *events, size_t n_events,
				   uint32_t page_index, uint32_t page_count,
				   const struct app_cmd_alarm_seq *sq, uint8_t *out, size_t out_cap,
				   size_t *out_len)
{
	(void)base_time;
	(void)total;
	(void)time_synced;
	(void)page_index;
	(void)page_count;
	size_t need = 11 + n_events * 6;

	if (n_events > 8 || need > out_cap || sq == NULL) {
		return -EMSGSIZE;
	}
	out[0] = 0x01;
	memcpy(&out[1], &sq->seq_first, 4);
	out[5] = sq->replay;
	memcpy(&out[6], &sq->oldest, 4);
	out[10] = (uint8_t)n_events;
	for (size_t i = 0; i < n_events; i++) {
		out[11 + i * 6] = events[i].rule;
		out[12 + i * 6] = events[i].edge;
		memcpy(&out[13 + i * 6], &events[i].seq, 4);
	}
	*out_len = need;
	return 0;
}
