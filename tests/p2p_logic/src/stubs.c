/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal stubs so app_radio_p2p.c links on native_sim: the global config it reads
 * and the app_compose entry points it calls. The pure-logic tests never drive
 * a real send/compose, so these can be trivial. app_ccm.c is the REAL source
 * (linked in CMakeLists) -- the frame codec tests need genuine CCM.
 *
 * The B8 history-replay tests reference app_radio_p2p_start_history_replay, which
 * makes the whole replay call graph live (it is otherwise dropped by
 * --gc-sections, which is why this file used to need only three stubs). Hence
 * the app_history and app_cmd set below: inert by default, with a couple of
 * `test_*` knobs and `g_*_calls` counters so a test can steer the path and see
 * what it touched.
 */

#include "app_compose.h"
#include "app_config.h"
#include "app_radio.h"
#include "app_settings.h"

#include <zephyr/toolchain.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_config g_app_config;

/* ---- B8 replay knobs, driven by tests/p2p_logic/src/main.c ---- */

/* How many frames app_history_count_frames() claims the window holds. */
uint16_t test_history_frame_count;
/* The stored span app_history_span() reports, in absolute ordinals: first
 * record and end (exclusive). The end drives the replay's terminator. */
uint32_t test_history_first_abs;
size_t test_history_count = 1;
/* Counts every telemetry compose the P2P send path attempted. */
int g_compose_budget_calls;
/* Frame length the compose stub reports (0 = nothing to send), and how many
 * times the P2P send path reset the snapshot. */
size_t test_compose_len;
int g_compose_reset_calls;
/* Tracks the last app_history_set_replay_active() argument. */
bool g_history_replay_active;

int app_compose_budget(uint8_t *buf, size_t size, size_t *len, bool *more, uint8_t budget)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(size);
	ARG_UNUSED(budget);
	g_compose_budget_calls++;
	if (len) {
		*len = test_compose_len < size ? test_compose_len : size;
	}
	if (more) {
		*more = false;
	}
	return 0;
}

void app_compose_reset(void)
{
	g_compose_reset_calls++;
}

/* ---- app_history / app_cmd, for the B8 replay call graph ---- */

uint16_t app_history_count_frames(uint32_t from_unix, uint32_t to_unix, size_t cap)
{
	ARG_UNUSED(from_unix);
	ARG_UNUSED(to_unix);
	ARG_UNUSED(cap);
	return test_history_frame_count;
}

void app_history_span(uint32_t *first_abs, uint32_t *end_abs)
{
	if (first_abs) {
		*first_abs = test_history_first_abs;
	}
	if (end_abs) {
		*end_abs = (uint32_t)test_history_count;
	}
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
	g_history_replay_active = active;
}

size_t app_history_export_abs(uint32_t from_unix, uint32_t to_unix, uint32_t start_abs,
			      uint32_t end_abs, uint8_t *out, size_t cap, uint32_t *t0,
			      bool *synced, uint16_t *n, uint32_t *next)
{
	ARG_UNUSED(from_unix);
	ARG_UNUSED(to_unix);
	ARG_UNUSED(end_abs);
	ARG_UNUSED(out);
	ARG_UNUSED(cap);
	if (t0) {
		*t0 = 0;
	}
	if (synced) {
		*synced = true;
	}
	if (n) {
		*n = 0; /* no records -- the replay terminates on the first pass */
	}
	if (next) {
		*next = start_abs;
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

/* #425 page stream (the P2P driver in app_radio_p2p.c): no stream in these tests. */
int app_cmd_stream_next(uint8_t *out, size_t out_cap, size_t *out_len)
{
	ARG_UNUSED(out);
	ARG_UNUSED(out_cap);
	ARG_UNUSED(out_len);
	return -ENODATA;
}

void app_cmd_stream_cancel(void)
{
}

/* ---- app_settings, for p2p_join_adopt_sf's persist ---- */

/* The SF the last call was asked to persist. */
int g_test_saved_sf;
/* How many calls the module made -- an unchanged SF must make none. */
int g_test_save_sf_calls;
/* What the save returns; set to an errno to exercise the failure path. */
int test_save_sf_ret;

int app_settings_save_p2p_spreading_factor(int sf)
{
	g_test_saved_sf = sf;
	g_test_save_sf_calls++;
	return test_save_sf_ret;
}

/* Last value handed to the network-time setter (0 = none), for the Ack time
 * tail tests. */
uint32_t g_test_network_time;

int app_clock_set_network_time(uint32_t unix_time)
{
	g_test_network_time = unix_time;
	return 0;
}

/* Common announce / clock-sync answer (app_radio.c is not built here): count
 * the calls so tests can check that the backend hands them to app_radio. */
int p2p_test_announce_calls;
int p2p_test_send_info_calls;
uint32_t p2p_test_send_info_seq;

void app_radio_announce(void)
{
	p2p_test_announce_calls++;
}

int app_radio_send_info(uint32_t seq)
{
	p2p_test_send_info_calls++;
	p2p_test_send_info_seq = seq;
	return 0;
}

/* RadioState push API (app_radio.c is not built here): record what the backend
 * reports so tests can check it (#446). */
int16_t p2p_test_dl_rssi;
int8_t p2p_test_dl_snr;
int p2p_test_dl_calls;
uint32_t p2p_test_fail_streak;
uint32_t p2p_test_join_attempts;
uint32_t p2p_test_cnt[APP_RADIO_CNT_COUNT];

void app_radio_count(enum app_radio_counter c)
{
	if (c < APP_RADIO_CNT_COUNT) {
		p2p_test_cnt[c]++;
	}
}

void app_radio_note_downlink(int16_t rssi, int8_t snr)
{
	p2p_test_dl_rssi = rssi;
	p2p_test_dl_snr = snr;
	p2p_test_dl_calls++;
}

void app_radio_set_params(uint8_t sf, int datarate, int8_t tx_power_dbm)
{
	ARG_UNUSED(sf);
	ARG_UNUSED(datarate);
	ARG_UNUSED(tx_power_dbm);
}

void app_radio_set_uplink_rssi(int16_t rssi, int8_t snr)
{
	ARG_UNUSED(rssi);
	ARG_UNUSED(snr);
}

void app_radio_set_session(uint32_t dev_addr, uint32_t fcnt_up)
{
	ARG_UNUSED(dev_addr);
	ARG_UNUSED(fcnt_up);
}

void app_radio_set_fail_streak(uint32_t n)
{
	p2p_test_fail_streak = n;
}

void app_radio_set_join_attempts(uint32_t n)
{
	p2p_test_join_attempts = n;
}

void app_radio_set_duty_held(bool held)
{
	ARG_UNUSED(held);
}

void app_radio_set_airtime(uint32_t ms)
{
	ARG_UNUSED(ms);
}

void app_radio_reset_link(void)
{
}

bool app_radio_announce_run(void)
{
	return false;
}

bool app_radio_announce_pending(void)
{
	return false;
}
