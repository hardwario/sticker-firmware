/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fixtures + stubs for the app_cmd test. The real app_config_ingest.c is linked
 * (so range validation is exercised); everything else app_cmd reaches is stubbed.
 */

#include "app_alarm_rules.h"
#include "app_config.h"

#include <stddef.h>
#include <stdint.h>

/* Config under test — seeded per test; app_config_ingest writes through here. */
struct app_config g_app_config;

struct app_config *app_config(void)
{
	return &g_app_config;
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

/* Battery (GetInfo battery field). Seedable per test; default reports a value so
 * the encoded Info carries a non-zero battery. */
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
 * inert in the unit test. */
void app_alarm_rules_reload_from_config(void)
{
}

/* Read-back path (handle_req_alarm_rules): no rules in the unit test, so the
 * dump comes back empty. quantity_kind only needs to resolve for the linker. */
const struct app_alarm_rule *app_alarm_rules_get(uint8_t slot)
{
	(void)slot;
	return NULL;
}

enum app_alarm_kind app_alarm_quantity_kind(enum app_alarm_quantity q)
{
	(void)q;
	return APP_ALARM_KIND_THRESHOLD;
}
