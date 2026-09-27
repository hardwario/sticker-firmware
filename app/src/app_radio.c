/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
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

/* Both work items are defined statically, not in app_radio_init(): calibration
 * mode brings LoRaWAN up through app_radio_lrw_init() alone, and its join still
 * reaches app_radio_announce() -- a delayable armed before its init faults on
 * a NULL handler (review of #400, 2026-09-27). */
static void jitter_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_jitter_work, jitter_work_handler);
/* The boot/join announce waits out the same fleet jitter (below). */
static void announce_jitter_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_announce_jitter_work, announce_jitter_work_handler);
/* Releases the boot/join data hold at its fallback deadline (below). */
static void seq_deadline_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_seq_deadline_work, seq_deadline_work_handler);

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

/* Boot/join order (Hynek, 2026-09-27): after a link-up the node sends the Info,
 * then the settings-info, then its first telemetry -- always in that order.
 * The announce spread moves the start of the whole sequence, never one frame
 * of it. From app_radio_announce() until every announce page has been handed
 * to the backend (m_seq_closed), data waits: a report is held and leaves right
 * after the announce with no jitter of its own, and app_alarm holds its batch
 * (app_radio_data_hold_ms()) until seq_release() flushes it, ahead of the
 * report. Each backend sends queued answers and alarms before telemetry, so
 * the air order follows. Held data goes anyway once ANNOUNCE_HOLD_MAX_MS pass
 * after the spread (m_seq_deadline_work releases the sequence), so an announce
 * that cannot get out (no budget, no room) never silences the node. Timing is
 * kept in work items, not in 32-bit uptime arithmetic, so nothing misreads a
 * deadline after 24.8 days of uptime. */
#define ANNOUNCE_HOLD_MAX_MS 60000

static atomic_t m_seq_closed;     /* the boot/join sequence is still announcing */
static atomic_t m_telemetry_held; /* a report waits for the announce */

/* The backend's queued answers have left. P2P shares one small TX queue
 * between answers and alarms, so data is released only once the announce
 * frames are out of it -- an alarm queued behind them was dropped as "TX queue
 * full" (HIL 2026-09-27). LoRaWAN queues alarms separately and drains answers
 * first, so it needs no wait. */
static bool backend_tx_idle(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_tx_idle();
	}
#endif
	return true;
}

/* ms the boot/join sequence still holds data (0 = none): the time left to its
 * fallback deadline, at least 1 while it is closed. */
static int32_t seq_hold_ms(void)
{
	if (!atomic_get(&m_seq_closed)) {
		return 0;
	}

	uint32_t left = k_ticks_to_ms_ceil32(k_work_delayable_remaining_get(&m_seq_deadline_work));

	return (int32_t)CLAMP(left, 1U, (uint32_t)INT32_MAX);
}

int32_t app_radio_data_hold_ms(void)
{
	if (!app_radio_is_ready()) {
		return -1; /* the next link-up's announce releases it */
	}
	return seq_hold_ms();
}

static void seq_release(void)
{
	if (!atomic_cas(&m_seq_closed, 1, 0)) {
		return;
	}
	(void)k_work_cancel_delayable(&m_seq_deadline_work);
	/* Alarms first, then the report: both work items run on the system work
	 * queue in this order. */
	app_alarm_flush_held();
	if (atomic_cas(&m_telemetry_held, 1, 0)) {
		k_work_reschedule(&m_jitter_work, K_NO_WAIT);
	}
}

static void seq_deadline_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (atomic_get(&m_seq_closed)) {
		LOG_WRN("Announce not out after %d s: held data goes first",
			ANNOUNCE_HOLD_MAX_MS / 1000);
		seq_release();
	}
}

/* The backend composes and sends at once; the delay was taken here. */
static void jitter_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	atomic_clear(&m_telemetry_held);
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

/* Uplink phase (O9, p2p_link_check.md §3.7, Hynek 2026-09-27): the report
 * cadence runs on wall-clock slots (F27), so without it a whole fleet sends in
 * the same few seconds of every interval -- on P2P's one channel two nodes
 * rebooted together collided every minute (F-P2P-5). Each node therefore sends
 * at a stable offset derived from its DevEUI, inside
 * min(interval_report - fleet jitter - 1 s, UPLINK_PHASE_MAX_SEC); the fleet
 * jitter still comes on top. The history records keep their slots; only the
 * transmission moves. Both radios, one policy. */
#define UPLINK_PHASE_MAX_SEC 60

/* A random delay of up to min(interval_report / 10, TX_JITTER_MAX_SEC). */
static uint32_t fleet_jitter_span_ms(void)
{
	uint32_t span_ms = (uint32_t)g_app_config.interval_report * 100U; /* interval/10 */

	return MIN(span_ms, (uint32_t)TX_JITTER_MAX_SEC * 1000U);
}

static uint32_t uplink_phase_ms(void)
{
	uint32_t interval_ms = (uint32_t)g_app_config.interval_report * 1000U;
	uint32_t room_ms = interval_ms - MIN(interval_ms, fleet_jitter_span_ms() + 1000U);
	uint32_t span_ms = MIN(room_ms, (uint32_t)UPLINK_PHASE_MAX_SEC * 1000U);
	uint32_t h = 2166136261U; /* FNV-1a over the DevEUI */

	for (size_t i = 0; i < sizeof(g_app_config.lrw_deveui); i++) {
		h = (h ^ g_app_config.lrw_deveui[i]) * 16777619U;
	}
	return span_ms ? (h % span_ms) : 0U;
}

static uint32_t fleet_jitter_ms(void)
{
	uint32_t span_ms = fleet_jitter_span_ms();

	return span_ms ? (sys_rand32_get() % span_ms) : 0U;
}

void app_radio_send_telemetry(bool periodic)
{
	/* Flag first, then look: a seq_release() in between either sees the
	 * flag and kicks the report, or has already opened the sequence. */
	atomic_set(&m_telemetry_held, 1);
	if (seq_hold_ms() > 0) {
		return; /* follows the announce (seq_release()) */
	}
	if (!atomic_cas(&m_telemetry_held, 1, 0)) {
		return; /* seq_release() just kicked it */
	}
	uint32_t phase_ms = periodic ? uplink_phase_ms() : 0U;

	k_work_reschedule(&m_jitter_work, K_MSEC(phase_ms + fleet_jitter_ms()));
}

/* The boot/join announce is a burst (Info + settings-info pages + the first
 * telemetry, ~4 frames / ~5 s on P2P), so it spreads wider than one uplink:
 * up to min(interval_report / 2, ANNOUNCE_JITTER_MAX_SEC). The 6 s of the
 * telemetry jitter at a 60 s interval still let two Nodes rebooted together
 * overlap and starve each other's retries (F-P2P-4, 2026-09-27). */
#define ANNOUNCE_JITTER_MAX_SEC 30

static uint32_t announce_jitter_ms(void)
{
	uint32_t span_ms = (uint32_t)g_app_config.interval_report * 500U; /* interval/2 */

	span_ms = MIN(span_ms, (uint32_t)ANNOUNCE_JITTER_MAX_SEC * 1000U);
	return span_ms ? (sys_rand32_get() % span_ms) : 0U;
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
/* 1 while the fleet jitter of app_radio_announce() runs: the announce does not
 * start before m_announce_jitter_work fires. A flag, not an uptime, so a
 * re-armed Info months later is never mistaken for one still in the spread. */
static atomic_t m_announce_spreading;

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

static void announce_jitter_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	atomic_clear(&m_announce_spreading);
	announce_kick();
}

void app_radio_announce(void)
{
	/* Fleet de-correlation for the announce too: nodes rebooted by one batch
	 * of commands, or by a power outage, would otherwise all send their Info +
	 * settings-info at the same moment and collide on the channel (Northbridge
	 * "Busy", P2P 2026-09-27). */
	uint32_t delay_ms = announce_jitter_ms();

	atomic_set(&m_announce_spreading, 1);
	atomic_set(&m_seq_closed, 1);
	k_work_reschedule(&m_seq_deadline_work, K_MSEC(delay_ms + ANNOUNCE_HOLD_MAX_MS));
	atomic_or(&m_announce, ANNOUNCE_INFO | ANNOUNCE_SETTINGS);
	k_work_reschedule(&m_announce_jitter_work, K_MSEC(delay_ms));
}

bool app_radio_announce_pending(void)
{
	/* Also while the last announce pages are still streaming: the run that
	 * follows the stream's end releases the held telemetry. */
	return atomic_get(&m_announce) != 0 || atomic_get(&m_seq_closed) != 0;
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
	if (atomic_get(&m_announce_spreading)) {
		return true; /* still in the fleet jitter; its work item kicks us */
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
	if (atomic_get(&m_announce) != 0) {
		return true;
	}
	if (app_cmd_stream_active()) {
		return true; /* settings-info pages still streaming */
	}
	if (!backend_tx_idle()) {
		return true; /* the backend runs us again once its queue drained */
	}
	seq_release(); /* the announce is out: alarms, then the first telemetry */
	return false;
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
