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

bool app_cmd_stream_active(void)
{
	return false;
}

void app_cmd_stream_cancel(void)
{
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
