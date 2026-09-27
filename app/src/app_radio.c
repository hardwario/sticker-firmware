/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_clock.h"
#include "app_cmd.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_log.h"
#include "app_radio_lrw.h"
#include "app_radio.h"

#if defined(CONFIG_RADIO_P2P)
#include "app_radio_p2p.h"
#endif

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_radio, LOG_LEVEL_INF);

/* The radio work queue (doc/plan/439 T2a). Only one backend runs, so the two
 * 4 KB stacks they had became one. 4096 B covers the deepest paths of both:
 * lorawan_send -> LoRaMac -> nanopb encode -> AES-CCM (#187), and a P2P 0x56
 * command -- recv_ack -> app_cmd_handle -> nanopb decode/encode, ~1000 B before
 * nanopb touches the stack. Release builds carry no stack canary, so an
 * overflow here would corrupt RAM silently. */
#define RADIO_WQ_STACK_SIZE 4096

static K_THREAD_STACK_DEFINE(m_wq_stack, RADIO_WQ_STACK_SIZE);
static struct k_work_q m_wq;

struct k_work_q *app_radio_work_q(void)
{
	return &m_wq;
}

static int radio_wq_init(void)
{
	const struct k_work_queue_config cfg = {.name = "radio_wq"};

	k_work_queue_init(&m_wq);
	k_work_queue_start(&m_wq, m_wq_stack, K_THREAD_STACK_SIZEOF(m_wq_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, &cfg);
	return 0;
}

SYS_INIT(radio_wq_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

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

/* The backend the common TX path drives (doc/plan/460 §2.1). LoRaWAN until
 * app_radio_init() picks P2P, as m_kind: calibration mode brings LoRaWAN up
 * without app_radio_init(). */
#if defined(CONFIG_LORAWAN)
static const struct app_radio_backend *m_be = &app_radio_lrw_backend;
#else
static const struct app_radio_backend *m_be;
#endif

int app_radio_init(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_P2P) {
		m_kind = APP_RADIO_P2P;
		m_be = &app_radio_p2p_backend;
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

/* ---- Uplink frames, queues and scheduler (doc/plan/460 §2.2, F4) ----------
 * One TX path for both radios (decision #23). app_radio owns the answer and
 * alarm queues and the report being sent; the backend only sends one frame
 * when asked (struct app_radio_backend.send) and kicks app_radio_tx_kick() when
 * the link comes up or its confirmed uplink ends. One work item on the radio
 * work queue sends one frame per run: the frame a retry is waiting for, then
 * answers, then alarms, and telemetry only when both queues are empty -- so
 * after a link-up the Info and settings-info stay ahead of the held alarms and
 * the first report on the air, on either radio. */

/* A frame the radio refused is retried after TX_RETRY_MS, TX_MAX_RETRIES times
 * at most (LoRaWAN #219): then it is dropped, and the report's snapshot is
 * reset (#340 M6) so the next report takes a fresh reading instead of packing
 * the abandoned one's data. A queued answer or alarm held by the duty cycle
 * (-EAGAIN) is not failing: it waits as long as the hold lasts. */
#define TX_RETRY_MS    15000
#define TX_MAX_RETRIES 8

/* Sized for the largest LoRaWAN payload (EU868 DR4-6 / US915 DR4 = 242 B; P2P
 * takes up to 239 B). A smaller buffer split a report into more uplinks than
 * the data rate needs -- more TX, receive windows, duty cycle and battery (#267). */
#define TLM_BUF_SIZE 242

struct tx_slot {
	uint8_t tag; /* enum app_radio_frame_tag */
	uint8_t port;
	uint16_t len;
	uint8_t buf[APP_RADIO_TX_SLOT_SIZE];
};

K_MSGQ_DEFINE(m_answer_q, sizeof(struct tx_slot), APP_RADIO_TX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(m_alarm_q, sizeof(struct tx_slot), APP_RADIO_TX_QUEUE_DEPTH, 4);

/* The queued frame being sent: off its queue and kept here until it left or
 * was given up, so a retry keeps the order. Radio work queue only. */
static struct tx_slot m_cur;
static uint8_t m_cur_kind;
static bool m_cur_valid;
static uint8_t m_cur_retries;

/* Telemetry: a request (any thread; one pending request, as a second report
 * before the first left carries the same data) and the report being sent
 * (radio work queue only). */
static atomic_t m_tlm_requested;
static bool m_tlm_open;  /* a report is being sent */
static bool m_tlm_frame; /* m_tlm_buf holds its next frame (a retry resends it) */
static bool m_tlm_first; /* ... the report's first */
static bool m_tlm_more;
static uint8_t m_tlm_flags; /* report_flags(), fixed at the first frame */
static uint8_t m_tlm_retries;
static size_t m_tlm_len;
static uint8_t m_tlm_buf[TLM_BUF_SIZE];

static void tx_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_tx_work, tx_work_handler);

/* From the scheduler itself: the next run at this time. */
static void tx_schedule(uint32_t delay_ms)
{
	k_work_reschedule_for_queue(app_radio_work_q(), &m_tx_work, K_MSEC(delay_ms));
}

void app_radio_tx_kick(void)
{
	/* k_work_schedule: a run already waiting (duty hold, retry, frame gap)
	 * keeps its time. */
	k_work_schedule_for_queue(app_radio_work_q(), &m_tx_work, K_NO_WAIT);
}

static void tx_request_telemetry(void)
{
	atomic_set(&m_tlm_requested, 1);
	app_radio_tx_kick();
}

int app_radio_tx_queue(enum app_radio_frame_kind kind, enum app_radio_frame_tag tag, uint8_t port,
		       const uint8_t *buf, size_t len)
{
	bool alarm = kind == APP_RADIO_FRAME_ALARM;

	if (!buf || len == 0 || (!alarm && kind != APP_RADIO_FRAME_ANSWER)) {
		return -EINVAL;
	}
	if (len > APP_RADIO_TX_SLOT_SIZE) {
		LOG_ERR("%s %zu B over the %d B slot", alarm ? "Alarm batch" : "Answer", len,
			APP_RADIO_TX_SLOT_SIZE);
		return -EMSGSIZE;
	}

	struct tx_slot slot = {.tag = (uint8_t)tag, .port = port, .len = (uint16_t)len};

	memcpy(slot.buf, buf, len);
	if (k_msgq_put(alarm ? &m_alarm_q : &m_answer_q, &slot, K_NO_WAIT) != 0) {
		LOG_WRN("%s queue full; dropped", alarm ? "Alarm" : "Answer");
		return -ENOMEM;
	}
	app_radio_tx_kick();
	return 0;
}

bool app_radio_tx_answer_pending(void)
{
	return k_msgq_num_used_get(&m_answer_q) > 0 ||
	       (m_cur_valid && m_cur_kind == APP_RADIO_FRAME_ANSWER);
}

uint32_t app_radio_tx_answer_free(void)
{
	return k_msgq_num_free_get(&m_answer_q);
}

size_t app_radio_tx_answer_cap(size_t buf_size)
{
	size_t cap = MIN(buf_size, (size_t)APP_RADIO_TX_SLOT_SIZE);
	uint8_t budget = m_be ? m_be->budget() : 0;

	/* 0 = no budget known now (pending MAC answers fill the frame): encode
	 * against the slot and let the send flush the MAC and retry, instead of
	 * pretending the frame has no room at all. */
	return (budget > 0 && budget < cap) ? budget : cap;
}

/* seq of an encoded Response (version byte + protobuf): field 1 comes first
 * when non-zero (nanopb encodes in field order); absent means seq 0. */
static uint32_t response_seq(const struct tx_slot *tx)
{
	uint32_t seq = 0;

	if (tx->len < 2 || tx->buf[1] != 0x08) {
		return 0;
	}
	for (uint16_t i = 2, shift = 0; i < tx->len && shift < 32; i++, shift += 7) {
		seq |= (uint32_t)(tx->buf[i] & 0x7f) << shift;
		if (!(tx->buf[i] & 0x80)) {
			break;
		}
	}
	return seq;
}

/* #409 3g: a queued frame no longer fits because the budget fell after it was
 * encoded (LoRaWAN ADR / LinkADRReq; the P2P budget is fixed). Recover by kind
 * instead of losing it silently: the announce Info / settings-info is re-armed
 * (sent again once it fits); a command answer is replaced in place by Error
 * BUDGET_TOO_SMALL carrying the command's seq (the host retries at a higher
 * DR); an alarm is dropped, its state still rides in telemetry system_flags.
 * Returns true when `tx` now holds a frame to send. */
static bool recover_over_budget(struct tx_slot *tx, uint8_t kind, uint8_t budget)
{
	LOG_WRN("TX %u B over budget %u B (kind %u, tag %u)", tx->len, budget, kind, tx->tag);

	if (kind == APP_RADIO_FRAME_ALARM) {
		LOG_INF("Alarm frame dropped; alarm state stays in telemetry system_flags");
		return false;
	}
	switch ((enum app_radio_frame_tag)tx->tag) {
	case APP_RADIO_TAG_INFO:
		app_radio_announce_rearm(false);
		LOG_INF("Info re-armed for the deferred announce");
		return false;
	case APP_RADIO_TAG_SETTINGS:
		app_radio_announce_rearm(true);
		LOG_INF("settings-info re-armed for the deferred announce");
		return false;
	case APP_RADIO_TAG_CMD_RESPONSE: {
		uint32_t seq = response_seq(tx);
		size_t len;

		if (app_cmd_build_budget_error(seq, tx->buf, MIN(budget, sizeof(tx->buf)), &len) ==
		    0) {
			tx->len = (uint16_t)len;
			tx->tag = APP_RADIO_TAG_OTHER; /* never recover the error itself */
			LOG_INF("Command answer (seq %u) replaced by BUDGET_TOO_SMALL", seq);
			return true;
		}
		LOG_ERR("Command answer (seq %u) dropped: not even the Error fits", seq);
		return false;
	}
	default:
		LOG_ERR("Frame dropped");
		return false;
	}
}

/* Send the queued frame in progress, or the next one. Returns false when both
 * queues are empty (the run goes on to telemetry). */
static bool tx_queued_step(void)
{
	if (!m_cur_valid) {
		if (k_msgq_get(&m_answer_q, &m_cur, K_NO_WAIT) == 0) {
			m_cur_kind = APP_RADIO_FRAME_ANSWER;
		} else if (k_msgq_get(&m_alarm_q, &m_cur, K_NO_WAIT) == 0) {
			m_cur_kind = APP_RADIO_FRAME_ALARM;
		} else {
			return false;
		}
		m_cur_valid = true;
		m_cur_retries = 0;
	}

	struct app_radio_frame f = {
		.kind = m_cur_kind,
		.tag = m_cur.tag,
		.port = m_cur.port,
		.flags = (m_be->confirm_kinds & BIT(m_cur_kind)) ? APP_RADIO_FRAME_CONFIRMED : 0,
		.len = m_cur.len,
		.buf = m_cur.buf,
	};
	struct app_radio_tx_result res = {0};
	int ret = m_be->send(&f, &res);

	switch (ret) {
	case 0:
		m_cur_valid = false;
		break;
	case -EMSGSIZE:
		if (!recover_over_budget(&m_cur, m_cur_kind, res.budget)) {
			m_cur_valid = false;
		}
		break;
	case -EBUSY:
	case -ENOTCONN:
		return true; /* waits for app_radio_tx_kick() */
	case -EAGAIN:
		tx_schedule(res.wait_ms ? res.wait_ms : TX_RETRY_MS);
		return true;
	default:
		if (++m_cur_retries > TX_MAX_RETRIES) {
			LOG_ERR("Frame (kind %u, %u B) abandoned after %d retries", m_cur_kind,
				m_cur.len, TX_MAX_RETRIES);
			m_cur_valid = false;
			break;
		}
		app_radio_count(APP_RADIO_CNT_RETRY);
		tx_schedule(TX_RETRY_MS);
		return true;
	}
	tx_schedule(0); /* the next frame */
	return true;
}

static void tlm_close(bool reset_snapshot)
{
	if (reset_snapshot) {
		app_compose_reset();
	}
	m_tlm_open = false;
	m_tlm_frame = false;
}

/* Send the report frame by frame, composed at send time against the budget
 * of that moment. */
static void tlm_step(void)
{
	if (!m_tlm_open) {
		/* MED-9: a history replay owns the radio; the request waits (the
		 * replay's end kicks a fresh report anyway). */
		if (!atomic_get(&m_tlm_requested) || m_be->replay_active()) {
			return;
		}
		atomic_clear(&m_tlm_requested);
		m_tlm_open = true;
		m_tlm_first = true;
		m_tlm_frame = false;
	}

	if (!m_tlm_frame) {
		int ret = app_compose_budget(m_tlm_buf, sizeof(m_tlm_buf), &m_tlm_len, &m_tlm_more,
					     m_be->budget());

		if (ret == -EAGAIN) {
			/* Budget 0: pending MAC answers (an ADR / channel batch from the
			 * LNS) fill the frame, so no telemetry fits. Stopping here
			 * deadlocks (H-1): no uplink -> the MAC answers never leave ->
			 * the budget stays 0 and the node goes mute. Send an empty frame
			 * so the MAC drains them; the budget is back for the next report. */
			struct app_radio_frame f = {.kind = APP_RADIO_FRAME_TELEMETRY,
						    .buf = m_tlm_buf};
			struct app_radio_tx_result res;

			(void)m_be->send(&f, &res);
			tlm_close(!m_tlm_first);
			return;
		}
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_compose_budget", ret);
			tlm_close(false);
			return;
		}
		if (m_tlm_len == 0) {
			tlm_close(false); /* nothing to report (e.g. no sample yet) */
			return;
		}
		if (m_tlm_first) {
			/* Once per report: the link-check decision advances its cadence,
			 * so a resend must not ask again (#188). */
			m_tlm_flags = m_be->report_flags();
		}
		m_tlm_frame = true;
		m_tlm_retries = 0;
	}

	uint8_t flags = m_tlm_flags;

	if (!m_tlm_first) {
		flags &= ~APP_RADIO_FRAME_LINK_CHECK;
	}
	if (m_tlm_more) {
		flags |= APP_RADIO_FRAME_MORE;
	}

	struct app_radio_frame f = {
		.kind = APP_RADIO_FRAME_TELEMETRY,
		.flags = flags,
		.len = (uint16_t)m_tlm_len,
		.buf = m_tlm_buf,
	};
	struct app_radio_tx_result res = {0};
	int ret = m_be->send(&f, &res);

	switch (ret) {
	case 0:
		break;
	case -EMSGSIZE:
		/* M-10: app_compose sends one group or 1-Wire reading that is bigger
		 * than the budget on its own (US915/AU915 DR0, 11 B, with a machine
		 * probe of ~25-30 B). Retrying it burns the retries every report on
		 * data that cannot fit: drop it and go on with the rest of the report;
		 * it fits again once the DR rises. */
		LOG_ERR("Telemetry frame %zu B over budget %u B; dropped (raise DR)", m_tlm_len,
			res.budget);
		break;
	case -EBUSY:
		return; /* kicked when the in-flight uplink is done */
	case -ENOTCONN:
		LOG_WRN("Report abandoned: no session; snapshot reset");
		tlm_close(true);
		return;
	default:
		if (++m_tlm_retries > TX_MAX_RETRIES) {
			LOG_ERR("Telemetry frame abandoned after %d retries; snapshot reset",
				TX_MAX_RETRIES);
			tlm_close(true);
			return;
		}
		app_radio_count(APP_RADIO_CNT_RETRY);
		tx_schedule((ret == -EAGAIN && res.wait_ms) ? res.wait_ms : TX_RETRY_MS);
		return;
	}

	m_tlm_frame = false;
	m_tlm_first = false;
	if (m_tlm_more) {
		tx_schedule(m_be->frame_gap_ms);
		return;
	}
	if (ret == 0) {
		/* Reports, not frames, drive the link-check cadence (#267). */
		m_be->report_done();
		LOG_INF("Snapshot complete");
	}
	tlm_close(false);
	if (atomic_get(&m_tlm_requested)) {
		tx_schedule(m_be->frame_gap_ms); /* a report requested meanwhile */
	}
}

static void tx_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Calibration mode transmits through its own path (app_calibration). */
	if (g_app_config.calibration || m_be == NULL) {
		return;
	}
	if (!m_be->tx_ready()) {
		if (m_tlm_open) {
			/* A (re)join starts from scratch: the report of the old
			 * session is not continued with stale data (#93.5). */
			LOG_WRN("Report abandoned: link down; snapshot reset");
			tlm_close(true);
		}
		return; /* everything waits for the link-up kick */
	}
	if (tx_queued_step()) {
		return;
	}
	tlm_step();
}

#if defined(CONFIG_ZTEST)
void app_radio_test_set_backend(const struct app_radio_backend *be)
{
	m_be = be;
}

void app_radio_test_tx_reset(void)
{
	struct k_work_sync sync;

	(void)k_work_cancel_delayable_sync(&m_tx_work, &sync);
	k_msgq_purge(&m_answer_q);
	k_msgq_purge(&m_alarm_q);
	m_cur_valid = false;
	atomic_clear(&m_tlm_requested);
	m_tlm_open = false;
	m_tlm_frame = false;
}
#endif

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

/* The report is composed and sent at once; the delay was taken here. */
static void jitter_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	atomic_clear(&m_telemetry_held);
	tx_request_telemetry();
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
	return app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, port, buf, len);
}

int app_radio_send_alarm(const uint8_t *buf, size_t len)
{
	return app_radio_tx_queue(APP_RADIO_FRAME_ALARM, APP_RADIO_TAG_OTHER, 0, buf, len);
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
	uint8_t buf[APP_RADIO_TX_SLOT_SIZE];
	size_t cap = app_radio_tx_answer_cap(sizeof(buf));
	size_t len;
	bool more = false;
	int ret = settings ? app_cmd_build_config_status(buf, cap, &len, &more)
			   : app_cmd_build_info_seq(seq, buf, cap, &len, &more);

	if (ret) {
		return ret;
	}
	ret = app_radio_tx_queue(APP_RADIO_FRAME_ANSWER,
				 settings ? APP_RADIO_TAG_SETTINGS : APP_RADIO_TAG_INFO, 0, buf,
				 len);
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

/* A network time younger than this answers a clock_sync at once (PF-2). The
 * same span as app_clock's forced-resync cooldown (#340 L11): on LoRaWAN a
 * second clock_sync inside it queued no DeviceTimeReq, so its answer waited
 * for the next time that landed -- up to the weekly re-sync. */
#define CLOCK_SYNC_FRESH_MS (60 * 1000)

static atomic_t m_clock_sync_now_seq;

static void clock_sync_now_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)app_radio_send_info((uint32_t)atomic_get(&m_clock_sync_now_seq));
}

static K_WORK_DEFINE(m_clock_sync_now_work, clock_sync_now_work_handler);

void app_radio_clock_sync(uint32_t seq)
{
	int64_t landed_ms = app_clock_network_time_at_ms();

	if (landed_ms != 0 && k_uptime_get() - landed_ms < CLOCK_SYNC_FRESH_MS) {
		/* The clock is fresh from the network: answer now, on the radio work
		 * queue like every other Info. */
		atomic_set(&m_clock_sync_now_seq, (atomic_val_t)seq);
		k_work_submit_to_queue(app_radio_work_q(), &m_clock_sync_now_work);
		return;
	}
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
