/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stubs for app_radio.c's app-level dependencies. app_compose is scripted:
 * each report is g_compose_n_frames frames of g_compose_frames[] bytes, and
 * every frame carries {report id, frame index} in its first two bytes.
 */

#include "stubs.h"

#include "app_alarm.h"
#include "app_clock.h"
#include "app_cmd.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_history.h"

#include <zephyr/kernel.h>

#include <errno.h>
#include <string.h>

struct app_config g_app_config;

size_t g_compose_frames[STUB_COMPOSE_MAX_FRAMES];
size_t g_compose_n_frames;
int g_compose_calls;
int g_compose_budget_zero_calls;
uint8_t g_compose_last_budget;
int g_compose_reset_calls;
uint8_t g_compose_report_id;
static size_t m_compose_idx;

uint32_t g_budget_error_seq;
size_t g_budget_error_cap;
int g_budget_error_calls;
int g_alarm_flush_calls;

int g_cmd_handle_calls;
int g_cmd_transport;
size_t g_cmd_out_cap;
size_t g_cmd_resp_len;
int g_cmd_action;
int g_stream_pages;
int g_stream_next_calls;
int g_stream_cancel_calls;
int g_run_action_calls;
int g_run_action_last;
int64_t g_run_action_at_ms;
static uint8_t m_stream_idx;

uint32_t g_hist_first;
uint32_t g_hist_end;
bool g_hist_replay_active;
int g_hist_replay_active_calls;

void stubs_reset(void)
{
	memset(g_compose_frames, 0, sizeof(g_compose_frames));
	g_compose_n_frames = 0;
	g_compose_calls = 0;
	g_compose_budget_zero_calls = 0;
	g_compose_last_budget = 0;
	g_compose_reset_calls = 0;
	g_compose_report_id = 0;
	m_compose_idx = 0;
	g_budget_error_seq = 0;
	g_budget_error_cap = 0;
	g_budget_error_calls = 0;
	g_alarm_flush_calls = 0;
	g_cmd_handle_calls = 0;
	g_cmd_transport = -1;
	g_cmd_out_cap = 0;
	g_cmd_resp_len = 0;
	g_cmd_action = APP_CMD_ACTION_NONE;
	g_stream_pages = 0;
	g_stream_next_calls = 0;
	g_stream_cancel_calls = 0;
	g_run_action_calls = 0;
	g_run_action_last = APP_CMD_ACTION_NONE;
	g_run_action_at_ms = 0;
	m_stream_idx = 0;
	g_hist_first = 0;
	g_hist_end = 0;
	g_hist_replay_active = false;
	g_hist_replay_active_calls = 0;
}

int app_compose_budget(uint8_t *buf, size_t size, size_t *len, bool *more, uint8_t budget)
{
	g_compose_calls++;
	g_compose_last_budget = budget;
	if (budget == 0) {
		g_compose_budget_zero_calls++;
		return -EAGAIN;
	}
	if (g_compose_n_frames == 0) {
		*len = 0;
		*more = false;
		return 0;
	}

	size_t n = MIN(g_compose_frames[m_compose_idx], size);

	memset(buf, 0xee, n);
	if (n >= 2) {
		buf[0] = g_compose_report_id;
		buf[1] = (uint8_t)m_compose_idx;
	}
	*len = n;
	*more = m_compose_idx + 1 < g_compose_n_frames;
	if (*more) {
		m_compose_idx++;
	} else {
		m_compose_idx = 0;
		g_compose_report_id++;
	}
	return 0;
}

void app_compose_reset(void)
{
	g_compose_reset_calls++;
	if (m_compose_idx != 0) {
		m_compose_idx = 0;
		g_compose_report_id++; /* a fresh snapshot */
	}
}

int app_cmd_handle(enum app_cmd_transport transport, const uint8_t *in, size_t in_len, uint8_t *out,
		   size_t out_cap, size_t *out_len, enum app_cmd_action *action)
{
	g_cmd_handle_calls++;
	g_cmd_transport = (int)transport;
	g_cmd_out_cap = out_cap;

	size_t n = MIN(g_cmd_resp_len, out_cap);

	memset(out, 0x77, n);
	if (n >= 2) {
		out[0] = 0xa0;
		out[1] = in_len > 0 ? in[0] : 0;
	}
	*out_len = n;
	*action = (enum app_cmd_action)g_cmd_action;
	return 0;
}

void app_cmd_run_action(enum app_cmd_action action)
{
	g_run_action_calls++;
	g_run_action_last = (int)action;
	g_run_action_at_ms = k_uptime_get();
}

bool app_cmd_stream_active(void)
{
	return g_stream_pages > 0;
}

int app_cmd_stream_next(uint8_t *out, size_t out_cap, size_t *out_len)
{
	g_stream_next_calls++;
	if (g_stream_pages <= 0) {
		return -ENODATA;
	}
	if (out_cap < 2) {
		return -ENOSPC;
	}
	out[0] = 0xc0;
	out[1] = m_stream_idx++;
	*out_len = 2;
	g_stream_pages--;
	return 0;
}

void app_cmd_stream_cancel(void)
{
	g_stream_cancel_calls++;
	g_stream_pages = 0;
}

int app_cmd_build_info_seq(uint32_t seq, uint8_t *out, size_t out_cap, size_t *out_len, bool *more)
{
	if (out_cap < 2) {
		return -ENOSPC;
	}
	out[0] = 0x01;
	out[1] = (uint8_t)seq;
	*out_len = 2;
	*more = false;
	return 0;
}

int app_cmd_build_config_status(uint8_t *out, size_t out_cap, size_t *out_len, bool *more)
{
	if (out_cap < 2) {
		return -ENOSPC;
	}
	out[0] = 0x01;
	out[1] = 0x5e;
	*out_len = 2;
	*more = false;
	return 0;
}

/* Error BUDGET_TOO_SMALL: version, field 1 (seq, one byte here), a marker. */
int app_cmd_build_budget_error(uint32_t seq, uint8_t *out, size_t out_cap, size_t *out_len)
{
	g_budget_error_calls++;
	g_budget_error_seq = seq;
	g_budget_error_cap = out_cap;
	if (out_cap < 4) {
		return -ENOSPC;
	}
	out[0] = 0x01;
	out[1] = 0x08;
	out[2] = (uint8_t)seq;
	out[3] = 0x2a;
	*out_len = 4;
	return 0;
}

int app_clock_get_unix(uint32_t *unix_time)
{
	ARG_UNUSED(unix_time);
	return -EAGAIN;
}

int64_t app_clock_network_time_at_ms(void)
{
	return 0;
}

void app_alarm_flush_held(void)
{
	g_alarm_flush_calls++;
}

/* ---- app_history and the HistoryFrame codec (F3c) ---- */

static size_t varint_len(uint32_t v)
{
	size_t n = 1;

	while (v >= 0x80) {
		v >>= 7;
		n++;
	}
	return n;
}

size_t stub_hist_overhead(uint32_t frame_index_bound)
{
	return 6 + 2 * varint_len(frame_index_bound);
}

uint32_t app_history_get_mask(void)
{
	return 0x01;
}

uint32_t app_history_get_interval(void)
{
	return 60;
}

void app_history_set_replay_active(bool active)
{
	g_hist_replay_active = active;
	g_hist_replay_active_calls++;
}

void app_history_span(uint32_t *first_abs, uint32_t *end_abs)
{
	*first_abs = g_hist_first;
	*end_abs = g_hist_end;
}

uint16_t app_history_count_frames(uint32_t from_unix, uint32_t to_unix, size_t cap)
{
	ARG_UNUSED(from_unix);
	ARG_UNUSED(to_unix);

	size_t per = cap / STUB_HIST_REC_SIZE;
	uint32_t n = g_hist_end - g_hist_first;

	return (per == 0 || n == 0) ? 0 : (uint16_t)((n + per - 1) / per);
}

size_t app_history_export_abs(uint32_t from_unix, uint32_t to_unix, uint32_t start_abs,
			      uint32_t end_abs, uint8_t *buf, size_t cap, uint32_t *t0_out,
			      bool *synced_out, uint16_t *n_written, uint32_t *next_abs)
{
	ARG_UNUSED(from_unix);
	ARG_UNUSED(to_unix);

	uint32_t start = MAX(start_abs, g_hist_first);
	uint32_t end = MIN(end_abs, g_hist_end);
	uint16_t n = 0;

	while (start + n < end && (size_t)(n + 1) * STUB_HIST_REC_SIZE <= cap) {
		uint8_t *rec = &buf[n * STUB_HIST_REC_SIZE];

		memset(rec, 0x5a, STUB_HIST_REC_SIZE);
		rec[0] = (uint8_t)(start + n);
		n++;
	}
	*t0_out = 1000 + start;
	*synced_out = true;
	*n_written = n;
	*next_abs = start + n;
	return (size_t)n * STUB_HIST_REC_SIZE;
}

size_t app_cmd_history_sample_capacity(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				       uint32_t t0_unix, uint32_t present, uint32_t interval_s,
				       size_t out_cap)
{
	ARG_UNUSED(seq);
	ARG_UNUSED(frame_count);
	ARG_UNUSED(t0_unix);
	ARG_UNUSED(present);
	ARG_UNUSED(interval_s);

	size_t over = stub_hist_overhead(frame_index);

	return out_cap > over ? out_cap - over : 0;
}

int app_cmd_build_history_frame(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				uint32_t t0_unix, uint32_t present, uint32_t interval_s,
				bool time_synced, const uint8_t *samples, size_t samples_len,
				uint8_t *out, size_t out_cap, size_t *out_len)
{
	ARG_UNUSED(t0_unix);
	ARG_UNUSED(present);
	ARG_UNUSED(interval_s);
	ARG_UNUSED(time_synced);

	if (4 + samples_len > out_cap) {
		return -EMSGSIZE;
	}
	out[0] = 0x01;
	out[1] = (uint8_t)seq;
	out[2] = (uint8_t)frame_index;
	out[3] = (uint8_t)frame_count;
	memcpy(&out[4], samples, samples_len);
	*out_len = 4 + samples_len;
	return 0;
}
