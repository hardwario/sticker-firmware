/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal stubs so app_radio_p2p.c links on native_sim: the global config it reads
 * and the app_compose entry points it calls. The pure-logic tests never drive
 * a real send/compose, so these can be trivial. app_ccm.c is the REAL source
 * (linked in CMakeLists) -- the frame codec tests need genuine CCM. The history
 * replay moved to app_radio (doc/plan/460 F3c), tested in tests/radio_common.
 */

#include "app_compose.h"
#include "app_config.h"
#include "app_radio.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/toolchain.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct app_config g_app_config;

/* Counts every telemetry compose the P2P send path attempted. */
int g_compose_budget_calls;
/* Frame length the compose stub reports (0 = nothing to send), and how many
 * times the P2P send path reset the snapshot. */
size_t test_compose_len;
int g_compose_reset_calls;

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

/* The Hello's reset_reason byte (app_cmd.c is not built here). */
uint32_t test_reset_cause = 0x08; /* RESET_POR */

uint32_t app_cmd_get_reset_cause(void)
{
	return test_reset_cause;
}

/* Last value handed to the network-time setter (0 = none), for the TimeAns
 * tests. */
uint32_t g_test_network_time;

int app_clock_set_network_time(uint32_t unix_time)
{
	g_test_network_time = unix_time;
	return 0;
}

/* Common announce (app_radio.c is not built here): count the calls so tests
 * can check that the backend hands it to app_radio. */
int p2p_test_announce_calls;

void app_radio_announce(void)
{
	p2p_test_announce_calls++;
}

/* The common network-time state (F3d): the backend reads the wanted flag and
 * reports a landed time; tests drive the one and count the other. */
bool p2p_test_time_wanted;
int p2p_test_time_events;

bool app_radio_time_wanted(void)
{
	return p2p_test_time_wanted;
}

void app_radio_time_event(void)
{
	p2p_test_time_events++;
}

/* The radio work queue app_radio.c owns (doc/plan/439 T2a) -- initialised but
 * never started. Every handler under test is driven on the test thread by its
 * hook (p2p_test_join_step(), p2p_test_ctrl_run()), so nothing the module
 * schedules can run underneath the assertions: a submit to a queue that is not
 * started is refused, and a delayed one keeps its timer for
 * p2p_test_join_pending_ms() to read. */
static struct k_work_q m_radio_wq;

struct k_work_q *app_radio_work_q(void)
{
	return &m_radio_wq;
}

static int radio_wq_init(void)
{
	k_work_queue_init(&m_radio_wq);
	return 0;
}

SYS_INIT(radio_wq_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* RadioState push API (app_radio.c is not built here): record what the backend
 * reports so tests can check it (#446). */
int16_t p2p_test_dl_rssi;
int8_t p2p_test_dl_snr;
int p2p_test_dl_calls;
int p2p_test_link_ups;
int p2p_test_link_ok_calls;
int p2p_test_link_fail_calls;
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

/* The LinkCheckAns numbers the backend pushes (plan §7.6). */
int16_t p2p_test_ul_rssi;
int8_t p2p_test_ul_snr;
uint8_t p2p_test_ul_margin;
uint8_t p2p_test_ul_gw_count;

void app_radio_set_uplink_rssi(int16_t rssi, int8_t snr)
{
	p2p_test_ul_rssi = rssi;
	p2p_test_ul_snr = snr;
}

void app_radio_set_uplink_margin(uint8_t margin, uint8_t gw_count)
{
	p2p_test_ul_margin = margin;
	p2p_test_ul_gw_count = gw_count;
}

void app_radio_set_session(uint32_t dev_addr, uint32_t fcnt_up)
{
	ARG_UNUSED(dev_addr);
	ARG_UNUSED(fcnt_up);
}

/* Link supervision lives in app_radio (tests/radio_common); the backend only
 * reports what it saw. */
void app_radio_link_up(void)
{
	p2p_test_link_ups++;
}

void app_radio_link_result(bool ok)
{
	if (ok) {
		p2p_test_link_ok_calls++;
	} else {
		p2p_test_link_fail_calls++;
	}
}

void app_radio_note_send(bool sent, bool duty_held)
{
	ARG_UNUSED(sent);
	ARG_UNUSED(duty_held);
}

void app_radio_note_uplink(void)
{
}

void app_radio_heartbeat_start(void)
{
}

void app_radio_heartbeat_feed(void)
{
}

void app_radio_set_join_attempts(uint32_t n)
{
	p2p_test_join_attempts = n;
}

void app_radio_set_duty_held(bool held)
{
	ARG_UNUSED(held);
}

/* The common duty ledger (app_radio.c, T2d), stubbed: a settable hold for the
 * join path, and a record of what the backend charged. */
int64_t test_duty_wait_ms;
uint32_t test_duty_charges;
uint64_t test_duty_charged_ms;
uint32_t test_duty_budget_ms;

void app_radio_duty_init(uint32_t budget_ms)
{
	test_duty_budget_ms = budget_ms;
}

int64_t app_radio_duty_wait_ms(uint32_t air_ms)
{
	ARG_UNUSED(air_ms);
	return test_duty_wait_ms;
}

void app_radio_duty_charge(uint32_t air_ms)
{
	test_duty_charges++;
	test_duty_charged_ms += air_ms;
}

void app_radio_reset_link(void)
{
}

/* Common downlink path (app_radio.c, doc/plan/460 F3): a received command is
 * handed over here; its dispatch is tested in tests/radio_common. The last one
 * is kept for the envelope checks. */
int p2p_test_downlinks;
uint8_t p2p_test_downlink_buf[80];
size_t p2p_test_downlink_len;

void app_radio_downlink(const uint8_t *buf, size_t len)
{
	p2p_test_downlink_len = MIN(len, sizeof(p2p_test_downlink_buf));
	memcpy(p2p_test_downlink_buf, buf, p2p_test_downlink_len);
	p2p_test_downlinks++;
}

/* Common TX core (app_radio.c is not built here, doc/plan/460 F4): record what
 * the backend queues and how often it kicks the scheduler; its queues, retries
 * and report split are tested in tests/radio_common. */
int p2p_test_tx_queue_calls;
int p2p_test_tx_kick_calls;
enum app_radio_frame_kind p2p_test_tx_queue_kind;

int app_radio_tx_queue(enum app_radio_frame_kind kind, enum app_radio_frame_tag tag, uint8_t port,
		       const uint8_t *buf, size_t len)
{
	ARG_UNUSED(tag);
	ARG_UNUSED(port);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	p2p_test_tx_queue_calls++;
	p2p_test_tx_queue_kind = kind;
	return 0;
}

void app_radio_tx_kick(void)
{
	p2p_test_tx_kick_calls++;
}

bool app_radio_ack_pending(void)
{
	return false;
}

bool app_radio_tx_answer_pending(void)
{
	return false;
}

uint32_t app_radio_tx_answer_free(void)
{
	return APP_RADIO_TX_QUEUE_DEPTH;
}

size_t app_radio_tx_answer_cap(size_t buf_size)
{
	return MIN(buf_size, (size_t)APP_RADIO_TX_SLOT_SIZE);
}

/* ---- settings: a RAM stub store (CONFIG_SETTINGS_CUSTOM). Saves succeed unless
 * a case sets test_settings_save_ret; nothing is kept or loaded back. ---- */

int test_settings_save_ret;

static int stub_store_load(struct settings_store *cs, const struct settings_load_arg *arg)
{
	ARG_UNUSED(cs);
	ARG_UNUSED(arg);
	return 0;
}

static int stub_store_save(struct settings_store *cs, const char *name, const char *value,
			   size_t val_len)
{
	ARG_UNUSED(cs);
	ARG_UNUSED(name);
	ARG_UNUSED(value);
	ARG_UNUSED(val_len);
	return test_settings_save_ret;
}

static const struct settings_store_itf stub_store_itf = {
	.csi_load = stub_store_load,
	.csi_save = stub_store_save,
};

static struct settings_store stub_store = {.cs_itf = &stub_store_itf};

int settings_backend_init(void)
{
	settings_dst_register(&stub_store);
	settings_src_register(&stub_store);
	return 0;
}

/* Nothing in this suite runs main(): bring the settings subsystem up here so
 * the stub store is the save destination. */
static int stub_settings_init(void)
{
	return settings_subsys_init();
}

SYS_INIT(stub_settings_init, APPLICATION, 0);

/* app_radio's flash/exchange gate: counted, so a test can check that every
 * exchange a TX began is ended again. */
int p2p_test_air_begins;
int p2p_test_air_ends;

void app_radio_air_begin(void)
{
	p2p_test_air_begins++;
}

void app_radio_air_end(void)
{
	p2p_test_air_ends++;
}
