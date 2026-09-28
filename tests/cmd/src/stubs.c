/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fixtures + stubs for the app_cmd test. The real app_config_ingest.c is linked
 * (so range validation is exercised); everything else app_cmd reaches is stubbed.
 */

#include <string.h>

#include "app_alarm.h"
#include "app_alarm_rules.h"
#include "app_buzzer.h"
#include "app_config.h"
#include "app_counters.h"
#include "app_history.h"
#include "app_radio_lrw.h"
#include "app_nfc.h"
#include "app_radio.h"
#include "app_sensor.h"
#include "app_settings.h"

#include "src/app_config.pb.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/ztest.h>

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

/* #340 M27: app_cmd_handle_set_param() now takes the real app_config_lock()/
 * _unlock() (shared with app_config.c's reset ops) instead of its own local
 * mutex — stub it here since the real app_config.c isn't linked. */
static K_MUTEX_DEFINE(m_app_config_lock);

void app_config_lock(void)
{
	k_mutex_lock(&m_app_config_lock, K_FOREVER);
}

void app_config_unlock(void)
{
	k_mutex_unlock(&m_app_config_lock);
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

void app_radio_clock_sync(uint32_t seq)
{
	ARG_UNUSED(seq);
}

void app_report_force(void)
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

/* #338 buzzer_play command. Records the args it was called with + returns
 * test_buzzer_play_ret, so the test can both check what app_cmd forwarded and
 * simulate app_buzzer.c rejecting an unknown melody id (-ENOENT). */
int g_buzzer_play_calls;
uint32_t g_buzzer_play_last_kind;
uint16_t g_buzzer_play_last_repeat_s;
int test_buzzer_play_ret;

int app_buzzer_play_repeating(uint32_t kind, uint16_t repeat_s)
{
	g_buzzer_play_calls++;
	g_buzzer_play_last_kind = kind;
	g_buzzer_play_last_repeat_s = repeat_s;
	return test_buzzer_play_ret;
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

/* History (APP_CMD_HAVE_HISTORY) — the NFC paged read (req_history_page, #260)
 * pulls from these; the unit test has no history backend, so they report an empty
 * buffer (export writes nothing, next_ord stays at start_ord => has_more false). */
size_t app_history_export_page(uint32_t from_unix, uint32_t to_unix, size_t start_ord, uint8_t *buf,
			       size_t cap, uint32_t *t0_out, bool *synced_out, uint16_t *n_written,
			       size_t *next_ord)
{
	(void)from_unix;
	(void)to_unix;
	(void)buf;
	(void)cap;
	if (t0_out) {
		*t0_out = 0;
	}
	if (synced_out) {
		*synced_out = false;
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

/* alarms_replace (SetParam field 6) empties the staged rule slots through this:
 * mirror the real implementation's effect on the config so the tests can see
 * which slots survive. */
int test_alarm_clear_all_calls;

void app_alarm_rules_clear_all(void)
{
	struct app_config *c = app_config();

	test_alarm_clear_all_calls++;
	memset(c->alarm_0, 0, sizeof(c->alarm_0));
	memset(c->alarm_1, 0, sizeof(c->alarm_1));
	memset(c->alarm_2, 0, sizeof(c->alarm_2));
	memset(c->alarm_3, 0, sizeof(c->alarm_3));
	memset(c->alarm_4, 0, sizeof(c->alarm_4));
	memset(c->alarm_5, 0, sizeof(c->alarm_5));
	memset(c->alarm_6, 0, sizeof(c->alarm_6));
	memset(c->alarm_7, 0, sizeof(c->alarm_7));
	memset(c->alarm_8, 0, sizeof(c->alarm_8));
	memset(c->alarm_9, 0, sizeof(c->alarm_9));
	memset(c->alarm_10, 0, sizeof(c->alarm_10));
	memset(c->alarm_11, 0, sizeof(c->alarm_11));
	memset(c->alarm_12, 0, sizeof(c->alarm_12));
	memset(c->alarm_13, 0, sizeof(c->alarm_13));
	memset(c->alarm_14, 0, sizeof(c->alarm_14));
	memset(c->alarm_15, 0, sizeof(c->alarm_15));
}

/* handle_set_param refreshes the runtime rule cache from config after an apply;
 * inert in the unit test. Returns the count of invalid slots dropped (H-10);
 * 0 by default so a staged alarms batch acks normally. test_alarm_reload_dropped
 * lets a test force a nonzero drop count to exercise app_cmd.c's alarm-shape
 * rollback path (the second rollback/`goto out` exit of
 * app_cmd_handle_set_param) without needing the real app_alarm_rules.c linked. */
int test_alarm_reload_dropped;

int app_alarm_rules_reload_from_config(void)
{
	return test_alarm_reload_dropped;
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

enum app_radio_state app_radio_get_state(void)
{
	return APP_RADIO_STATE_HEALTHY;
}

/* RadioState source (#446): tests set test_dl_valid + values for the last
 * downlink, and test_radio_full for every other group. */
bool test_dl_valid;
int16_t test_dl_rssi;
int8_t test_dl_snr;
uint32_t test_dl_age_s;
bool test_radio_full;

void app_radio_get_status(struct app_radio_status *st)
{
	*st = (struct app_radio_status){0};
	st->state = app_radio_get_state();
	st->uptime_s = 86400;
	if (test_dl_valid) {
		st->has_dl = true;
		st->dl_rssi = test_dl_rssi;
		st->dl_snr = test_dl_snr;
		st->dl_age_s = test_dl_age_s;
	}
	if (test_radio_full) {
		st->sf = 7;
		st->has_datarate = true;
		st->datarate = 5;
		st->has_tx_power = true;
		st->tx_power_dbm = 14;
		st->has_dl_unix = test_dl_valid;
		st->dl_unix_time = 1790449436u;
		st->has_ul_rssi = true;
		st->ul_rssi = -58;
		st->ul_snr = 12;
		st->has_ul_margin = true;
		st->ul_margin = 20;
		st->ul_gw_count = 2;
		st->has_session = true;
		st->dev_addr = 0x260B1234u;
		st->fcnt_up = 2334;
		st->fail_streak = 3;
		st->join_attempts = 1;
		st->duty_blocked_s = 120;
		st->has_airtime = true;
		st->airtime_hour_ms = 3456;
		for (size_t i = 0; i < APP_RADIO_CNT_COUNT; i++) {
			st->cnt[i] = 100 + (uint32_t)i;
		}
	}
}

/* device_status inputs: app_cmd_get_info() aggregates these into the status
 * bitmask. Stubbed to the "all healthy / nothing active" baseline. */
uint32_t app_alarm_status_flags(void)
{
	return 0;
}

/* Info.active_alarms input: defaults to "nothing active" (matching
 * app_alarm_status_flags() above), but test_set_active_alarm_count() lets a
 * test report N synthetic alarms to exercise the DR-budget trimming in
 * app_cmd_build_info() / app_cmd_handle() (#335 tier-2) without needing the
 * real rule-evaluation pipeline. Fixed non-zero (source, quantity, type) so
 * every entry costs its real 8 B on the wire. */
static size_t m_test_active_alarm_count;

void test_set_active_alarm_count(size_t n)
{
	m_test_active_alarm_count = n;
}

size_t app_alarm_active_snapshot(struct app_alarm_active *out, size_t max)
{
	size_t n = m_test_active_alarm_count;
	if (n > max) {
		n = max;
	}
	for (size_t i = 0; i < n; i++) {
		out[i].source = APP_ALARM_SRC_SLOT1;
		out[i].quantity = APP_ALARM_Q_HUMIDITY;
		out[i].type = 2; /* ALARM_TYPE_HIGH */
	}
	return n;
}

bool app_nfc_ready(void)
{
	return true;
}

bool app_nfc_mailbox_available(void)
{
	return true;
}

/* #308/#415: call counter so the test can confirm claim_done dispatch reached
 * app_nfc without linking the real app_nfc.c (its claim latch is covered by
 * tests/nfc_hw). */
int g_claim_done_calls;

void app_nfc_claim_done(void)
{
	g_claim_done_calls++;
}

/* #351/#415: mirrors g_claim_done_calls above, for the claim_active command. */
int g_claim_active_calls;

void app_nfc_claim_active(void)
{
	g_claim_active_calls++;
}

/* #415: claim window state seen by app_cmd_handle_get_claim_info(); the test
 * drives it (default ACTIVE, like a freshly provisioned unit). */
uint8_t g_claim_state = APP_NFC_CLAIM_ACTIVE;

uint8_t app_nfc_claim_state_get(void)
{
	return g_claim_state;
}

bool app_history_is_ready(void)
{
	return true;
}

bool app_sensor_i2c_wedged(void)
{
	return false;
}

/* ---- The history-replay entry point app_cmd_handle_req_history calls ---- */

int g_history_replay_start_calls;
uint32_t g_history_replay_start_from;
uint32_t g_history_replay_start_to;
uint32_t g_history_replay_start_seq;
/* What the stub reports, as app_radio_history_replay_start(): 0 = a stream runs
 * and IS the answer; -EMSGSIZE / -ENODATA / -EAGAIN = the handler answers an
 * Error instead. */
int test_history_replay_start_ret;

int app_radio_history_replay_start(uint32_t from_unix, uint32_t to_unix, uint32_t seq)
{
	g_history_replay_start_calls++;
	g_history_replay_start_from = from_unix;
	g_history_replay_start_to = to_unix;
	g_history_replay_start_seq = seq;
	return test_history_replay_start_ret;
}

/* ---- #460 F3: what the one executor, app_cmd_run_action(), drives ---- */

int test_run_settings_save_calls;
int test_run_settings_save_ret;
int test_run_device_reset_calls;
int test_run_factory_reset_calls;
int test_run_vendor_reset_calls;
const uint8_t *test_run_vendor_reset_key;
int test_run_counters_save_calls;
int test_run_rejoin_calls;
int test_run_reset_link_calls;

int app_settings_save(bool reboot)
{
	ARG_UNUSED(reboot);
	test_run_settings_save_calls++;
	return test_run_settings_save_ret;
}

int app_settings_device_reset(void)
{
	test_run_device_reset_calls++;
	return 0;
}

int app_settings_factory_reset(void)
{
	test_run_factory_reset_calls++;
	return 0;
}

int app_settings_vendor_reset(const uint8_t *new_secret_key)
{
	test_run_vendor_reset_calls++;
	test_run_vendor_reset_key = new_secret_key;
	return 0;
}

int app_counters_save(bool force)
{
	ARG_UNUSED(force);
	test_run_counters_save_calls++;
	return 0;
}

void app_radio_rejoin(void)
{
	test_run_rejoin_calls++;
}

void app_radio_reset_link(void)
{
	test_run_reset_link_calls++;
}

/* The executor tests run only the actions that return; a reboot fails them. */
FUNC_NORETURN void sys_reboot(int type)
{
	printk("unexpected sys_reboot(%d)\n", type);
	ztest_test_fail();
	for (;;) {
		k_sleep(K_FOREVER);
	}
}
