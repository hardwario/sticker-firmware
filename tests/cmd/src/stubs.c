/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fixtures + stubs for the app_cmd test. The real app_config_ingest.c is linked
 * (so range validation is exercised); everything else app_cmd reaches is stubbed.
 */

#include "app_alarm.h"
#include "app_alarm_rules.h"
#include "app_config.h"
#include "app_history.h"
#include "app_lrw.h"
#include "app_nfc.h"
#include "app_sensor.h"

#include "src/app_config.pb.h"

#include <zephyr/kernel.h>

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Config under test — seeded per test; app_config_ingest writes through here. */
struct app_config g_app_config;

struct app_config *app_config(void)
{
	return &g_app_config;
}

/* F-1 staging-dirty guard: the flag + test_set_lrw_dirty() hook now live in the
 * real app_cmd.c (linked here), so no stub is needed. */

/* Sensor sampling + telemetry snapshot (sample command). app_sensor_sample is
 * inert; app_compose_snapshot fills a recognisable reading so the test can check
 * the Sample response carries the snapshot. */
void app_sensor_sample(void)
{
}

void app_compose_snapshot(Telemetry *out)
{
	if (!out) {
		return;
	}
	*out = (Telemetry)Telemetry_init_zero;
	out->has_voltage = true;
	out->voltage = 165; /* 3.30 V x50 */
	out->has_temperature = true;
	out->temperature = 2345; /* 23.45 °C x100 */
}

/* Clock (APP_CMD_HAVE_CLOCK). */
uint32_t test_clock_unix;
bool test_clock_has;

int app_clock_get_unix(uint32_t *unix_s)
{
	if (!test_clock_has) {
		return -1;
	}
	*unix_s = test_clock_unix;
	return 0;
}

int app_clock_set_unix(uint32_t unix_s)
{
	test_clock_unix = unix_s;
	test_clock_has = true;
	return 0;
}

void app_clock_force_resync(void)
{
}

/* Battery (GetInfo battery field). get_info now reads the cached
 * g_app_sensor_data.voltage rather than a fresh app_battery_measure(), so the
 * test seeds the cache (see main.c setUp). The measure stub + test_battery_v are
 * kept for any caller that still measures directly. */
struct app_sensor_data g_app_sensor_data = {.voltage = NAN};
K_MUTEX_DEFINE(g_app_sensor_data_lock);

float test_battery_v = 3.3f;
int test_battery_ret;

int app_battery_measure(float *voltage)
{
	if (test_battery_ret == 0 && voltage) {
		*voltage = test_battery_v;
	}
	return test_battery_ret;
}

/* Counters. */
void app_hall_reset_count(bool left, bool right)
{
	(void)left;
	(void)right;
}

void app_input_reset_count(bool input_a, bool input_b)
{
	(void)input_a;
	(void)input_b;
}

/* History (APP_CMD_HAVE_HISTORY) — app_cmd only calls export here. */
size_t app_history_export(uint32_t from_unix, uint32_t to_unix, uint8_t *buf, size_t cap,
			  uint32_t *t0_out, uint16_t *n_written, uint16_t *total)
{
	(void)from_unix;
	(void)to_unix;
	(void)buf;
	(void)cap;
	if (t0_out) {
		*t0_out = 0;
	}
	if (n_written) {
		*n_written = 0;
	}
	if (total) {
		*total = 0;
	}
	return 0;
}

/* Dynamic alarm rules — app_cmd's handle_alarm_rule mutates the list; the unit
 * test only checks the command path, so these are inert. */
int app_alarm_rules_set(uint8_t slot, const struct app_alarm_rule *rule)
{
	(void)slot;
	(void)rule;
	return 0;
}

int app_alarm_rules_clear(uint8_t slot)
{
	(void)slot;
	return 0;
}

void app_alarm_rules_clear_all(void)
{
}

/* handle_set_param refreshes the runtime rule cache from config after an apply;
 * inert in the unit test. Returns the count of invalid slots dropped (H-10);
 * 0 here so a staged alarms batch always acks in the unit test. */
int app_alarm_rules_reload_from_config(void)
{
	return 0;
}

/* Read-back path (handle_req_alarm_rules): no rules in the unit test, so the
 * dump comes back empty. quantity_kind only needs to resolve for the linker.
 * Signature follows the M-6 lock-copy API (bool + out param). */
bool app_alarm_rules_get(uint8_t slot, struct app_alarm_rule *out)
{
	(void)slot;
	(void)out;
	return false;
}

enum app_alarm_kind app_alarm_quantity_kind(enum app_alarm_quantity q)
{
	(void)q;
	return APP_ALARM_KIND_THRESHOLD;
}

enum app_lrw_state app_lrw_get_state(void)
{
	return APP_LRW_STATE_HEALTHY;
}

/* device_status inputs: app_cmd_get_info() aggregates these into the status
 * bitmask. Stubbed to the "all healthy / nothing active" baseline. */
uint32_t app_alarm_status_flags(void)
{
	return 0;
}

bool app_nfc_ready(void)
{
	return true;
}

bool app_history_is_ready(void)
{
	return true;
}

bool app_sensor_i2c_wedged(void)
{
	return false;
}
