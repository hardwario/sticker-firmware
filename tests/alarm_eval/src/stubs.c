/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal environment for app_alarm.c host tests (#348 dwell/confirm/hold
 * state machine): the config store, sensor-data globals, and the hall/input/
 * clock/report call surface app_alarm.c uses. CONFIG_LORAWAN and CONFIG_SHELL
 * are off in this test's prj.conf, so the LoRaWAN send path and the shell
 * commands (and their extra dependencies, e.g. app_sensor_sample()) are
 * compiled out entirely — only what's left is stubbed here.
 */

#include "app_alarm_rules.h"
#include "app_clock.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_lrw.h"
#include "app_sensor.h"
#include "app_w1_slots.h"

#include <zephyr/kernel.h>

#include <errno.h>

/* ---- config ---- */

struct app_config g_app_config;

struct app_config *app_config(void)
{
	return &g_app_config;
}

int settings_save(void)
{
	return 0;
}

/* ---- sensor data (THRESHOLD quantities read this; unused by the STATE
 * tests but must exist for app_alarm_poll()'s no-data sweep to link/run). */

struct app_sensor_data g_app_sensor_data;
K_MUTEX_DEFINE(g_app_sensor_data_lock);

/* ---- hall / input GPIO — tests set these to drive STATE rules ---- */

struct app_hall_data test_hall;
struct app_input_data test_input;

int app_hall_get_data(struct app_hall_data *data)
{
	*data = test_hall;
	return 0;
}

int app_input_get_data(struct app_input_data *data)
{
	*data = test_input;
	return 0;
}

/* ---- 1-Wire slots (no-data sweep gate; short-circuited by
 * g_app_config.cap_w1_sensors == false, but must exist to link). ---- */

bool app_w1_slot_is_configured(int slot)
{
	(void)slot;
	return false;
}

/* ---- clock: report "not synced" so alarm timestamps stay uptime-relative,
 * same as a bench device that never joined. ---- */

int app_clock_get_unix(uint32_t *unix_s)
{
	(void)unix_s;
	return -1;
}

/* ---- LoRaWAN (CONFIG_LORAWAN is off, so alarm_lrw_send() compiles to an
 * empty function and never calls these — kept only so the batch-flush code,
 * which is NOT LoRaWAN-gated, links). ---- */

uint8_t app_lrw_get_max_payload(void)
{
	return 51;
}

int app_lrw_send_alarm(const uint8_t *buf, size_t len)
{
	(void)buf;
	(void)len;
	return 0;
}

/* ---- alarm-report encoding: alarm_batch_flush() calls this unconditionally
 * (not LoRaWAN-gated) once the batch work item fires. Succeeds so the batch
 * counter resets, and appends every flushed event into a test-visible capture
 * buffer so cases can assert on emitted activate/deactivate edges (with
 * g_app_config.alarm_limit = 0 the flush — and so the capture — happens
 * synchronously inside app_alarm_poll()). ---- */

struct app_cmd_alarm_event test_alarm_events[16];
size_t test_alarm_event_count;

int app_cmd_build_alarm_report(uint32_t base_time, uint32_t total, bool time_synced,
			       const struct app_cmd_alarm_event *events, size_t n_events,
			       uint8_t *out, size_t out_cap, size_t *out_len)
{
	(void)base_time;
	(void)total;
	(void)time_synced;
	if (!out || !out_len || out_cap == 0) {
		return -EINVAL;
	}
	for (size_t i = 0; i < n_events; i++) {
		if (test_alarm_event_count < ARRAY_SIZE(test_alarm_events)) {
			test_alarm_events[test_alarm_event_count++] = events[i];
		}
	}
	out[0] = 0;
	*out_len = 1;
	return 0;
}
