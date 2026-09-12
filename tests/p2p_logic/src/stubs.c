/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal stubs so app_p2p.c links on native_sim: the global config it reads
 * and the app_compose entry points it calls. The pure-logic tests never drive
 * a real send/compose, so these can be trivial. app_ccm.c is the REAL source
 * (linked in CMakeLists) -- the frame codec tests need genuine CCM.
 *
 * The B8 history-replay tests reference app_p2p_start_history_replay, which
 * makes the whole replay call graph live (it is otherwise dropped by
 * --gc-sections, which is why this file used to need only three stubs). Hence
 * the app_history and app_cmd set below: inert by default, with a couple of
 * `test_*` knobs and `g_*_calls` counters so a test can steer the path and see
 * what it touched.
 */

#include "app_compose.h"
#include "app_config.h"

#include <zephyr/toolchain.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_config g_app_config;

/* ---- B8 replay knobs, driven by tests/p2p_logic/src/main.c ---- */

/* How many frames app_history_count_frames() claims the window holds. */
uint16_t test_history_frame_count;
/* How many records still sit past the cursor (drives the replay's terminator). */
size_t test_history_count = 1;
/* Counts every telemetry compose the P2P send path attempted. */
int g_compose_budget_calls;
/* Tracks the last app_history_set_replay_active() argument. */
bool g_history_replay_active;

int app_compose_budget(uint8_t *buf, size_t size, size_t *len, bool *more, uint8_t budget)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(size);
	ARG_UNUSED(budget);
	g_compose_budget_calls++;
	if (len) {
		*len = 0;
	}
	if (more) {
		*more = false;
	}
	return 0;
}

void app_compose_reset(void)
{
}

/* ---- app_history / app_cmd, for the B8 replay call graph ---- */

uint16_t app_history_count_frames(uint32_t from_unix, uint32_t to_unix, size_t cap)
{
	ARG_UNUSED(from_unix);
	ARG_UNUSED(to_unix);
	ARG_UNUSED(cap);
	return test_history_frame_count;
}

size_t app_history_count(void)
{
	return test_history_count;
}

uint32_t app_history_get_mask(void)
{
	return 0x01;
}

uint32_t app_history_get_interval(void)
{
	return 60;
}

bool app_history_base_synced(void)
{
	return true;
}

void app_history_set_replay_active(bool active)
{
	g_history_replay_active = active;
}

size_t app_history_export_page(uint32_t from_unix, uint32_t to_unix, size_t cursor, uint8_t *out,
			       size_t cap, uint32_t *t0, uint16_t *n, size_t *next)
{
	ARG_UNUSED(from_unix);
	ARG_UNUSED(to_unix);
	ARG_UNUSED(out);
	ARG_UNUSED(cap);
	if (t0) {
		*t0 = 0;
	}
	if (n) {
		*n = 0; /* no records -- the replay terminates on the first pass */
	}
	if (next) {
		*next = cursor;
	}
	return 0;
}

size_t app_cmd_history_sample_capacity(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				       uint32_t t0, uint32_t present, uint32_t interval,
				       size_t out_cap)
{
	ARG_UNUSED(seq);
	ARG_UNUSED(frame_index);
	ARG_UNUSED(frame_count);
	ARG_UNUSED(t0);
	ARG_UNUSED(present);
	ARG_UNUSED(interval);
	return out_cap;
}

int app_cmd_build_history_frame(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				uint32_t t0, uint32_t present, uint32_t interval, bool synced,
				const uint8_t *samples, size_t samples_len, uint8_t *out,
				size_t out_cap, size_t *out_len)
{
	ARG_UNUSED(seq);
	ARG_UNUSED(frame_index);
	ARG_UNUSED(frame_count);
	ARG_UNUSED(t0);
	ARG_UNUSED(present);
	ARG_UNUSED(interval);
	ARG_UNUSED(synced);
	ARG_UNUSED(samples);
	ARG_UNUSED(samples_len);
	ARG_UNUSED(out);
	ARG_UNUSED(out_cap);
	if (out_len) {
		*out_len = 0;
	}
	return 0;
}

int app_cmd_handle(int transport, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
		   size_t *out_len, int *action)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(in);
	ARG_UNUSED(in_len);
	ARG_UNUSED(out);
	ARG_UNUSED(out_cap);
	if (out_len) {
		*out_len = 0;
	}
	ARG_UNUSED(action);
	return 0;
}

int app_clock_set_unix(uint32_t unix_time)
{
	ARG_UNUSED(unix_time);
	return 0;
}
