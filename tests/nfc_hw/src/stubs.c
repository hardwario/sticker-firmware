/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fixtures + stubs for the app_nfc_hw test. The real app_nfc.c, app_nfc_parser.c,
 * app_ccm.c, and app_cmd.c (+ app_config_ingest.c) are linked; everything else
 * they reach is stubbed here — largely mirroring tests/cmd/src/stubs.c (app_cmd's
 * own dependency set), minus the app_nfc stubs it needs (real app_nfc.c is linked
 * here instead), plus the handful of things ONLY app_nfc.c itself calls
 * (app_led_set, the app_settings_* reset/save ladder).
 */

#include "app_alarm.h"
#include "app_alarm_rules.h"
#include "app_buzzer.h"
#include "app_config.h"
#include "app_history.h"
#include "app_led.h"
#include "app_lrw.h"
#include "app_sensor.h"
#include "app_settings.h"

#include "src/app_config.pb.h"

#include <zephyr/kernel.h>

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Config under test — seeded per test; app_config_ingest/app_nfc write through
 * here. app_config() and g_app_config share one struct (a test simplification
 * of the real m_app_config/g_app_config split in app_config.c). */
struct app_config g_app_config;

struct app_config *app_config(void)
{
	return &g_app_config;
}

/* Sensor sampling + telemetry snapshot (sample command via app_cmd). */
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
	out->voltage = 165;
	out->has_temperature = true;
	out->temperature = 2345;
}

/* Clock (APP_CMD_HAVE_CLOCK / app_nfc's clock_sync command). */
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

/* Battery (GetInfo battery field). */
struct app_sensor_data g_app_sensor_data = {.voltage = NAN};
K_MUTEX_DEFINE(g_app_sensor_data_lock);

int app_battery_measure(float *voltage)
{
	if (voltage) {
		*voltage = 3.3f;
	}
	return 0;
}

/* #338 buzzer_play command — inert, no test here exercises it. */
int app_buzzer_play_repeating(uint32_t kind, uint16_t repeat_s)
{
	(void)kind;
	(void)repeat_s;
	return 0;
}

/* Counters — reset_counters command. */
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

/* History — req_history_page (NFC-only, #260). Empty backend: export writes
 * nothing, next_ord stays at start_ord (has_more false). */
size_t app_history_export_page(uint32_t from_unix, uint32_t to_unix, size_t start_ord, uint8_t *buf,
			       size_t cap, uint32_t *t0_out, uint16_t *n_written, size_t *next_ord)
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
	if (next_ord) {
		*next_ord = start_ord;
	}
	return 0;
}

uint16_t app_history_count_frames(uint32_t from_unix, uint32_t to_unix, size_t cap)
{
	(void)from_unix;
	(void)to_unix;
	(void)cap;
	return 0;
}

size_t app_history_count(void)
{
	return 0;
}

uint32_t app_history_get_mask(void)
{
	return 0;
}

uint32_t app_history_get_interval(void)
{
	return 0;
}

bool app_history_base_synced(void)
{
	return false;
}

bool app_history_is_ready(void)
{
	return true;
}

/* Dynamic alarm rules — inert; no test here exercises alarm_rule mutation. */
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

int app_alarm_rules_reload_from_config(void)
{
	return 0;
}

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

uint32_t app_alarm_status_flags(void)
{
	return 0;
}

size_t app_alarm_active_snapshot(struct app_alarm_active *out, size_t max)
{
	(void)out;
	(void)max;
	return 0;
}

bool app_sensor_i2c_wedged(void)
{
	return false;
}

/* LED signalling (app_nfc's nfc_led_* helpers) — no test here asserts on LED
 * channel/state, only that app_nfc.c's calls into it don't crash the link. */
int g_led_set_calls;

void app_led_set(enum app_led_channel ch, int state)
{
	(void)ch;
	(void)state;
	g_led_set_calls++;
}

/* Reset/save ladder — app_nfc.c only reaches these via its deferred-action
 * dispatch (app_nfc_take_cmd_action() consumers), not via anything the first
 * slice of tests here drives. Record calls so a future test can assert on
 * dispatch without needing the real app_settings.c (which pulls in flash_map/
 * NVS/reboot/most other app_* modules). */
int test_settings_save_calls;
int test_settings_device_reset_calls;
int test_settings_factory_reset_calls;
int test_settings_vendor_reset_calls;

int app_settings_save(bool reboot)
{
	(void)reboot;
	test_settings_save_calls++;
	return 0;
}

int app_settings_device_reset(void)
{
	test_settings_device_reset_calls++;
	return 0;
}

int app_settings_factory_reset(void)
{
	test_settings_factory_reset_calls++;
	return 0;
}

int app_settings_vendor_reset(const uint8_t *new_secret_key)
{
	(void)new_secret_key;
	test_settings_vendor_reset_calls++;
	return 0;
}

int app_settings_save_nonce_counter(void)
{
	return 0;
}
