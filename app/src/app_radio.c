/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_clock.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_log.h"
#include "app_radio_lrw.h"
#include "app_radio.h"

#if defined(CONFIG_RADIO_P2P)
#include "app_radio_p2p.h"
#endif

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <errno.h>

LOG_MODULE_REGISTER(app_radio, LOG_LEVEL_INF);

/* Fleet pre-send jitter (#267), one policy for both radios. The cap keeps a
 * long interval_report (e.g. 900 s) from delaying a report by 90 s. */
#define TX_JITTER_MAX_SEC 10

static struct k_work_delayable m_jitter_work;
static void jitter_work_handler(struct k_work *work);

/* Kept out of a static entirely when CONFIG_RADIO_P2P=n: with P2P not even
 * compiled in, the radio is always running LoRaWAN by construction (radio_mode's
 * only other option, OFF, is handled inside app_radio_lrw itself, #271), so tracking
 * a runtime "which one did we pick" has no observable use — and every byte
 * counts on the flash-tight debug build (doc/p2p.md §11). */
#if defined(CONFIG_RADIO_P2P)
static enum app_radio_kind m_kind = APP_RADIO_LORAWAN;

static inline bool is_p2p(void)
{
	return m_kind == APP_RADIO_P2P;
}
#endif

int app_radio_init(void)
{
	k_work_init_delayable(&m_jitter_work, jitter_work_handler);

#if defined(CONFIG_RADIO_P2P)
	if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_P2P) {
		m_kind = APP_RADIO_P2P;
		LOG_INF("Radio: P2P (raw LoRa)");
		return app_radio_p2p_init();
	}
	m_kind = APP_RADIO_LORAWAN;
#else
	if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_P2P) {
		LOG_WRN("radio-mode=p2p but CONFIG_RADIO_P2P=n; falling back to LoRaWAN");
	}
#endif
#if defined(CONFIG_LORAWAN)
	LOG_INF("Radio: LoRaWAN");
	return app_radio_lrw_init();
#else
	/* Neither transport compiled in for this radio_mode. Only reachable on a
	 * P2P-only build (CONFIG_LORAWAN=n, #118 phase 2 flash budget) whose
	 * radio_mode is not p2p -- which includes `off`, the factory default
	 * (app_config.yml, #350), so a factory-reset bench node lands here.
	 *
	 * Degrade rather than fail. main.c treats a non-zero app_radio_init() as
	 * fatal and die()s into a 60 s reboot loop, leaving about nine seconds of
	 * shell per cycle to fix the setting in -- which is how F-36 was found.
	 * Every other app_radio_* entry point already has a safe #else tail
	 * (is_ready -> false, send -> -ENODEV) and app_report.c gates on
	 * app_radio_is_ready(), so an idle radio is a state the rest of the
	 * application already understands. Production compiles both transports
	 * (app/Kconfig: default y), so this branch never exists there. */
	LOG_ERR("Radio: no transport for radio-mode %d in this image; radio idle",
		g_app_config.radio_mode);
	return 0;
#endif
}

void app_radio_start(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_start();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_join();
#endif
}

void app_radio_rejoin(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_rejoin();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_join(); /* already unconditional -- see app_radio_start() above */
#endif
}

enum app_radio_kind app_radio_get_kind(void)
{
#if defined(CONFIG_RADIO_P2P)
	return m_kind;
#else
	return APP_RADIO_LORAWAN;
#endif
}

enum app_radio_state app_radio_get_state(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_get_state();
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_get_state();
#else
	return APP_RADIO_STATE_IDLE;
#endif
}

/* The PA caps whatever a backend asks for: the STICKER RFO_LP path tops out at
 * rfo-lp-max-power (14 dBm), so report what actually goes out. */
#if DT_NODE_HAS_PROP(DT_NODELABEL(lora), rfo_lp_max_power)
#define RADIO_PA_MAX_DBM DT_PROP(DT_NODELABEL(lora), rfo_lp_max_power)
#else
#define RADIO_PA_MAX_DBM 22
#endif

/* RadioState data pushed by the backends (#446). m_st holds the pushed fields
 * only; state, ages, wall-clock time, counters and uptime are filled in by
 * app_radio_get_status(). Writers run on the backend work queues and the MAC
 * callbacks, readers on NFC / shell / m_work_q: the spinlock keeps a snapshot
 * consistent. */
static struct k_spinlock m_st_lock;
static struct app_radio_status m_st;
static int64_t m_dl_ms;         /* uptime of the last downlink, 0 = none */
static int64_t m_duty_since_ms; /* start of the current duty-cycle hold, 0 = none */
static atomic_t m_cnt[APP_RADIO_CNT_COUNT];

void app_radio_count(enum app_radio_counter c)
{
	if (c < APP_RADIO_CNT_COUNT) {
		atomic_inc(&m_cnt[c]);
	}
}

void app_radio_note_downlink(int16_t rssi, int8_t snr)
{
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	m_st.has_dl = true;
	m_st.dl_rssi = rssi;
	m_st.dl_snr = snr;
	m_dl_ms = MAX(k_uptime_get(), 1);
	k_spin_unlock(&m_st_lock, key);
	app_radio_count(APP_RADIO_CNT_RX);
}

void app_radio_set_params(uint8_t sf, int datarate, int8_t tx_power_dbm)
{
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	m_st.sf = sf;
	m_st.has_datarate = datarate >= 0;
	m_st.datarate = datarate >= 0 ? (uint8_t)datarate : 0;
	m_st.has_tx_power = true;
	m_st.tx_power_dbm = MIN(tx_power_dbm, RADIO_PA_MAX_DBM);
	k_spin_unlock(&m_st_lock, key);
}

void app_radio_set_uplink_rssi(int16_t rssi, int8_t snr)
{
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	m_st.has_ul_rssi = true;
	m_st.ul_rssi = rssi;
	m_st.ul_snr = snr;
	k_spin_unlock(&m_st_lock, key);
}

void app_radio_set_uplink_margin(uint8_t margin, uint8_t gw_count)
{
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	m_st.has_ul_margin = true;
	m_st.ul_margin = margin;
	m_st.ul_gw_count = gw_count;
	k_spin_unlock(&m_st_lock, key);
}

void app_radio_set_session(uint32_t dev_addr, uint32_t fcnt_up)
{
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	m_st.has_session = true;
	m_st.dev_addr = dev_addr;
	m_st.fcnt_up = fcnt_up;
	k_spin_unlock(&m_st_lock, key);
}

void app_radio_set_fail_streak(uint32_t n)
{
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	m_st.fail_streak = n;
	k_spin_unlock(&m_st_lock, key);
}

void app_radio_set_join_attempts(uint32_t n)
{
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	m_st.join_attempts = n;
	k_spin_unlock(&m_st_lock, key);
}

void app_radio_set_duty_held(bool held)
{
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	if (!held) {
		m_duty_since_ms = 0;
	} else if (m_duty_since_ms == 0) {
		m_duty_since_ms = MAX(k_uptime_get(), 1);
	}
	k_spin_unlock(&m_st_lock, key);
}

void app_radio_set_airtime(uint32_t ms)
{
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	m_st.has_airtime = true;
	m_st.airtime_hour_ms = ms;
	k_spin_unlock(&m_st_lock, key);
}

void app_radio_get_status(struct app_radio_status *st)
{
	int64_t now = k_uptime_get();
	k_spinlock_key_t key = k_spin_lock(&m_st_lock);

	*st = m_st;
	if (st->has_dl) {
		st->dl_age_s = (uint32_t)((now - m_dl_ms) / 1000);
	}
	if (m_duty_since_ms != 0) {
		st->duty_blocked_s = MAX(1U, (uint32_t)((now - m_duty_since_ms) / 1000));
	}
	k_spin_unlock(&m_st_lock, key);

	st->state = app_radio_get_state();
	st->uptime_s = (uint32_t)(now / 1000);
	for (size_t i = 0; i < APP_RADIO_CNT_COUNT; i++) {
		st->cnt[i] = (uint32_t)atomic_get(&m_cnt[i]);
	}
	if (st->has_dl) {
		uint32_t unix_now;

		if (app_clock_get_unix(&unix_now) == 0 && unix_now > st->dl_age_s) {
			st->has_dl_unix = true;
			st->dl_unix_time = unix_now - st->dl_age_s;
		}
	}
}

bool app_radio_is_ready(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_is_ready();
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_is_ready();
#else
	return false;
#endif
}

uint8_t app_radio_get_max_payload(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_get_max_payload();
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_get_max_payload();
#else
	return 0;
#endif
}

/* The backend composes and sends at once; the delay was taken here. */
static void jitter_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_send_telemetry();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_send_telemetry();
#endif
}

void app_radio_send_telemetry(void)
{
	uint32_t span_ms = (uint32_t)g_app_config.interval_report * 100U; /* interval/10 */

	span_ms = MIN(span_ms, (uint32_t)TX_JITTER_MAX_SEC * 1000U);
	uint32_t delay_ms = span_ms ? (sys_rand32_get() % span_ms) : 0U;

	k_work_reschedule(&m_jitter_work, K_MSEC(delay_ms));
}

void app_radio_send_telemetry_now(void)
{
	/* F14: a host-requested uplink targets this one device, so the fleet
	 * de-correlation buys nothing. Rescheduling to zero also folds a jittered
	 * report still pending into this send (a single pending instance). */
	k_work_reschedule(&m_jitter_work, K_NO_WAIT);
}

void app_radio_reset_link(void)
{
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_reset_nvm();
#endif
#if defined(CONFIG_RADIO_P2P)
	app_radio_p2p_forget_pairing();
#endif
}

int app_radio_queue_response(uint8_t port, const uint8_t *buf, size_t len)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_queue_response(port, buf, len);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_queue_response(port, buf, len);
#else
	return -ENODEV;
#endif
}

int app_radio_send_alarm(const uint8_t *buf, size_t len)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_send_alarm(buf, len);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_send_alarm(buf, len);
#else
	return -ENODEV;
#endif
}

void app_radio_register_ready_cb(void (*cb)(void))
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_register_ready_cb(cb);
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_register_ready_cb(cb);
#endif
}

void app_radio_suspend(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_suspend();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_suspend();
#endif
}

/* ---- Boot/join announce --------------------------------------------------- */

#define ANNOUNCE_INFO     BIT(0)
#define ANNOUNCE_SETTINGS BIT(1)

/* The response buffers of both backends are 64 B (APP_RADIO_LRW_RESPONSE_BUF_SIZE,
 * P2P_TX_BUF_SIZE); the budget below caps the page size further. */
#define ANNOUNCE_BUF_SIZE 64

static atomic_t m_announce;

/* Have the backend call app_radio_announce_run() on its work queue. */
static void announce_kick(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_announce_kick();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_announce_kick();
#endif
}

/* Payload budget (bytes) of the next response frame, at most `buf_size`. */
static size_t response_cap(size_t buf_size)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_response_cap(buf_size);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_response_cap(buf_size);
#else
	return 0;
#endif
}

static int queue_announce(bool settings, const uint8_t *buf, size_t len)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_queue_announce(settings, buf, len);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_queue_announce(settings, buf, len);
#else
	ARG_UNUSED(settings);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return -ENODEV;
#endif
}

/* Page 0 went out and more pages follow: start the backend's page stream. */
static void page_stream_kick(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_page_stream_kick();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_page_stream_kick();
#endif
}

/* Queue page 0 of an Info (settings = false) or settings-info, and start the
 * page stream when more pages follow. */
static int announce_frame(bool settings, uint32_t seq)
{
	uint8_t buf[ANNOUNCE_BUF_SIZE];
	size_t cap = response_cap(sizeof(buf));
	size_t len;
	bool more = false;
	int ret = settings ? app_cmd_build_config_status(buf, cap, &len, &more)
			   : app_cmd_build_info_seq(seq, buf, cap, &len, &more);

	if (ret) {
		return ret;
	}
	ret = queue_announce(settings, buf, len);
	if (ret) {
		if (more) {
			app_cmd_stream_cancel(); /* page 0 never left */
		}
		return ret;
	}
	if (more) {
		page_stream_kick();
	}
	return 0;
}

void app_radio_announce(void)
{
	atomic_or(&m_announce, ANNOUNCE_INFO | ANNOUNCE_SETTINGS);
	announce_kick();
}

bool app_radio_announce_pending(void)
{
	return atomic_get(&m_announce) != 0;
}

void app_radio_announce_rearm(bool settings)
{
	atomic_or(&m_announce, settings ? ANNOUNCE_SETTINGS : ANNOUNCE_INFO);
}

bool app_radio_announce_run(void)
{
	enum app_radio_state state = app_radio_get_state();

	if (state != APP_RADIO_STATE_HEALTHY && state != APP_RADIO_STATE_WARNING) {
		return false; /* the next link-up re-announces from scratch */
	}
	if (app_cmd_stream_active()) {
		return true; /* run again when the running page stream ends */
	}

	if (atomic_get(&m_announce) & ANNOUNCE_INFO) {
		if (announce_frame(false, 0) == 0) {
			atomic_and(&m_announce, ~ANNOUNCE_INFO);
			LOG_INF("Info announced");
		}
		if (app_cmd_stream_active()) {
			return true; /* settings-info follows once these pages are out */
		}
	}
	if (atomic_get(&m_announce) & ANNOUNCE_SETTINGS) {
		if (announce_frame(true, 0) == 0) {
			atomic_and(&m_announce, ~ANNOUNCE_SETTINGS);
			LOG_INF("Settings-info announced");
		}
	}
	return atomic_get(&m_announce) != 0;
}

int app_radio_send_info(uint32_t seq)
{
	int ret = announce_frame(false, seq);

	if (ret) {
		/* Not even one Info field fits now: the announce Info goes later. */
		atomic_or(&m_announce, ANNOUNCE_INFO);
	}
	return ret;
}

void app_radio_clock_sync(uint32_t seq)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_clock_sync(seq);
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_clock_sync(seq);
#else
	ARG_UNUSED(seq);
#endif
}
