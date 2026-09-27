/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STUBS_H_
#define STUBS_H_

#include <zephyr/sys/util.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STUB_COMPOSE_MAX_FRAMES 8

extern size_t g_compose_frames[STUB_COMPOSE_MAX_FRAMES];
extern size_t g_compose_n_frames;
extern int g_compose_calls;
extern int g_compose_budget_zero_calls;
extern uint8_t g_compose_last_budget;
extern int g_compose_reset_calls;
extern uint8_t g_compose_report_id;

extern uint32_t g_budget_error_seq;
extern size_t g_budget_error_cap;
extern int g_budget_error_calls;
extern int g_alarm_flush_calls;

/* app_cmd, scripted for the downlink path (F3): app_cmd_handle() answers
 * g_cmd_resp_len bytes {0xa0, request byte 0, ...} with g_cmd_action; a page
 * stream has g_stream_pages pages {0xc0, page index} left. */
extern int g_cmd_handle_calls;
extern int g_cmd_transport;
extern size_t g_cmd_out_cap;
extern size_t g_cmd_resp_len;
extern int g_cmd_action;
extern int g_stream_pages;
extern int g_stream_next_calls;
extern int g_stream_cancel_calls;
extern int g_run_action_calls;
extern int g_run_action_last;
extern int64_t g_run_action_at_ms;

/* app_history + the HistoryFrame codec, for the replay (F3c). The store holds
 * the records [g_hist_first, g_hist_end) (absolute ordinals), each
 * STUB_HIST_REC_SIZE bytes, every one in the replay window. A frame's envelope
 * costs 6 B plus 2 x the varint width of the frame_index bound
 * (app_cmd_history_sample_capacity), so the #409 3f bound tightening shows.
 * A built frame is {0x01, seq, frame_index, frame_count, samples...} and each
 * sample record starts with its ordinal. */
#define STUB_HIST_REC_SIZE 10

extern uint32_t g_hist_first;
extern uint32_t g_hist_end;
extern bool g_hist_replay_active;
extern int g_hist_replay_active_calls;

/* app_clock_network_time_at_ms(): uptime a network time landed at, 0 = never. */
extern int64_t g_network_time_at_ms;

size_t stub_hist_overhead(uint32_t frame_index_bound);

void stubs_reset(void);

#endif /* STUBS_H_ */
