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
#include "app_history.h"
#include "app_log.h"
#include "app_radio_lrw.h"
#include "app_radio.h"

#if defined(CONFIG_RADIO_P2P)
#include "app_radio_p2p.h"
#endif
#if defined(CONFIG_WATCHDOG)
#include "app_wdog.h"
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

/* ---- Flash writes vs radio exchanges ---------------------------------------
 * The STM32WLE5 has one flash bank: a program or page erase stalls every
 * instruction fetch, interrupt handlers included -- ~22 ms per page erase, an
 * NVS garbage collection several of them. Landing between TX done and the RX1
 * opening, it makes the receiver miss the Ack or the JoinAccept (F4 HIL
 * 2026-09-27: an `alarm new` save lost three P2P Acks). So a backend marks
 * each exchange, TX start until its receive windows closed, and every flash
 * writer holds the flash around its write: a writer waits for a running
 * exchange, an exchange waits for a running write and for writers already
 * waiting, so a busy radio cannot starve them. Both waits are capped; past the
 * cap the write or the TX goes ahead (logged) -- nothing is dropped. */
#define FLASH_WAIT_AIR_MAX_MS 10000 /* LoRaWAN DR0 join: JoinAccept in RX2 at 6 s */
#define AIR_WAIT_FLASH_MAX_MS 1000  /* a history clear, 16 page erases */

static K_MUTEX_DEFINE(m_air_mutex);
static K_CONDVAR_DEFINE(m_air_cv);
static bool m_on_air;
static uint8_t m_flash_writers;
static uint8_t m_flash_waiting;

/* The exchange itself runs on the radio work queue, LoRaMacProcess() and the
 * SX126x DIO1 work on the system one: a writer there must never block on the
 * exchange it would hold up. It still counts, so no TX starts mid-write. */
static bool flash_writer_may_wait(void)
{
	if (k_is_in_isr()) {
		return false;
	}

	k_tid_t self = k_current_get();

	return self != k_work_queue_thread_get(&m_wq) &&
	       self != k_work_queue_thread_get(&k_sys_work_q);
}

static bool air_idle(void)
{
	return !m_on_air;
}

static bool flash_idle(void)
{
	return m_flash_writers == 0 && m_flash_waiting == 0;
}

/* m_air_mutex held: wait on m_air_cv until `done`; false once `max_ms` ran out. */
static bool air_wait(bool (*done)(void), int32_t max_ms)
{
	int64_t deadline = k_uptime_get() + max_ms;

	while (!done()) {
		int64_t left = deadline - k_uptime_get();

		if (left <= 0) {
			return false;
		}
		(void)k_condvar_wait(&m_air_cv, &m_air_mutex, K_MSEC(left));
	}
	return true;
}

void app_radio_flash_hold(void)
{
	k_mutex_lock(&m_air_mutex, K_FOREVER);
	if (m_on_air && flash_writer_may_wait()) {
		m_flash_waiting++;
		if (!air_wait(air_idle, FLASH_WAIT_AIR_MAX_MS)) {
			LOG_WRN("Flash write waited %d ms for the radio; writing anyway",
				FLASH_WAIT_AIR_MAX_MS);
		}
		m_flash_waiting--;
	}
	m_flash_writers++;
	k_mutex_unlock(&m_air_mutex);
}

void app_radio_flash_release(void)
{
	k_mutex_lock(&m_air_mutex, K_FOREVER);
	if (m_flash_writers > 0) {
		m_flash_writers--;
	}
	k_condvar_broadcast(&m_air_cv);
	k_mutex_unlock(&m_air_mutex);
}

void app_radio_air_begin(void)
{
	k_mutex_lock(&m_air_mutex, K_FOREVER);
	if (!air_wait(flash_idle, AIR_WAIT_FLASH_MAX_MS)) {
		LOG_WRN("Radio waited %d ms for a flash write; sending anyway",
			AIR_WAIT_FLASH_MAX_MS);
	}
	m_on_air = true;
	k_mutex_unlock(&m_air_mutex);
}

void app_radio_air_end(void)
{
	k_mutex_lock(&m_air_mutex, K_FOREVER);
	m_on_air = false;
	k_condvar_broadcast(&m_air_cv);
	k_mutex_unlock(&m_air_mutex);
}

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
/* The downlink path and the announce frames run on the radio work queue
 * (doc/plan/460 §2.5, F3). */
static void announce_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_announce_work, announce_work_handler);
static void page_stream_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_page_stream_work, page_stream_work_handler);
static void post_cmd_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_post_cmd_work, post_cmd_work_handler);
/* The history replay (F3c): one frame per run. */
static void hist_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_hist_work, hist_work_handler);
/* A replay is streaming: telemetry waits (MED-9). Radio work queue only. */
static bool m_hist_active;
/* app_report's link-ready kick, also fired when a replay ends. */
static void (*m_ready_cb)(void);

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

/* Link supervision (doc/plan/460 §2.4, F2): radio work queue only, except the
 * forced-check flag (any thread) and `warning`, read by app_radio_get_state()
 * from any thread (a bool). */
static struct app_radio_link m_link;
static atomic_t m_link_forced;

enum app_radio_state app_radio_get_state(void)
{
	enum app_radio_state state = m_be ? m_be->get_state() : APP_RADIO_STATE_IDLE;

	return (state == APP_RADIO_STATE_HEALTHY && m_link.warning) ? APP_RADIO_STATE_WARNING
								    : state;
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

static void set_fail_streak(uint32_t n)
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

/* ---- Duty-cycle ledger (doc/plan/460 §2.7, T2d) ------------------------------
 *
 * EU868 duty cycle, enforced app-side with an exact sliding-hour ledger (B2,
 * decision D1; common to both radios since T2d). Raw LoRa (P2P) has no duty
 * enforcement of its own. LoRaMac has one, but its credits come back only when
 * a fixed hour since the band's last refill has run out, so the MAC used to
 * refuse frames that app_radio then retried blindly every 15 s.
 *
 * Two models preceded it on P2P. The first blocked the radio for air*99 ms
 * after EVERY frame, so an alarm queued behind a long telemetry frame waited
 * minutes. The second (PR #408) was a token bucket refilling at 1% of wall
 * time and capped at the full hourly allowance: that fixed the latency and
 * held the long-run average at 1%, but a node idle for an hour could then
 * burst the whole 36 s of air at once, which means a worst-case SLIDING hour
 * of ~2%. Amortised compliance, not compliance.
 *
 * The ledger records (end time, air-time) per transmission and admits a frame
 * only if the air already inside the trailing hour plus this frame fits the
 * allowance -- so every sliding hour sums to <= the limit, with no burst hole
 * to argue about in a certification review. A frame goes the moment there is
 * room, rather than serving a fixed post-frame penalty.
 *
 * LoRaWAN: the MAC's fixed hour started at most an hour ago, so the air it
 * counts in it is never more than the trailing hour of the ledger. With the
 * same air per frame (app_radio_lora_toa_ms() is the MAC's formula) and the
 * JoinRequests charged too, a frame the ledger admits the MAC admits as well:
 * the ledger is the conservative one of the two, and its wait is exact. It
 * sums all bands into one 1 % allowance; the MAC counts 1 % per band. Two
 * edges remain, both caught by the MAC's own check (-ECONNREFUSED, duty hold):
 * an ADR step inside a send is charged at the DR before it, and the MAC wants
 * its credits strictly above the cost where the ledger admits an exact fit.
 *
 * An empty ledger at boot: never blocked, as the full token bucket was not. A
 * reboot therefore forgets the hour just transmitted. That hole is accepted:
 * the ledger is RAM-only, and persisting it would cost an NVS write per frame.
 * doc/p2p.md §6 records it. Cost: 392 B of RAM.
 */
#if defined(CONFIG_ZTEST)
#define DUTY_TESTABLE
#else
#define DUTY_TESTABLE static
#endif

static struct app_radio_duty m_duty;
static struct k_spinlock m_duty_lock; /* the radio work queue writes, readers take status */
static bool m_duty_on;                /* a backend set the ledger up */

/* Index of the i-th oldest entry. */
static inline uint8_t duty_slot(const struct app_radio_duty *d, uint8_t i)
{
	return (uint8_t)((d->head + i) % APP_RADIO_DUTY_LEDGER_ENTRIES);
}

/* Drop every entry that has fallen out of the trailing window.
 *
 * `now` and `end_ms` are uptime truncated to 32 bits and compared as an
 * unsigned difference, which stays correct across the ~49.7-day wrap: an
 * entry only ever lives APP_RADIO_DUTY_WINDOW_MS, four orders of magnitude
 * short of the wrap distance, so `now - end_ms` can never alias. */
static void duty_expire(struct app_radio_duty *d, uint32_t now)
{
	while (d->count > 0 && (now - d->entries[d->head].end_ms) >= APP_RADIO_DUTY_WINDOW_MS) {
		d->head = duty_slot(d, 1);
		d->count--;
	}
}

/* Make room for one more entry by folding the two oldest into one: the
 * younger keeps its end time and takes the older's air, so the pair leaves the
 * window when the younger would have. Only ever over-counts air (the older
 * half is held a little longer), so the bound holds (F-P2P-1). */
static void duty_fold_oldest(struct app_radio_duty *d)
{
	struct app_radio_duty_entry *oldest = &d->entries[d->head];
	struct app_radio_duty_entry *next = &d->entries[duty_slot(d, 1)];

	next->air_ms += oldest->air_ms;
	d->head = duty_slot(d, 1);
	d->count--;
}

/* Air recorded inside the window that ends at `now_ms`. Reads only, so a
 * status reader needs no expiry first. */
DUTY_TESTABLE uint32_t app_radio_ledger_used_ms(const struct app_radio_duty *d, int64_t now_ms)
{
	uint32_t now = (uint32_t)now_ms;
	uint32_t used = 0;

	for (uint8_t i = 0; i < d->count; i++) {
		const struct app_radio_duty_entry *e = &d->entries[duty_slot(d, i)];

		if (now - e->end_ms < APP_RADIO_DUTY_WINDOW_MS) {
			used += e->air_ms;
		}
	}
	return used;
}

DUTY_TESTABLE void app_radio_ledger_init(struct app_radio_duty *d, uint32_t budget_ms)
{
	d->budget_ms = budget_ms;
	d->head = 0;
	d->count = 0;
}

/* Record `air_ms` of air that finished at `now_ms`. */
DUTY_TESTABLE void app_radio_ledger_charge(struct app_radio_duty *d, int64_t now_ms,
					   uint32_t air_ms)
{
	uint32_t now = (uint32_t)now_ms;

	duty_expire(d, now);
	if (d->count >= APP_RADIO_DUTY_LEDGER_ENTRIES) {
		/* app_radio_ledger_wait_ms() already folds before admitting, so
		 * the real call paths never get here with a full ring; fold anyway
		 * rather than drop a charge, the one outcome that could breach the
		 * limit. */
		duty_fold_oldest(d);
	}
	d->entries[duty_slot(d, d->count)] = (struct app_radio_duty_entry){
		.end_ms = now,
		.air_ms = air_ms,
	};
	d->count++;
}

/* How many ms to wait before `air_ms` of air may be transmitted -- 0 if now.
 *
 * The guarantee is exact rather than amortised: a frame is admitted only when
 * the air already recorded in the trailing hour plus this frame fits inside
 * the budget, so EVERY sliding one-hour window stays within it. When blocked,
 * the answer is the time until enough of the oldest entries have left the
 * window for the frame to fit, so one wait is always enough. */
DUTY_TESTABLE int64_t app_radio_ledger_wait_ms(struct app_radio_duty *d, int64_t now_ms,
					       uint32_t air_ms)
{
	uint32_t now = (uint32_t)now_ms;

	duty_expire(d, now);

	/* F-P2P-1: a full ring folds its two oldest entries rather than making
	 * the frame wait for a slot, so only the air-time budget can refuse it. */
	if (d->count >= APP_RADIO_DUTY_LEDGER_ENTRIES) {
		duty_fold_oldest(d);
	}

	uint32_t used = app_radio_ledger_used_ms(d, now_ms);

	if (d->budget_ms == 0 || used + air_ms <= d->budget_ms || d->count == 0) {
		/* No limit, room now -- or an empty ledger: only a frame whose own
		 * air exceeds the whole allowance gets here, and refusing it for
		 * ever would be worse than sending it. */
		return 0;
	}

	uint32_t over = used + air_ms - d->budget_ms;
	uint32_t freed = 0;
	uint8_t i = 0;

	/* The oldest entries until they free `over`, or all of them (the frame
	 * alone is over the allowance: it goes once the ledger is empty).
	 * duty_expire() keeps every entry inside the window, so the wait is in
	 * (0, APP_RADIO_DUTY_WINDOW_MS]. */
	for (; i < d->count - 1; i++) {
		freed += d->entries[duty_slot(d, i)].air_ms;
		if (freed >= over) {
			break;
		}
	}
	return (int64_t)(APP_RADIO_DUTY_WINDOW_MS - (now - d->entries[duty_slot(d, i)].end_ms));
}

void app_radio_duty_init(uint32_t budget_ms)
{
	k_spinlock_key_t key = k_spin_lock(&m_duty_lock);

	app_radio_ledger_init(&m_duty, budget_ms);
	m_duty_on = true;
	k_spin_unlock(&m_duty_lock, key);
}

int64_t app_radio_duty_wait_ms(uint32_t air_ms)
{
	k_spinlock_key_t key = k_spin_lock(&m_duty_lock);
	int64_t wait = app_radio_ledger_wait_ms(&m_duty, k_uptime_get(), air_ms);

	k_spin_unlock(&m_duty_lock, key);
	if (wait > 0) {
		app_radio_set_duty_held(true);
	}
	return wait;
}

void app_radio_duty_charge(uint32_t air_ms)
{
	k_spinlock_key_t key = k_spin_lock(&m_duty_lock);

	app_radio_ledger_charge(&m_duty, k_uptime_get(), air_ms);
	k_spin_unlock(&m_duty_lock, key);
	app_radio_set_duty_held(false);
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

	key = k_spin_lock(&m_duty_lock);
	st->has_airtime = m_duty_on;
	st->airtime_hour_ms = m_duty_on ? app_radio_ledger_used_ms(&m_duty, k_uptime_get()) : 0;
	k_spin_unlock(&m_duty_lock, key);

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
	if (m_hist_active) {
		k_work_schedule_for_queue(app_radio_work_q(), &m_hist_work, K_NO_WAIT);
	}
}

static void tx_request_telemetry(void)
{
	atomic_set(&m_tlm_requested, 1);
	app_radio_tx_kick();
}

/* ---- Confirmed uplinks (doc/plan/460 §2.6, T2c) ---------------------------
 * One confirmed frame in flight on either radio (P2P's F-P1-1 made common). A
 * frame the backend sent without an Ack (-ETIMEDOUT) stays with its path -- the
 * queued frame, the report's frame or the replay's -- and goes again after
 * app_radio_ack_backoff_ms(), APP_RADIO_ACK_MAX_RETRIES times at most; every
 * other send waits meanwhile. P2P sends it under the same counter (the central
 * keeps a strict counter high-water), LoRaWAN under a new FCnt. Given up, it
 * is a failed link check, and the frame counts as sent: it went out. A new
 * session starts it afresh. Radio work queue only. */
#define TX_ACK_RETRY      1 /* tx_send(): no Ack; sent again after res->wait_ms */
/* On top of a duty-cycle wait: a frame woken a few ms early would be held
 * again at once (#118). */
#define TX_DUTY_MARGIN_MS 50

static bool m_ack_pending;    /* a confirmed frame waits for its retry */
static uint8_t m_ack_kind;    /* ... of this kind: the path holding it */
static uint8_t m_ack_retries; /* ... retries sent so far */

static bool kind_confirmed(uint8_t kind)
{
	if (kind == APP_RADIO_FRAME_ALARM) {
		return g_app_config.radio_alarm_ack; /* both radios alike */
	}
	return (m_be->confirm_kinds & BIT(kind)) != 0;
}

/* The path is done with its frame (sent, dropped, the replay ended): the
 * frames that waited go now. Only the other path waits for the kick (-EBUSY);
 * the releasing one schedules its own next step, and a kick of it would run
 * it at once, cutting its frame gap short (a delayable work submitted with
 * K_NO_WAIT stays queued through the reschedule that follows). */
static void ack_release(uint8_t kind)
{
	if (!m_ack_pending || m_ack_kind != kind) {
		return;
	}
	m_ack_pending = false;
	if (kind == APP_RADIO_FRAME_HISTORY) {
		k_work_schedule_for_queue(app_radio_work_q(), &m_tx_work, K_NO_WAIT);
	} else if (m_hist_active) {
		k_work_schedule_for_queue(app_radio_work_q(), &m_hist_work, K_NO_WAIT);
	}
}

bool app_radio_ack_pending(void)
{
	return m_ack_pending;
}

static void stale_note_hold(int64_t hold_ms);

/* m_be->send() under the confirmed ladder. Returns the backend's result, or
 * TX_ACK_RETRY: the caller keeps the frame, byte for byte, and sends it again
 * after res->wait_ms. */
static int tx_send(struct app_radio_frame *f, struct app_radio_tx_result *res)
{
	bool retry = m_ack_pending && m_ack_kind == f->kind;

	if (m_ack_pending && !retry) {
		return -EBUSY; /* kicked by ack_release() */
	}
	if (retry) {
		f->flags |= APP_RADIO_FRAME_CONFIRMED; /* a retry stays confirmed */
	} else {
		m_ack_retries = 0;
	}
	f->attempt = m_ack_retries;

	/* doc/plan/460 §2.7 (T2d): the frame waits for the duty ledger, exactly
	 * as long as it takes to fit, on either radio. */
	int64_t hold = app_radio_duty_wait_ms(m_be->airtime_ms(f->len));
	int ret;

	if (hold > 0) {
		LOG_WRN("TX duty-cycle blocked for %lld ms", hold);
		stale_note_hold(hold);
		res->wait_ms = (uint32_t)hold + TX_DUTY_MARGIN_MS;
		ret = -EAGAIN;
	} else {
		ret = m_be->send(f, res);
	}

	if (retry && (ret == 0 || ret == -ETIMEDOUT)) {
		app_radio_count(APP_RADIO_CNT_RETRY);
	}
	switch (ret) {
	case 0:
		if (retry) {
			LOG_INF("Confirmed frame (kind %u) acknowledged on retry %u", f->kind,
				m_ack_retries);
		}
		ack_release(f->kind);
		return 0;
	case -ETIMEDOUT:
		if (!(f->flags & APP_RADIO_FRAME_CONFIRMED)) {
			return -EIO; /* no Ack was asked for: a radio / MAC error */
		}
		if (m_ack_retries >= APP_RADIO_ACK_MAX_RETRIES) {
			LOG_WRN("Confirmed frame (kind %u) unacknowledged after %d retries; given "
				"up",
				f->kind, APP_RADIO_ACK_MAX_RETRIES);
			ack_release(f->kind);
			app_radio_link_result(false);
			return 0;
		}
		m_ack_pending = true;
		m_ack_kind = f->kind;
		m_ack_retries++;
		res->wait_ms = app_radio_ack_backoff_ms(m_ack_retries, sys_rand32_get());
		LOG_WRN("Confirmed frame (kind %u) unacknowledged: retry %u/%d in %u ms", f->kind,
			m_ack_retries, APP_RADIO_ACK_MAX_RETRIES, res->wait_ms);
		return TX_ACK_RETRY;
	case -EAGAIN:
		if (retry) {
			/* The duty cycle holds the retry: the spread comes on top of
			 * it, or two held nodes would retry together again. */
			res->wait_ms = (res->wait_ms ? res->wait_ms : TX_RETRY_MS) +
				       app_radio_ack_backoff_ms(m_ack_retries, sys_rand32_get());
		}
		return ret;
	case -EMSGSIZE:
		ack_release(f->kind); /* recovered by its kind: a new frame */
		return ret;
	default:
		return ret;
	}
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

bool app_radio_tx_alarm_pending(void)
{
	return k_msgq_num_used_get(&m_alarm_q) > 0 ||
	       (m_cur_valid && m_cur_kind == APP_RADIO_FRAME_ALARM);
}

uint32_t app_radio_tx_alarm_free(void)
{
	return k_msgq_num_free_get(&m_alarm_q);
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
	/* A report frame waits for its Ack retry: it goes before any queued one. */
	if (m_ack_pending && m_ack_kind == APP_RADIO_FRAME_TELEMETRY) {
		return false;
	}
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
		if (m_cur_kind == APP_RADIO_FRAME_ALARM) {
			/* A slot is free: a batch held for room may go now (#462). */
			app_alarm_flush_held();
		}
	}

	struct app_radio_frame f = {
		.kind = m_cur_kind,
		.tag = m_cur.tag,
		.port = m_cur.port,
		.flags = kind_confirmed(m_cur_kind) ? APP_RADIO_FRAME_CONFIRMED : 0,
		.len = m_cur.len,
		.buf = m_cur.buf,
	};
	struct app_radio_tx_result res = {0};
	int ret = tx_send(&f, &res);

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
	case TX_ACK_RETRY:
		tx_schedule(res.wait_ms);
		return true;
	default:
		if (++m_cur_retries > TX_MAX_RETRIES) {
			LOG_ERR("Frame (kind %u, %u B) abandoned after %d retries", m_cur_kind,
				m_cur.len, TX_MAX_RETRIES);
			m_cur_valid = false;
			ack_release(m_cur_kind);
			break;
		}
		app_radio_count(APP_RADIO_CNT_RETRY);
		tx_schedule(TX_RETRY_MS);
		return true;
	}
	tx_schedule(0); /* the next frame */
	return true;
}

/* ---- Link supervision (doc/plan/460 §2.4, F2) -----------------------------
 * The machine both radios ran on their own (LoRaWAN #71, P2P decision #22
 * §3.4), in one place: the cadence of link checks, WARNING with its recovery
 * rungs, and the rejoin once they are exhausted. The backend only says how a
 * link check rides (report_flags), reports its outcome (app_radio_link_result)
 * and carries out a rung or a rejoin. */

/* M-2 stale-uplink watchdog: uptime of the last telemetry uplink (0 = none
 * since the last link-up) and the duty-cycle hold streak. Radio work queue. */
static int64_t m_last_uplink_ms;
static struct app_radio_stale_dc m_dc;
static bool m_dc_hold_logged;

static void link_set_streak(uint32_t n)
{
	m_link.fail_streak = n;
	set_fail_streak(n);
}

static void link_clear(void)
{
	link_set_streak(0);
	m_link.warning = false;
	m_link.warning_fails = 0;
}

static void hist_drop(void);

void app_radio_link_up(void)
{
	/* A replay does not outlive its session: its frame waiting for the retry
	 * would go out under the new one. The host asks again. */
	hist_drop();
	/* A frame waiting for its Ack retry goes afresh under the new session
	 * (P2P: a new counter, never the old one under the new key). */
	m_ack_pending = false;
	link_clear();
	m_link.reports = 0; /* the first report of the session is a link check */
	atomic_clear(&m_link_forced);
	m_last_uplink_ms = MAX(k_uptime_get(), 1);
	m_dc = (struct app_radio_stale_dc){0};
	m_dc_hold_logged = false;
}

void app_radio_link_result(bool ok)
{
	if (!ok) {
		app_radio_count(APP_RADIO_CNT_FAIL);
	}
	if (m_be == NULL || m_be->get_state() != APP_RADIO_STATE_HEALTHY) {
		return; /* (re)joining: the next link-up starts afresh */
	}
	if (ok) {
		if (m_link.warning) {
			LOG_INF("Link check OK in WARNING: back to HEALTHY");
		}
		link_clear();
		return;
	}

	link_set_streak(m_link.fail_streak + 1);
	if (!m_link.warning) {
		LOG_WRN("Link check failed (streak %u/%d)", m_link.fail_streak,
			APP_RADIO_LINK_WARNING_THRESHOLD);
		if (m_link.fail_streak >= APP_RADIO_LINK_WARNING_THRESHOLD) {
			m_link.warning = true;
			m_link.warning_fails = 0;
			LOG_WRN("Link WARNING: session kept, every report checks the link");
			/* The failures that got here already show the link no longer
			 * reaches: take the first rung now. */
			(void)m_be->warning_step();
		}
		return;
	}

	/* The next rung before the rejoin budget: a rejoin is never spent while a
	 * rung is still untried (it costs the session and, on LoRaWAN, a DevNonce). */
	bool stepped = m_be->warning_step();
	int budget = MAX(g_app_config.radio_link_check_fail_rejoin, 1);

	m_link.warning_fails++;
	LOG_WRN("Link check failed in WARNING (%u/%d%s)", m_link.warning_fails, budget,
		stepped ? ", recovery step" : "");
	if (stepped || m_link.warning_fails < (uint32_t)budget) {
		return;
	}
	if (m_be->rejoin(false) != 0) {
		/* Cannot rejoin (LoRaWAN ABP, P2P unprovisioned): stay in WARNING
		 * and keep checking every report; recover when the link returns. */
		LOG_WRN("Cannot rejoin: staying in WARNING");
		m_link.warning_fails = 0;
		return;
	}
	LOG_WRN("Link lost after %u failed checks in WARNING: rejoin", m_link.warning_fails);
}

void app_radio_force_link_check(void)
{
	atomic_set(&m_link_forced, 1);
}

void app_radio_get_link(struct app_radio_link *link)
{
	*link = m_link;
}

void app_radio_note_send(bool sent, bool duty_held)
{
	app_radio_stale_note(&m_dc, sent, duty_held, k_uptime_get());
	if (sent) {
		m_dc_hold_logged = false;
	}
}

/* The duty ledger holds the frame for `hold_ms`: the M-2 excuse lasts until then. */
static void stale_note_hold(int64_t hold_ms)
{
	app_radio_stale_note_hold(&m_dc, k_uptime_get(), hold_ms);
}

void app_radio_note_uplink(void)
{
	m_last_uplink_ms = MAX(k_uptime_get(), 1);
}

/* A report starts: is it a link check? The first report after a link-up and
 * every N-th after it, every report while WARNING, and the next one after
 * app_radio_force_link_check(). A forced check is used up once one rode. */
static uint8_t report_flags(void)
{
	bool forced = atomic_get(&m_link_forced) != 0;
	bool due = forced ||
		   app_radio_link_check_due(m_link.reports, g_app_config.radio_link_check_interval,
					    m_link.warning);
	uint8_t flags = m_be->report_flags(due);

	if (forced && (flags & (APP_RADIO_FRAME_LINK_CHECK | APP_RADIO_FRAME_CONFIRMED))) {
		atomic_clear(&m_link_forced);
	}
	return flags;
}

/* M-2 (F29): a joined node whose telemetry has not left for
 * APP_RADIO_STALE_FACTOR x interval_report is mute although the radio work
 * queue drains (sends perpetually skipped, retries exhausted): rejoin --
 * unless the duty cycle explains it (app_radio_stale_check()). */
#if defined(CONFIG_WATCHDOG) || defined(CONFIG_ZTEST)
static void stale_tick(int64_t now)
{
	if (m_be == NULL || m_be->get_state() != APP_RADIO_STATE_HEALTHY) {
		return;
	}

	switch (app_radio_stale_check(now, m_last_uplink_ms, &m_dc,
				      (uint32_t)g_app_config.interval_report)) {
	case APP_RADIO_STALE_HOLD_DC:
		if (!m_dc_hold_logged) {
			LOG_WRN("No telemetry uplink for >%d report intervals, but the duty cycle "
				"holds sends (%d s): no rejoin (M-2)",
				APP_RADIO_STALE_FACTOR, (int)((now - m_dc.since_ms) / 1000));
			m_dc_hold_logged = true;
		}
		break;
	case APP_RADIO_STALE_REJOIN:
		LOG_WRN("No telemetry uplink for >%d report intervals: rejoin (M-2)",
			APP_RADIO_STALE_FACTOR);
		m_last_uplink_ms = now; /* don't re-trigger every tick */
		(void)m_be->rejoin(true);
		break;
	default:
		break;
	}
}
#endif

#if defined(CONFIG_WATCHDOG)
/* Liveness heartbeat (#182): a self-rearming work item proves the radio work
 * queue still drains. If it wedges, the channel goes stale and app_wdog stops
 * feeding the IWDG -> SoC reset and a fresh join. The timeout is far above
 * the longest legitimate single send (~7 s on TTN with a 5 s RX1 delay; a
 * P2P confirmed uplink and its RX1), so only a true wedge trips it. */
#define HEARTBEAT_PERIOD_SEC 5
#define HEARTBEAT_TIMEOUT_MS 30000

/* A lost MAC confirm must end in -ETIMEDOUT from lorawan_send()/lorawan_join()
 * (#181) before the liveness channel goes stale and resets the SoC. */
#if defined(CONFIG_LORAWAN_CONFIRM_TIMEOUT_MS)
BUILD_ASSERT(CONFIG_LORAWAN_CONFIRM_TIMEOUT_MS > 0 &&
		     CONFIG_LORAWAN_CONFIRM_TIMEOUT_MS < HEARTBEAT_TIMEOUT_MS,
	     "LoRaWAN confirm timeout must be bounded and below the work-queue heartbeat");
#endif

static int m_wdog_channel = -1;
static bool m_heartbeat_started;

static void heartbeat_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(m_heartbeat_work, heartbeat_work_handler);

static void heartbeat_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	app_wdog_ping(m_wdog_channel);
	stale_tick(k_uptime_get());
	k_work_schedule_for_queue(app_radio_work_q(), &m_heartbeat_work,
				  K_SECONDS(HEARTBEAT_PERIOD_SEC));
}
#endif /* defined(CONFIG_WATCHDOG) */

void app_radio_heartbeat_start(void)
{
#if defined(CONFIG_WATCHDOG)
	if (m_heartbeat_started) {
		return;
	}
	m_heartbeat_started = true;
	/* Registered even when radio-silent: the queue still runs, and a wedge
	 * there should still recover. */
	m_wdog_channel = app_wdog_register(HEARTBEAT_TIMEOUT_MS);
	if (m_wdog_channel < 0) {
		LOG_ERR_CALL_FAILED_INT("app_wdog_register", m_wdog_channel);
	}
	k_work_schedule_for_queue(app_radio_work_q(), &m_heartbeat_work, K_NO_WAIT);
#endif
}

void app_radio_heartbeat_feed(void)
{
#if defined(CONFIG_WATCHDOG)
	app_wdog_ping(m_wdog_channel);
#endif
}

static void tlm_close(bool reset_snapshot)
{
	if (reset_snapshot) {
		app_compose_reset();
	}
	m_tlm_open = false;
	m_tlm_frame = false;
	ack_release(APP_RADIO_FRAME_TELEMETRY);
}

/* Send the report frame by frame, composed at send time against the budget
 * of that moment. */
static void tlm_step(void)
{
	if (!m_tlm_open) {
		/* MED-9: a history replay owns the radio; the request waits (the
		 * replay's end kicks a fresh report anyway). */
		if (!atomic_get(&m_tlm_requested) || m_hist_active) {
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

			if (tx_send(&f, &res) == -EBUSY) {
				return; /* kicked when the confirmed frame is done */
			}
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
			/* Once per report: a resend must not ask again (#188). */
			m_tlm_flags = report_flags();
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
	int ret = tx_send(&f, &res);

	switch (ret) {
	case 0:
		m_last_uplink_ms = k_uptime_get(); /* M-2: telemetry went out */
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
	case TX_ACK_RETRY:
		tx_schedule(res.wait_ms);
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
		m_link.reports++;
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

/* ---- History replay (doc/plan/460 §2.5, F3c) ------------------------------
 * ReqHistory (#52) streams the matching records back as HistoryFrame answers,
 * one frame per run of the radio work queue, HIST_FRAME_GAP_MS apart -- the
 * machine both radios ran on their own. Each frame is sized for the budget of
 * its moment, so a LoRaWAN DR change packs more or fewer records into it; the
 * replay ends when its cursor reaches the end, not after the up-front frame
 * count (#89), which is only an estimate (the host concatenates by
 * frame_index). Telemetry waits for the end (MED-9): frames in between would
 * spend the duty cycle and, on P2P, the confirmed-uplink slot the replay needs,
 * and break the run of frames the host reassembles. Alarms and answers do not
 * wait: a replay can run for minutes. */
#define HIST_FRAME_GAP_MS 3000

static uint32_t m_hist_from, m_hist_to, m_hist_seq;
static uint32_t m_hist_count; /* frame_count: the estimate at the start */
static uint32_t m_hist_idx;   /* frame_index of the next frame */
/* #409 3f: upper bound for frame_index / frame_count when sizing a frame (their
 * varint width). UINT32_MAX = worst case; tightened to the frame count at the
 * start, which buys ~8 B of samples per frame at low DRs. */
static uint32_t m_hist_frame_bound = UINT32_MAX;
/* Absolute record ordinals (app_history_span()): the next record to send and
 * the end of the replay (exclusive), snapshot at the start. Captures go on
 * during a replay and the RAM ring may evict under it, but an absolute cursor
 * names the same record throughout, so nothing is repeated or skipped; records
 * captured after the start are left for the next replay. */
static uint32_t m_hist_cursor;
static uint32_t m_hist_end;
static uint32_t m_hist_present; /* sensor mask, snapshot at the start */
static uint32_t m_hist_interval;
static uint8_t m_hist_retries; /* failed sends of the current frame (#89) */
/* The frame built last: its length, record count and the cursor after it. A
 * confirmed retry sends it again as built -- rebuilt, it could differ (an
 * eviction, a DR change), and P2P resends it under the same counter. */
static uint16_t m_hist_len;
static uint16_t m_hist_n;
static uint32_t m_hist_next;
/* The encoded frame (version byte + Response) and its raw samples; static,
 * 512 B off the radio work queue stack. */
static uint8_t m_hist_buf[APP_CMD_HISTORY_FRAME_BUF_SIZE];
static uint8_t m_hist_samples[APP_CMD_HISTORY_FRAME_BUF_SIZE];

/* Samples one frame holds within `budget`: the exact envelope overhead of this
 * replay's fields (#89), frame_index / frame_count sized by m_hist_frame_bound
 * and t0 by the widest varint -- a lower bound for every frame, so counting
 * and sending use the same cap. */
static size_t hist_frame_cap(uint8_t budget)
{
	return app_cmd_history_sample_capacity(m_hist_seq, m_hist_frame_bound, m_hist_frame_bound,
					       UINT32_MAX, m_hist_present, m_hist_interval,
					       MIN((size_t)budget, sizeof(m_hist_buf)));
}

static void hist_end(void)
{
	m_hist_active = false;
	app_history_set_replay_active(false);
	ack_release(APP_RADIO_FRAME_HISTORY);
}

/* The replay is over (done or given up): the telemetry it held may go, and
 * app_report takes its cadence back with an immediate report. */
static void hist_finish(void)
{
	hist_end();
	app_radio_tx_kick();
	if (m_ready_cb) {
		m_ready_cb();
	}
}

static void hist_drop(void)
{
	if (m_hist_active) {
		LOG_WRN("History replay dropped: new session");
		(void)k_work_cancel_delayable(&m_hist_work);
		hist_end();
	}
}

/* #409 3f: records remain, but not one fits the budget now (a LoRaWAN DR
 * drop). Tell the host instead of going silent mid-stream; it asks again at a
 * higher DR. */
static void hist_budget_error(void)
{
	uint8_t err[16];
	size_t len;

	if (app_cmd_build_budget_error(m_hist_seq, err, app_radio_tx_answer_cap(sizeof(err)),
				       &len) == 0) {
		(void)app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER, 0, err, len);
	}
}

/* Build the next frame into m_hist_buf for `budget`. Returns its record
 * count, 0 once the replay is over (hist_finish() done). */
static uint16_t hist_build(uint8_t budget, size_t *len, uint32_t *next)
{
	/* A DR drop packs fewer records per frame, so frame_index can outgrow the
	 * bound the cap was sized with: back to the worst case. */
	if (m_hist_idx >= m_hist_frame_bound) {
		m_hist_frame_bound = UINT32_MAX;
	}

	size_t cap = MIN(hist_frame_cap(budget), sizeof(m_hist_samples));
	uint32_t t0 = 0;
	bool synced = false;
	uint16_t n = 0;
	size_t slen = 0;

	*next = m_hist_cursor;
	if (cap > 0) {
		slen = app_history_export_abs(m_hist_from, m_hist_to, m_hist_cursor, m_hist_end,
					      m_hist_samples, cap, &t0, &synced, &n, next);
	}
	if (n == 0) {
		if (*next >= m_hist_end) {
			/* Nothing left in the window: the records were evicted or
			 * the ring was reset since the previous frame. */
			LOG_INF("History replay complete: %u frames", m_hist_idx);
		} else {
			LOG_WRN("History replay stop at frame %u/%u (cap %zu B)", m_hist_idx,
				m_hist_count, cap);
			hist_budget_error();
		}
		hist_finish();
		return 0;
	}

	/* time_synced is per frame: a frame never spans two history segments,
	 * and each segment (flash page) knows whether its base is unix or uptime. */
	int ret = app_cmd_build_history_frame(
		m_hist_seq, m_hist_idx, m_hist_count, t0, m_hist_present, m_hist_interval, synced,
		m_hist_samples, slen, m_hist_buf, sizeof(m_hist_buf), len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_cmd_build_history_frame", ret);
		hist_finish();
		return 0;
	}
	return n;
}

static void hist_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!m_hist_active || m_be == NULL) {
		return;
	}
	if (!m_be->tx_ready()) {
		LOG_WRN("History replay aborted: link down");
		hist_finish();
		return;
	}

	struct app_radio_frame f = {
		.kind = APP_RADIO_FRAME_HISTORY,
		.flags = kind_confirmed(APP_RADIO_FRAME_HISTORY) ? APP_RADIO_FRAME_CONFIRMED : 0,
		.buf = m_hist_buf,
	};

	if (!(m_ack_pending && m_ack_kind == APP_RADIO_FRAME_HISTORY)) {
		uint8_t budget = m_be->budget();
		size_t len = 0;

		m_hist_len = 0;
		m_hist_n = 0;
		m_hist_next = m_hist_cursor;
		/* Budget 0: pending LoRaWAN MAC answers fill the frame (H-1). The
		 * empty frame lets the MAC drain them; the send answers -EAGAIN and
		 * the frame is built again for the budget that comes back. */
		if (budget > 0) {
			m_hist_n = hist_build(budget, &len, &m_hist_next);
			if (m_hist_n == 0) {
				return;
			}
			m_hist_len = (uint16_t)len;
		}
	}
	f.len = m_hist_len;

	struct app_radio_tx_result res = {0};
	int ret = tx_send(&f, &res);

	if (ret == 0 && f.len == 0) {
		ret = -EAGAIN; /* only the MAC flush went */
	}
	switch (ret) {
	case 0:
		break;
	case -EBUSY:
		return; /* kicked when the in-flight uplink is done */
	case -ENOTCONN:
		LOG_WRN("History replay aborted: no session");
		hist_finish();
		return;
	case TX_ACK_RETRY:
		k_work_reschedule_for_queue(app_radio_work_q(), &m_hist_work, K_MSEC(res.wait_ms));
		return;
	default:
		/* Duty cycle, MAC busy, a budget that fell under the frame: the
		 * same frame again, a bounded number of times (#89). */
		if (++m_hist_retries > TX_MAX_RETRIES) {
			LOG_ERR("History frame %u/%u abandoned after %d retries", m_hist_idx + 1,
				m_hist_count, TX_MAX_RETRIES);
			hist_finish();
			return;
		}
		app_radio_count(APP_RADIO_CNT_RETRY);
		k_work_reschedule_for_queue(
			app_radio_work_q(), &m_hist_work,
			K_MSEC((ret == -EAGAIN && res.wait_ms) ? res.wait_ms : TX_RETRY_MS));
		return;
	}

	m_hist_retries = 0;
	/* M-2: the replay holds telemetry back, so its frames prove the channel;
	 * without this a long replay tripped the stale-uplink rejoin mid-stream. */
	app_radio_note_uplink();
	LOG_INF("History frame %u/%u sent (%u rec, %u B)", m_hist_idx + 1, m_hist_count, m_hist_n,
		f.len);
	m_hist_cursor = m_hist_next;
	m_hist_idx++;

	/* The export skips to the next record in the window, so the frame
	 * carrying the window's last record ends the replay here: no trailing
	 * empty attempt (H-4). */
	if (m_hist_cursor < m_hist_end) {
		k_work_reschedule_for_queue(app_radio_work_q(), &m_hist_work,
					    K_MSEC(HIST_FRAME_GAP_MS));
	} else {
		LOG_INF("History replay complete: %u frames", m_hist_idx);
		hist_finish();
	}
}

int app_radio_history_replay_start(uint32_t from_unix, uint32_t to_unix, uint32_t seq)
{
	if (m_be == NULL || !m_be->tx_ready()) {
		LOG_WRN("History replay requested but the link is down; ignoring");
		return -EAGAIN;
	}

	/* One stream at a time. The request being answered can arrive again from
	 * inside the stream's own call stack: P2P dispatches the 0x56 it receives
	 * while waiting for a frame's Ack (hist_work_handler -> send -> ... ->
	 * app_cmd_handle_req_history -> here). Re-seeding the cursor there would
	 * have the outer run write its stale values back over it and answer one
	 * request with two interleaved streams. The stream is the answer, so the
	 * caller sends no error either. */
	if (m_hist_active) {
		LOG_INF("History replay already streaming (seq %u); request (seq %u) ignored",
			m_hist_seq, seq);
		return 0;
	}

	/* The fields the cap depends on first, so counting and sending size the
	 * frames alike. */
	m_hist_from = from_unix;
	m_hist_to = to_unix;
	m_hist_seq = seq;
	m_hist_present = app_history_get_mask();
	m_hist_interval = app_history_get_interval();

	/* #409 3f: count with the worst-case bound, then tighten the bound to that
	 * count and count again. A bigger cap never needs more frames, so the
	 * count stays within the bound. */
	uint8_t budget = m_be->budget();

	m_hist_frame_bound = UINT32_MAX;

	size_t cap = hist_frame_cap(budget);
	uint32_t n = cap > 0 ? app_history_count_frames(from_unix, to_unix, cap) : 0;

	if (n > 0) {
		m_hist_frame_bound = n;
		n = app_history_count_frames(from_unix, to_unix, hist_frame_cap(budget));
	}
	if (n == 0) {
		/* An empty window, or records not one of which fits the budget
		 * (the 11 B LoRaWAN tier)? The whole frame buffer tells them apart. */
		if (app_history_count_frames(from_unix, to_unix, sizeof(m_hist_buf)) > 0) {
			LOG_WRN("History replay: budget %u B too small for one record", budget);
			return -EMSGSIZE;
		}
		LOG_INF("History replay: no records in window");
		return -ENODATA;
	}

	m_hist_count = n;
	m_hist_idx = 0;
	app_history_span(&m_hist_cursor, &m_hist_end);
	m_hist_retries = 0;
	m_hist_active = true;
	/* Capture goes on (absolute cursor); only the flash page rollover is held
	 * off. */
	app_history_set_replay_active(true);

	LOG_INF("History replay start: %u frames (window %u..%u, seq %u)", n, from_unix, to_unix,
		seq);
	k_work_reschedule_for_queue(app_radio_work_q(), &m_hist_work, K_NO_WAIT);
	return 0;
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
	(void)k_work_cancel_delayable_sync(&m_hist_work, &sync);
	m_cur_valid = false;
	atomic_clear(&m_tlm_requested);
	m_tlm_open = false;
	m_tlm_frame = false;
	m_hist_active = false;
	m_ack_pending = false;
	m_ready_cb = NULL;
	app_radio_duty_init(0); /* no limit unless a test sets one */
}

void app_radio_test_link_reset(void)
{
	m_link = (struct app_radio_link){0};
	atomic_clear(&m_link_forced);
	m_last_uplink_ms = 0;
	m_dc = (struct app_radio_stale_dc){0};
	m_dc_hold_logged = false;
}

void app_radio_test_stale_tick(int64_t now_ms)
{
	stale_tick(now_ms);
}

void app_radio_test_duty_charge_at(int64_t end_ms, uint32_t air_ms)
{
	k_spinlock_key_t key = k_spin_lock(&m_duty_lock);

	app_radio_ledger_charge(&m_duty, end_ms, air_ms);
	k_spin_unlock(&m_duty_lock, key);
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

	for (size_t i = 0; i < sizeof(g_app_config.radio_deveui); i++) {
		h = (h ^ g_app_config.radio_deveui[i]) * 16777619U;
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
	m_ready_cb = cb;
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

/* Retry pace while something of the announce stays pending: a frame the
 * answer queue or the budget refused, or pages still streaming. A retry only
 * re-checks for room; with ADR off or a pinned DR nothing else would come to
 * retry, and the held alarms and first report wait for the announce. */
#define ANNOUNCE_RETRY_SEC 5

static bool announce_run(void);

static void announce_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (announce_run()) {
		k_work_reschedule_for_queue(app_radio_work_q(), &m_announce_work,
					    K_SECONDS(ANNOUNCE_RETRY_SEC));
	}
}

/* Run the pending announce frames now, on the radio work queue. */
static void announce_kick(void)
{
	k_work_reschedule_for_queue(app_radio_work_q(), &m_announce_work, K_NO_WAIT);
}

void app_radio_announce_kick(void)
{
	if (app_radio_announce_pending()) {
		announce_kick();
	}
}

/* #409 3d/3e, #425: the page stream of an answer that did not fit one frame.
 * One page per run, paced, and only while the answer queue keeps two slots
 * free, so an alarm and another answer always fit between the pages. A lost
 * link cancels it: a rejoin starts from scratch. */
#define PAGE_STREAM_PACE_SEC 2

static void page_stream_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (m_be == NULL || m_be->get_state() != APP_RADIO_STATE_HEALTHY) {
		app_cmd_stream_cancel();
		return;
	}
	if (app_radio_tx_answer_free() < 2) {
		k_work_schedule_for_queue(app_radio_work_q(), &m_page_stream_work,
					  K_SECONDS(PAGE_STREAM_PACE_SEC));
		return;
	}

	uint8_t buf[APP_RADIO_TX_SLOT_SIZE];
	size_t len;
	int ret = app_cmd_stream_next(buf, sizeof(buf), &len);

	if (ret) {
		if (ret != -ENODATA) {
			LOG_ERR_CALL_FAILED_INT("app_cmd_stream_next", ret);
		}
		/* All pages queued (or the stream died): an announce may have waited
		 * for it. */
		app_radio_announce_kick();
		return;
	}
	(void)app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_CMD_RESPONSE, 0, buf, len);
	k_work_schedule_for_queue(app_radio_work_q(), &m_page_stream_work,
				  K_SECONDS(PAGE_STREAM_PACE_SEC));
}

/* Page 0 went out and more pages follow: start the page stream. */
static void page_stream_kick(void)
{
	k_work_schedule_for_queue(app_radio_work_q(), &m_page_stream_work,
				  K_SECONDS(PAGE_STREAM_PACE_SEC));
}

/* ---- Downlink commands (doc/plan/460 §2.5, F3) ---------------------------- */

/* A deferred action waits for its answer: 8 s covers a send and its receive
 * windows; a duty-cycle-held or retrying answer takes longer, so the wait is
 * re-checked, at most POST_CMD_DRAIN_MAX_DEFERRALS times -- a TX that keeps
 * failing must not postpone the commanded action forever. The action (mostly a
 * reboot) also waits for the alarm frames still queued and for an alarm batch
 * that can go now, which is sent early instead of at the end of its window
 * (#462). */
#define POST_CMD_DRAIN_WAIT_SEC      8
#define POST_CMD_DRAIN_MAX_DEFERRALS 6

static enum app_cmd_action m_post_cmd_action;
static uint8_t m_post_cmd_deferrals;

static bool tx_undelivered(void)
{
	return app_radio_tx_answer_pending() || app_radio_tx_alarm_pending() ||
	       app_radio_ack_pending();
}

static void post_cmd_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	bool alarm_batch = app_alarm_flush_pending();

	if ((alarm_batch || tx_undelivered()) &&
	    m_post_cmd_deferrals < POST_CMD_DRAIN_MAX_DEFERRALS) {
		m_post_cmd_deferrals++;
		LOG_WRN("Post-command action %d deferred: frames still undelivered (%u/%u)",
			(int)m_post_cmd_action, (unsigned)m_post_cmd_deferrals,
			(unsigned)POST_CMD_DRAIN_MAX_DEFERRALS);
		k_work_schedule_for_queue(app_radio_work_q(), &m_post_cmd_work,
					  K_SECONDS(POST_CMD_DRAIN_WAIT_SEC));
		return;
	}
	app_cmd_run_action(m_post_cmd_action);
}

void app_radio_downlink(const uint8_t *buf, size_t len)
{
	/* Radio work queue only, so one static buffer serves both radios and
	 * keeps the answer off the deepest stack (P2P runs this from inside its
	 * receive path). */
	static uint8_t resp[APP_RADIO_TX_SLOT_SIZE];
	size_t resp_len = 0;
	enum app_cmd_action action = APP_CMD_ACTION_NONE;

	if (m_be == NULL) {
		return;
	}

	/* Cap to the payload budget of the next uplink, not just the buffer, so
	 * a GetInfo command gets the same trimming as the autonomous Info
	 * (#409 3g); an answer that still does not fit is paged. */
	int ret = app_cmd_handle((enum app_cmd_transport)m_be->cmd_transport, buf, len, resp,
				 app_radio_tx_answer_cap(sizeof(resp)), &resp_len, &action);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_cmd_handle", ret);
		return;
	}

	if (resp_len > 0) {
		ret = app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_CMD_RESPONSE, 0,
					 resp, resp_len);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_radio_tx_queue", ret);
		}
	}

	if (action == APP_CMD_ACTION_PAGE_STREAM) {
		/* #409/#425: the remaining pages follow page 0 by themselves. */
		page_stream_kick();
	} else if (action != APP_CMD_ACTION_NONE) {
		m_post_cmd_action = action;
		m_post_cmd_deferrals = 0;
		k_work_schedule_for_queue(app_radio_work_q(), &m_post_cmd_work,
					  K_SECONDS(POST_CMD_DRAIN_WAIT_SEC));
		LOG_INF("Post-command action %d scheduled in %ds", (int)action,
			POST_CMD_DRAIN_WAIT_SEC);
	}

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	/* The deepest thing the radio work queue does (see RADIO_WQ_STACK_SIZE),
	 * so this is where its real high-water shows. */
	size_t unused;

	if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
		LOG_INF("Radio work queue stack: %zu B unused after cmd handle", unused);
	}
#endif /* defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO) */
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

static bool announce_run(void)
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

	/* No room in the answer queue (a long duty-cycle hold keeps it full):
	 * retry without encoding a frame it would refuse. */
	if ((atomic_get(&m_announce) & ANNOUNCE_INFO) && app_radio_tx_answer_free() > 0) {
		if (announce_frame(false, 0) == 0) {
			atomic_and(&m_announce, ~ANNOUNCE_INFO);
			LOG_INF("Info announced");
		}
		if (app_cmd_stream_active()) {
			return true; /* settings-info follows once these pages are out */
		}
	}
	if ((atomic_get(&m_announce) & ANNOUNCE_SETTINGS) && app_radio_tx_answer_free() > 0) {
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

#if defined(CONFIG_ZTEST)
void app_radio_test_cmd_reset(void)
{
	struct k_work_sync sync;

	(void)k_work_cancel_delayable_sync(&m_post_cmd_work, &sync);
	(void)k_work_cancel_delayable_sync(&m_page_stream_work, &sync);
	(void)k_work_cancel_delayable_sync(&m_announce_work, &sync);
	(void)k_work_cancel_delayable_sync(&m_announce_jitter_work, &sync);
	(void)k_work_cancel_delayable_sync(&m_seq_deadline_work, &sync);
	(void)k_work_cancel_delayable_sync(&m_jitter_work, &sync);
	m_post_cmd_action = APP_CMD_ACTION_NONE;
	m_post_cmd_deferrals = 0;
	atomic_clear(&m_announce);
	atomic_clear(&m_announce_spreading);
	atomic_clear(&m_seq_closed);
	atomic_clear(&m_telemetry_held);
}

void app_radio_test_air_reset(void)
{
	k_mutex_lock(&m_air_mutex, K_FOREVER);
	m_on_air = false;
	m_flash_writers = 0;
	m_flash_waiting = 0;
	k_condvar_broadcast(&m_air_cv);
	k_mutex_unlock(&m_air_mutex);
}
#endif

/* A network time younger than this answers a clock_sync at once (PF-2). The
 * same span as app_clock's forced-resync cooldown (#340 L11): on LoRaWAN a
 * second clock_sync inside it queued no DeviceTimeReq, so its answer waited
 * for the next time that landed -- up to the weekly re-sync. */
#define CLOCK_SYNC_FRESH_MS (60 * 1000)

/* The seq the clock_sync Info answers, and whether it waits for a network
 * time. Set from any thread; the answer goes on the radio work queue. */
static atomic_t m_clock_sync_seq;
static atomic_t m_clock_sync_pending;

static void clock_sync_answer_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)app_radio_send_info((uint32_t)atomic_get(&m_clock_sync_seq));
}

static K_WORK_DEFINE(m_clock_sync_answer_work, clock_sync_answer_work_handler);

/* Radio work queue: a fresh network time answers at once, otherwise the
 * backend is asked for one and the request waits for app_radio_time_event().
 * On the queue, so a time landing between the request and this check makes
 * it fresh instead of being missed. */
static void clock_sync_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int64_t landed_ms = app_clock_network_time_at_ms();

	if (landed_ms != 0 && k_uptime_get() - landed_ms < CLOCK_SYNC_FRESH_MS) {
		atomic_clear(&m_clock_sync_pending);
		(void)app_radio_send_info((uint32_t)atomic_get(&m_clock_sync_seq));
		return;
	}
	atomic_set(&m_clock_sync_pending, 1);
	if (m_be && m_be->time_request) {
		m_be->time_request();
	}
}

static K_WORK_DEFINE(m_clock_sync_work, clock_sync_work_handler);

void app_radio_clock_sync(uint32_t seq)
{
	/* seq before the work, so an answer never pairs a stale seq. */
	atomic_set(&m_clock_sync_seq, (atomic_val_t)seq);
	k_work_submit_to_queue(app_radio_work_q(), &m_clock_sync_work);
}

void app_radio_time_event(void)
{
	if (atomic_cas(&m_clock_sync_pending, 1, 0)) {
		k_work_submit_to_queue(app_radio_work_q(), &m_clock_sync_answer_work);
	}
}

bool app_radio_clock_sync_pending(void)
{
	return atomic_get(&m_clock_sync_pending) != 0;
}

#if defined(CONFIG_ZTEST)
void app_radio_test_clock_sync_reset(void)
{
	struct k_work_sync sync;

	k_work_cancel_sync(&m_clock_sync_work, &sync);
	k_work_cancel_sync(&m_clock_sync_answer_work, &sync);
	atomic_clear(&m_clock_sync_pending);
	atomic_clear(&m_clock_sync_seq);
}
#endif
