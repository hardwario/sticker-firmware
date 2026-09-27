/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm_rules.h"
#include "app_battery.h"
#include "app_cmd.h"
#include "app_clock.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_history.h"
#include "app_log.h"
#include "app_radio_lrw.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

/* LoRaMac includes */
#include <LoRaMac.h>
#include <LoRaMacCrypto.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_radio_lrw, LOG_LEVEL_DBG);

/*
 * State-machine refactor (#71).
 *
 * Concurrency model: ALL state and counters are mutated only on the radio work queue
 * (serialized). Callbacks (downlink / link-check / DR-change) and timer ISRs
 * run in other contexts and ONLY enqueue work or msgq items — they never touch
 * m_state, the counters, or m_link_check_pending directly.
 *
 * m_state is changed ONLY through state_transition(), which runs the exit
 * action of the old state and the entry action of the new state (counter
 * resets, timer start/stop) in one place.
 *
 * Two independent timers, each with a single meaning (no more inferring a
 * timer's purpose from the current state):
 *   - m_lc_timeout_timer  : link-check response timeout only
 *   - m_rejoin_timer      : rejoin backoff only
 *
 * The periodic report cadence lives in app_report (#126). The uplink queues,
 * the scheduler and the report split live in app_radio (doc/plan/460 F4); this
 * backend sends the one frame app_radio asks for (lrw_tx_send(): DR budget,
 * LinkCheckReq piggyback, MAC flush) and streams a history replay. On a
 * link-ready edge (join success / replay finish) app_radio_lrw kicks the TX
 * scheduler and app_report via the registered callback.
 */

/* Link check configuration constants.
 * The LC cadence (every N-th uplink) and the failures-before-rejoin threshold
 * are runtime-configurable: g_app_config.radio_link_check_interval and
 * radio_link_check_fail_rejoin (config keys radio-link-check-interval /
 * radio-link-check-fail-rejoin), shared by both radios via app_radio (2026-09-27
 * rename from lrw_link_check_*; the LoRaWAN proto_id/proto_group are unchanged). */
#define LINK_CHECK_TIMEOUT_SEC 10 /* LC answer timeout, from lorawan_send() return */

/* All work runs on the radio work queue, app_radio_work_q() (doc/plan/439 T2a).
 * The link supervision (WARNING, the rejoin budget), the rejoin backoff, the
 * liveness heartbeat and the M-2 stale-uplink watchdog are app_radio's, one
 * implementation for both radios (doc/plan/460 F2); this backend reports the
 * link-check outcomes and carries out a recovery rung or a rejoin. */

static void publish_mac(void);

/* Every uplink goes through here, so the M-2 duty-cycle streak sees each result.
 * A send refused by the EU868 duty cycle (-ECONNREFUSED,
 * LORAMAC_STATUS_DUTYCYCLE_RESTRICTED) is a throttled MAC, not a mute node: a
 * rejoin would only reset the band credits kept in RAM (F29). */
static int lrw_send(uint8_t port, uint8_t *data, uint8_t len, enum lorawan_message_type type)
{
	/* lorawan_send() returns once the RX windows closed: the whole exchange
	 * is clear of flash writes (app_radio_air_begin()). */
	app_radio_air_begin();
	int ret = lorawan_send(port, data, len, type);

	app_radio_air_end();
	app_radio_count(ret == 0 ? APP_RADIO_CNT_TX : APP_RADIO_CNT_TX_ERR);
	app_radio_note_send(ret == 0, ret == -ECONNREFUSED);
	if (ret == 0) {
		app_radio_set_duty_held(false);
		publish_mac();
	} else if (ret == -ECONNREFUSED) {
		app_radio_set_duty_held(true);
	}
	return ret;
}

/* --- Timers (each one meaning only) --- */
static struct k_timer m_lc_timeout_timer;
static struct k_timer m_rejoin_timer;

/* --- Works --- */
static struct k_work m_join_work;
static struct k_work m_link_check_work;       /* LC timeout (from m_lc_timeout_timer) */
static struct k_work m_downlink_success_work; /* deferred from downlink_callback */
static struct k_work m_clock_sync_info_work;  /* deferred ClockSync Info uplink (#219) */
static struct k_work m_lc_response_work;      /* deferred from link_check_callback */
static struct k_work m_dl_request_work;       /* drains m_dl_msgq (port-85 commands) */
static struct k_work_delayable m_join_complete_work;
static struct k_work_delayable m_hist_work;

/* Gap before the next frame of a multi-frame uplink (covers the RX1/RX2 windows)
 * and backoff when a send is refused (duty cycle / MAC busy). */
#define FRAME_GAP_SEC   3
#define FRAME_RETRY_SEC 15

/* History replay (#52): one ReqHistory streams all matching records back as N
 * HistoryFrame uplinks on the command port, ASAP. */
/* Staging buffer for the raw sample bytes of one HistoryFrame before it is
 * protobuf-encoded into m_hist_tx_buf. history_frame_cap() bounds the export to
 * MIN(DR budget, sizeof(m_hist_tx_buf)), so this must be at least as large as
 * m_hist_tx_buf — a fixed 48 B (the pre-#260 nanopb bound) under-sized it and let
 * app_history_export_page() overrun the stack on any DR whose budget exceeds 48 B. */
#define HISTORY_SAMPLES_MAX APP_CMD_HISTORY_FRAME_BUF_SIZE
#define HISTORY_MAX_RETRIES 8 /* duty-cycle/MAC-busy retries before aborting a frame (#89) */

static bool m_hist_active;
static uint32_t m_hist_from, m_hist_to, m_hist_seq;
static uint32_t m_hist_count;
/* #409 3f: upper bound for frame_index/frame_count when sizing a frame (their
 * varint width). UINT32_MAX = worst case; tightened to the first frame count at
 * replay start, which buys ~8 B of samples per frame at low DRs. */
static uint32_t m_hist_frame_bound = UINT32_MAX;
static uint32_t m_hist_idx;
/* Absolute record ordinals (app_history_span()): next record to send and the end
 * of the replay (exclusive), snapshot at start. Captures keep appending during a
 * replay and the RAM ring may evict under it, but an absolute cursor names the
 * same record throughout, so nothing is repeated or skipped; records captured
 * after the start are left for the next replay. */
static uint32_t m_hist_cursor;
static uint32_t m_hist_end;
static uint32_t m_hist_present; /* shared sensor mask (uint32), snapshot at replay start */
static uint32_t m_hist_interval;
static int m_hist_retries; /* consecutive lorawan_send failures on the current frame (#89) */
/* Full encoded HistoryFrame Response + version byte; the old 64 B overflowed
 * once samples filled (frame ~70-90 B) so replay silently died on DR3+ (#89). */
static uint8_t m_hist_tx_buf[APP_CMD_HISTORY_FRAME_BUF_SIZE];

static void m_hist_work_handler(struct k_work *work);

/* --- State machine --- */
static atomic_t m_state = ATOMIC_INIT(APP_RADIO_STATE_IDLE);
static bool m_link_check_pending; /* Waiting for LC response */
static int m_rejoin_attempts;     /* Rejoin attempt counter for backoff */
static int m_join_busy_polls;     /* Counter for MAC busy polling */
static bool m_init_join;          /* True for first join after boot */
static bool m_mac_started;        /* lorawan_start() succeeded; LoRaMac state is valid */

#define JOIN_BUSY_POLL_INTERVAL_MS 500
#define JOIN_BUSY_MAX_POLLS        30

/* Application-payload budget (bytes), refreshed from lorawan_get_payload_sizes()
 * before each composing TX (MED-6: was cached only on DR-change/join). */
static uint8_t m_max_next_payload;
static int16_t m_last_rssi;
static int8_t m_last_snr;
static uint8_t m_last_margin;
static uint8_t m_last_gw_count;
static uint8_t m_lc_response_gw_count;

/* --- Downlink command queue (MED-7: was a single overwrite-able slot) --- */
/* Incoming command buffer. The network can deliver up to the LoRaWAN MTU
 * (~222 B at the highest DR) on a single downlink; a realistic SetParam with
 * deveui+joineui+appkey encodes to ~90 B. 224 covers the full MTU so large
 * commands are never silently dropped (#93.4 — was 64, which dropped them with
 * only a LOG_WRN and the host never learned the command wasn't processed). */
#define APP_RADIO_LRW_REQUEST_BUF_SIZE  224
#define APP_RADIO_LRW_DOWNLINK_CMD_PORT 85
#define APP_RADIO_LRW_ALARM_PORT        3
#define APP_RADIO_LRW_TELEMETRY_PORT    2
/* Downlink-command FIFO. Each slot is a full-MTU lrw_dl_msg (~228 B), and the
 * network delivers at most one port-85 command per RX window, drained promptly by
 * m_dl_request_work. Depth 2 absorbs a back-to-back pair while halving the buffer
 * vs the old depth 4 (saves ~456 B RAM, #221.4). */
#define APP_RADIO_LRW_DL_QUEUE_DEPTH    2

/* The request buffer must hold a full-MTU downlink so the largest command the
 * network can deliver still fits (#93.4/#93.7). Response/frame buffers are
 * deliberately NOT asserted against the nanopb *_size bounds: those frames are
 * DR-budget-limited and paged (get_config/get_param/history), so the on-air
 * size is always far below the protobuf worst case. */
BUILD_ASSERT(APP_RADIO_LRW_REQUEST_BUF_SIZE >= 222, "request buffer below LoRaWAN MTU");

struct lrw_dl_msg {
	uint16_t len;
	uint8_t buf[APP_RADIO_LRW_REQUEST_BUF_SIZE];
};

K_MSGQ_DEFINE(m_dl_msgq, sizeof(struct lrw_dl_msg), APP_RADIO_LRW_DL_QUEUE_DEPTH, 4);

/* Set by a ClockSync command; the next network time-update sends an Info uplink
 * (with the synced unix_time) instead of the command acking immediately. */
/* #193: set from a command-handler thread, test-and-cleared in the LoRaMac
 * downlink callback (another context) — an atomic bit closes the lost-update race. */
static atomic_t m_clock_sync_info_pending;
/* seq of the ClockSync command the deferred Info answers (F13: without it the
 * answer went out with seq 0 and the host could not pair it). Written by the
 * command handler before the pending bit is set, read by the Info work item. */
static atomic_t m_clock_sync_info_seq;

/* Kicked on a link-ready edge (join success / history-replay finish) so
 * app_report can resume the report cadence with an immediate uplink. */
static void (*m_ready_cb)(void);

/* Forward declarations */
static void on_join_success(void);
static void on_join_failure(void);
static void on_lc_success(void);
static void on_lc_failure(void);
static void on_lc_timeout(void);
static void on_downlink_received(void);
static void state_transition(enum app_radio_state new_state);
static int apply_channel_plan(void);

static void fire_ready_cb(void)
{
	if (m_ready_cb) {
		m_ready_cb();
	}
}

/* The common rejoin backoff (60 s doubling to 1 h, +/-25 % jitter: M-1). */
static uint32_t calculate_rejoin_backoff_ms(int attempt)
{
	uint32_t base = app_radio_rejoin_backoff_ms((uint32_t)attempt);

	return (uint32_t)app_radio_backoff_jitter_ms(base, 0, base, sys_rand32_get());
}

static uint8_t refresh_payload_budget(void)
{
	uint8_t max_next = 0, max_now = 0;

	lorawan_get_payload_sizes(&max_next, &max_now);
	m_max_next_payload = max_next;
	return max_next;
}

uint8_t app_radio_lrw_get_max_payload(void)
{
	return m_max_next_payload;
}

/* Link-recovery ladder, one rung per failed link check in WARNING. LoRaMac's own
 * ADR backoff needs ADR_ACK_LIMIT + 2 * ADR_ACK_DELAY = 128 unanswered uplinks
 * before its first data-rate step (~32 h at the default 900 s report interval),
 * so the state machine always rejoined long before it helped. Walk the same
 * ladder here: restore the default (maximum) TX power, then drop the data rate
 * one step towards the region minimum. A device moved out of reach of its
 * ADR-optimised DR (or whose nearest gateway went away) reaches a gateway again
 * without losing the session; with ADR on, the network raises the DR again from
 * the uplinks it now receives. Returns true if a rung was taken, false at the
 * floor (default TX power, minimum DR) — the caller then falls back to a rejoin.
 * Runs on the radio work queue. */
static bool lrw_backoff_step(void)
{
	MibRequestConfirm_t mib;
	bool stepped = false;
	int8_t pwr, def_pwr, new_pwr, dr;

	lorawan_mac_lock();
	mib.Type = MIB_CHANNELS_DEFAULT_TX_POWER;
	LoRaMacMibGetRequestConfirm(&mib);
	def_pwr = mib.Param.ChannelsDefaultTxPower;

	mib.Type = MIB_CHANNELS_TX_POWER;
	LoRaMacMibGetRequestConfirm(&mib);
	pwr = mib.Param.ChannelsTxPower;
	new_pwr = pwr;

	/* TX power is an index: 0 is the maximum EIRP, higher is weaker. */
	if (pwr > def_pwr) {
		mib.Param.ChannelsTxPower = def_pwr;
		if (LoRaMacMibSetRequestConfirm(&mib) == LORAMAC_STATUS_OK) {
			new_pwr = def_pwr;
			stepped = true;
		}
	}

	mib.Type = MIB_CHANNELS_DATARATE;
	LoRaMacMibGetRequestConfirm(&mib);
	dr = mib.Param.ChannelsDatarate;
	lorawan_mac_unlock();

	int8_t min_dr = (int8_t)lorawan_get_min_datarate();
	int8_t new_dr = dr;

	if (dr > min_dr) {
		int ret;

		new_dr = dr - 1;
		if (g_app_config.lrw_adr) {
			/* lorawan_set_datarate() refuses while ADR is on; the MAC keeps
			 * ChannelsDatarate as the ADR starting point, so set it directly. */
			mib.Type = MIB_CHANNELS_DATARATE;
			mib.Param.ChannelsDatarate = new_dr;
			lorawan_mac_lock();
			ret = LoRaMacMibSetRequestConfirm(&mib) == LORAMAC_STATUS_OK ? 0 : -EINVAL;
			lorawan_mac_unlock();
		} else {
			/* ADR off: lorawan_send() passes its own DR with every frame, which
			 * overrides the MIB, so go through the Zephyr API. */
			ret = lorawan_set_datarate((enum lorawan_datarate)new_dr);
		}
		if (ret) {
			LOG_WRN("Link recovery: DR%d -> DR%d refused: %d", dr, new_dr, ret);
			new_dr = dr;
		} else {
			stepped = true;
		}
	}

	if (stepped) {
		refresh_payload_budget();
		LOG_WRN("Link recovery: TX power %d -> %d, DR%d -> DR%d (payload %u B)", pwr,
			new_pwr, dr, new_dr, m_max_next_payload);
	}
	return stepped;
}

/* The DR budget capped to `buf_size`, re-queried from the stack (MED-6). 0 = no
 * budget known right now (before join, or pending MAC answers fill the frame):
 * encode against the buffer and let the send flush the MAC and retry. Only on
 * the radio work queue: lorawan_get_payload_sizes() calls into the
 * non-thread-safe LoRaMac. */
static size_t refresh_payload_cap(size_t buf_size)
{
	uint8_t budget = refresh_payload_budget();

	return (budget > 0 && budget < buf_size) ? budget : buf_size;
}

/* ======================================================================== */
/* State machine                                                            */
/* ======================================================================== */

static const char *state_name(enum app_radio_state s)
{
	switch (s) {
	case APP_RADIO_STATE_IDLE:
		return "IDLE";
	case APP_RADIO_STATE_JOINING:
		return "JOINING";
	case APP_RADIO_STATE_HEALTHY:
		return "HEALTHY";
	case APP_RADIO_STATE_WARNING:
		return "WARNING";
	case APP_RADIO_STATE_RECONNECT:
		return "RECONNECT";
	case APP_RADIO_STATE_DISABLED:
		return "DISABLED";
	default:
		return "?";
	}
}

/* The ONLY place m_state changes. Runs exit action of the old state then entry
 * action of the new state. Must be called on the radio work queue. */
static void state_transition(enum app_radio_state new_state)
{
	enum app_radio_state old = (enum app_radio_state)atomic_get(&m_state);

	/* --- Exit actions --- */
	switch (old) {
	case APP_RADIO_STATE_JOINING:
		k_work_cancel_delayable(&m_join_complete_work);
		app_radio_air_end(); /* the join exchange join_work_handler() began */
		break;
	case APP_RADIO_STATE_HEALTHY:
		k_timer_stop(&m_lc_timeout_timer);
		m_link_check_pending = false;
		break;
	case APP_RADIO_STATE_RECONNECT:
		k_timer_stop(&m_rejoin_timer);
		break;
	default:
		break;
	}

	atomic_set(&m_state, new_state);
	LOG_INF("State: %s -> %s", state_name(old), state_name(new_state));

	/* --- Entry actions --- */
	switch (new_state) {
	case APP_RADIO_STATE_IDLE:
	case APP_RADIO_STATE_DISABLED:
		/* Radio-silent: no link-check, no rejoin. (The report cadence is
		 * app_report's; it self-pauses while not app_radio_lrw_is_ready().) */
		k_timer_stop(&m_lc_timeout_timer);
		k_timer_stop(&m_rejoin_timer);
		break;

	case APP_RADIO_STATE_JOINING:
		/* Drop any in-flight history replay across (re)join. */
		m_hist_active = false;
		app_history_set_replay_active(false);
		break;

	case APP_RADIO_STATE_HEALTHY:
		/* Joined: the rejoin episode is over. The link supervision starts
		 * afresh in app_radio_link_up(). */
		m_rejoin_attempts = 0;
		app_radio_set_join_attempts(0);
		publish_mac();
		m_link_check_pending = false;
		break;

	case APP_RADIO_STATE_RECONNECT: {
		/* Single, consistent rejoin policy (HIGH-4): always escalate the
		 * backoff and arm the rejoin timer here — never reset attempts. */
		uint32_t backoff_ms = calculate_rejoin_backoff_ms(m_rejoin_attempts);

		m_rejoin_attempts++;
		app_radio_set_join_attempts((uint32_t)m_rejoin_attempts);
		LOG_WRN("Rejoin in %u s (attempt %d)", backoff_ms / 1000, m_rejoin_attempts);
		k_timer_start(&m_rejoin_timer, K_MSEC(backoff_ms), K_FOREVER);
		break;
	}

	default:
		break;
	}
}

/* ======================================================================== */
/* Event handlers (run on the radio work queue)                             */
/* ======================================================================== */

/* Pin the uplink datarate from lrw-datarate (#409 A3, like twr-sdk AT$DR). Runs
 * after every (re)join, after ADR is configured and before the payload budget is
 * captured, so the budget reflects the pinned DR. It does NOT set the join DR:
 * lorawan_start() (boot and each rejoin's MAC re-init) resets the stack's DR to
 * the region default, so JoinRequests always go out at that DR and the pin is
 * re-applied here once the join succeeds. Validity is region/dwell dependent (e.g. AU915
 * dwell=1 rejects DR0/DR1): an invalid DR is rejected by the MAC and the stack's
 * own DR stays in use. Calibration pins its own DR and is left alone. */
static void apply_manual_datarate(void)
{
	if (g_app_config.lrw_datarate == APP_CONFIG_LRW_DATARATE_AUTO || g_app_config.calibration) {
		return;
	}

	int dr = (int)g_app_config.lrw_datarate - (int)APP_CONFIG_LRW_DATARATE_DR0;

	if (g_app_config.lrw_adr) {
		LOG_WRN("lrw-datarate DR%d ignored: ADR is on (set lrw-adr false)", dr);
		return;
	}

	int ret = lorawan_set_datarate((enum lorawan_datarate)dr);
	if (ret) {
		LOG_ERR("lrw-datarate DR%d rejected in this region (%d); stack DR kept", dr, ret);
		return;
	}
	LOG_INF("Uplink datarate pinned to DR%d (lrw-datarate)", dr);
}

static void on_join_success(void)
{
	LOG_INF("Join successful");
	m_init_join = false; /* Next join will be a rejoin with MAC reset */
	lorawan_enable_adr(g_app_config.lrw_adr);
	apply_manual_datarate();

	/* Capture the initial DR's payload budget; the DR-changed callback may not
	 * fire on join. */
	refresh_payload_budget();

	state_transition(APP_RADIO_STATE_HEALTHY); /* resets the rejoin attempts */
	app_radio_link_up(); /* link supervision and the M-2 clock start afresh */

	/* Request network time once joined; the answer sets the RTC asynchronously. */
	app_clock_request_sync();

	/* Autonomous Info + settings-info ConfigDump (#412) on join, through the
	 * common announce (app_radio): identity/firmware and the effective config on
	 * fPort 85 before the first telemetry. The app_radio scheduler sends queued
	 * answers ahead of alarms and telemetry. */
	app_radio_announce();

	/* Frames queued while the link was down leave now. */
	app_radio_tx_kick();

	/* Kick app_report to start the report cadence with an immediate uplink (its
	 * cycle samples, captures and requests telemetry; the first frame carries
	 * LC, msg #1). */
	fire_ready_cb();
}

static void on_join_failure(void)
{
	/* MAC activation failure (start/join error or not-activated). Applies to
	 * both OTAA and ABP — this is about getting the MAC session up, not link
	 * health. The RECONNECT entry action arms the backoff. */
	state_transition(APP_RADIO_STATE_RECONNECT);
}

/* A link-check outcome: this backend resolves its pending LinkCheckReq, the
 * common supervision (app_radio_link_result()) decides what follows. */
static void on_lc_failure(void)
{
	k_timer_stop(&m_lc_timeout_timer);
	m_link_check_pending = false;
	app_radio_link_result(false);
}

static void on_lc_success(void)
{
	k_timer_stop(&m_lc_timeout_timer);
	m_link_check_pending = false;
	app_radio_link_result(true);
}

static void on_lc_timeout(void)
{
	enum app_radio_state state = (enum app_radio_state)atomic_get(&m_state);

	if (state != APP_RADIO_STATE_HEALTHY) {
		return;
	}
	if (m_link_check_pending) {
		LOG_WRN("Link check timeout - no response received");
		on_lc_failure();
	} else {
		LOG_DBG("LC timeout fired but already resolved");
	}
}

static void on_downlink_received(void)
{
	enum app_radio_state state = (enum app_radio_state)atomic_get(&m_state);

	if (state != APP_RADIO_STATE_HEALTHY) {
		return;
	}
	/* Any authenticated downlink proves the link, as on P2P (decision #22
	 * §3.4, Hynek 2026-09-27): it counts as a link-check success whether or
	 * not a LinkCheckReq is outstanding -- the fail streak is cleared and, in
	 * WARNING, it counts towards the return to HEALTHY. A pending check is
	 * resolved by it; its late LinkCheckAns then finds nothing pending
	 * (lc_response_work_handler). Before, a downlink without a pending check
	 * was only logged, so a WARNING node receiving commands or ADR requests
	 * stayed in WARNING until its next own link check. */
	LOG_INF("Link confirmed via downlink%s", m_link_check_pending ? " (LC pending)" : "");
	on_lc_success();
}

#if defined(CONFIG_SHELL)
/* Debug: inject a synthetic link-check outcome onto the radio work queue so the state
 * machine can be driven deterministically from the shell (`ats radio lc ...`)
 * without a real RF outage — including the late-LC-in-RECONNECT case (#71
 * HIGH-1), which is otherwise practically impossible to trigger on the bench.
 * The handlers themselves are state-guarded, so an injected event in
 * IDLE/JOINING/RECONNECT is ignored exactly as a real one would be. */
static bool m_dbg_lc_ok;
static struct k_work m_dbg_lc_work;

static void dbg_lc_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (m_dbg_lc_ok) {
		on_lc_success();
	} else {
		on_lc_failure();
	}
}

void app_radio_lrw_debug_inject_lc(bool ok)
{
	m_dbg_lc_ok = ok;
	k_work_submit_to_queue(app_radio_work_q(), &m_dbg_lc_work);
}
#endif /* CONFIG_SHELL */

/* #340 M22: not CONFIG_SHELL-gated (unlike the debug helpers above) - lets a
 * caller outside app_radio_lrw.c run its own work serialized with the real
 * telemetry TX path on the radio work queue, without standing up a second queue+stack of
 * its own. Originally shell-only (`ats radio compose`); calibration mode's
 * send path needs the same thing in Release builds, where CONFIG_SHELL is
 * off. Returns k_work_submit_to_queue()'s result: >=0 queued/running, a
 * negative errno if the queue rejected it. */
int app_radio_lrw_run_on_work_q(struct k_work *work)
{
	return k_work_submit_to_queue(app_radio_work_q(), work);
}

/* ======================================================================== */
/* Work handlers                                                            */
/* ======================================================================== */

static void link_check_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	on_lc_timeout();
}

static void downlink_success_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	on_downlink_received();
}

/* Deferred from downlink_callback (#219): the nanopb Info encode + queue must not
 * run on the LoRaMac/system-WQ callback stack, whose depth is not ours to assume.
 * Runs on the radio work queue like every other TX-side work item. */
static void clock_sync_info_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)app_radio_send_info((uint32_t)atomic_get(&m_clock_sync_info_seq));
}

static void lc_response_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	enum app_radio_state state = (enum app_radio_state)atomic_get(&m_state);

	/* HIGH-3: ignore a stale/late LC answer outside HEALTHY so it can never
	 * cancel the rejoin timer or mutate counters in JOINING/RECONNECT. */
	if (state != APP_RADIO_STATE_HEALTHY) {
		LOG_DBG("LC answer in %s ignored", state_name(state));
		return;
	}

	/* #340 M13: on_downlink_received() treats ANY downlink as an implicit LC
	 * confirmation and may have already resolved (and cleared) this same
	 * request via on_lc_success()/on_lc_failure() before the real LinkCheckAns
	 * got here. Without this guard the genuine-but-late answer would run the
	 * success/failure path a second time for one physical round-trip,
	 * counting it twice in the link supervision and potentially causing an
	 * unearned state transition. Mirrors the same
	 * already-resolved check in on_lc_timeout() above. */
	if (!m_link_check_pending) {
		LOG_DBG("LC answer arrived but already resolved (implicit via downlink)");
		return;
	}

	if (m_lc_response_gw_count == 0) {
		on_lc_failure();
	} else {
		on_lc_success();
	}
}

/* Commands arrive in the MAC's receive callback: dispatch them on the radio
 * work queue through the common downlink path (doc/plan/460 §2.5). */
static void dl_request_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct lrw_dl_msg msg;

	/* Drain every queued command (MED-7: was a single overwrite-able slot). */
	while (k_msgq_get(&m_dl_msgq, &msg, K_NO_WAIT) == 0) {
		app_radio_downlink(msg.buf, msg.len);
	}
}

static void join_complete_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if ((enum app_radio_state)atomic_get(&m_state) != APP_RADIO_STATE_JOINING) {
		LOG_DBG("Join complete handler: not JOINING, ignoring");
		return;
	}

	/* MAC layer still busy (join request in progress)? LoRaMac is not
	 * thread-safe, so direct calls go under the MAC lock (#241). */
	lorawan_mac_lock();
	bool mac_busy = LoRaMacIsBusy();
	lorawan_mac_unlock();

	if (mac_busy) {
		m_join_busy_polls++;
		if (m_join_busy_polls >= JOIN_BUSY_MAX_POLLS) {
			LOG_ERR("MAC busy timeout after %d ms - reconnecting",
				JOIN_BUSY_MAX_POLLS * JOIN_BUSY_POLL_INTERVAL_MS);
			on_join_failure();
			return;
		}
		if ((m_join_busy_polls % 10) == 0) {
			LOG_INF("MAC still busy (%d/%d)...", m_join_busy_polls,
				JOIN_BUSY_MAX_POLLS);
		}
		k_work_schedule_for_queue(app_radio_work_q(), &m_join_complete_work,
					  K_MSEC(JOIN_BUSY_POLL_INTERVAL_MS));
		return;
	}

	/* MAC ready - verify activation status. */
	MibRequestConfirm_t mib_req;

	mib_req.Type = MIB_NETWORK_ACTIVATION;
	lorawan_mac_lock();
	LoRaMacStatus_t mib_status = LoRaMacMibGetRequestConfirm(&mib_req);
	lorawan_mac_unlock();

	if (mib_status != LORAMAC_STATUS_OK ||
	    mib_req.Param.NetworkActivation == ACTIVATION_TYPE_NONE) {
		LOG_ERR("Join failed (not activated)");
		on_join_failure();
		return;
	}

	on_join_success();
}

/* A1 (#409): the stored lrw-region is not compiled into this image (e.g. a
 * debug.conf build that trims US915/AU915). Set once in app_radio_lrw_init() before
 * any radio bring-up; the radio then stays silent like radio-mode OFF. */
static bool m_region_unsupported;

/* Radio disabled by the radio-mode config (#271). This replaces the old
 * DevEUI/DevAddr-zero radio-silent guard (#98/#175): whether the radio comes up
 * is now an explicit user choice, not inferred from a blank identifier. OFF is
 * radio-silent (sensor/history still run); LORAWAN (default) brings the stack
 * up normally. A LORAWAN device with an all-zero DevEUI therefore attempts to
 * join and fails loudly instead of silently disabling — provisioning problems
 * surface instead of masquerading as OFF.
 *
 * radio_mode == P2P never reaches this function at all (#118): the
 * app_radio facade routes it to app_radio_p2p_init()/app_radio_p2p_join() instead of
 * app_radio_lrw_init()/app_radio_lrw_join(), so app_radio_lrw.c's own state machine never runs
 * in that mode. */
static bool radio_disabled(void)
{
	if (m_region_unsupported) {
		return true;
	}
	return g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_OFF;
}

static void join_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int ret;

	/* Radio-silent mode (#271): radio-mode is OFF (or reserved P2P) — don't burn
	 * power on join requests. Enter DISABLED and stay there until radio-mode is set
	 * back to LORAWAN + rebooted. */
	if (radio_disabled()) {
		if ((enum app_radio_state)atomic_get(&m_state) != APP_RADIO_STATE_DISABLED) {
			LOG_WRN("%s: disabled (radio-silent)",
				m_region_unsupported ? "lrw-region not in this image"
						     : "radio-mode not LORAWAN");
			state_transition(APP_RADIO_STATE_DISABLED);
		}
		return;
	}

	/* MED-10: ignore re-entry while a join is already in progress. */
	if ((enum app_radio_state)atomic_get(&m_state) == APP_RADIO_STATE_JOINING) {
		LOG_WRN("Join already in progress, ignoring request");
		return;
	}

	state_transition(APP_RADIO_STATE_JOINING); /* stops send timer, drops history */

	/* Discard any in-progress telemetry snapshot: a rejoin must not resume a
	 * pre-outage snapshot with stale sensor data and no indication (#93.5). */
	app_compose_reset();

	if (m_init_join) {
		LOG_INF("Initial join after boot");
	} else {
		LOG_INF("Rejoin attempt %d...", m_rejoin_attempts);
		LOG_INF("Deinitializing MAC...");
		lorawan_mac_lock();
		LoRaMacDeInitialization();
		lorawan_mac_unlock();

		ret = lorawan_start();
		if (ret) {
			LOG_ERR("lorawan_start failed: %d", ret);
			on_join_failure();
			return;
		}
		/* lorawan_start() re-runs LoRaMacInitialization (region-default
		 * channel masks) and restores the NVM snapshot: re-apply the
		 * configured sub-band so the join uses it. */
		ret = apply_channel_plan();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("apply_channel_plan", ret);
			on_join_failure();
			return;
		}
		LOG_INF("MAC reinitialized");
	}

	/* Configure join based on activation mode */
	static struct lorawan_join_config config;

	memset(&config, 0, sizeof(config));
	config.dev_eui = g_app_config.lrw_deveui;

	if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_OTAA) {
		LOG_INF("Using OTAA activation");
		config.mode = LORAWAN_ACT_OTAA;
		config.otaa.join_eui = g_app_config.lrw_joineui;
#if defined(CONFIG_APP_LORAWAN_1_1)
		config.otaa.nwk_key = g_app_config.lrw_nwkkey;
#else
		/* LoRaWAN 1.0.x: NwkKey == AppKey (TTN/ChirpStack/Helium). */
		config.otaa.nwk_key = g_app_config.lrw_appkey;
#endif
		config.otaa.app_key = g_app_config.lrw_appkey;
	} else if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_ABP) {
		LOG_INF("Using ABP activation");
		config.mode = LORAWAN_ACT_ABP;
		config.abp.dev_addr = sys_get_be32(g_app_config.lrw_devaddr);
		config.abp.nwk_skey = g_app_config.lrw_nwkskey;
		config.abp.app_skey = g_app_config.lrw_appskey;
	} else {
		LOG_ERR("Invalid activation mode: %d", g_app_config.lrw_activation);
		state_transition(APP_RADIO_STATE_IDLE);
		return;
	}

	m_join_busy_polls = 0;

	app_radio_count(APP_RADIO_CNT_JOIN);
	/* The join exchange ends when JOINING is left (state_transition()). */
	app_radio_air_begin();
	ret = lorawan_join(&config);
	if (ret && ret != -ETIMEDOUT) {
		LOG_ERR("Join failed: %d", ret);
		on_join_failure();
		return;
	}

	/* ABP: explicit RX delays matching the LNS (1s/2s defaults are fine). */
	if (config.mode == LORAWAN_ACT_ABP) {
		MibRequestConfirm_t mib;

		lorawan_mac_lock();
		mib.Type = MIB_RECEIVE_DELAY_1;
		mib.Param.ReceiveDelay1 = 1000;
		LoRaMacMibSetRequestConfirm(&mib);

		mib.Type = MIB_RECEIVE_DELAY_2;
		mib.Param.ReceiveDelay2 = 2000;
		LoRaMacMibSetRequestConfirm(&mib);
		lorawan_mac_unlock();

		LOG_INF("RX delays set: RX1=1s, RX2=2s");
	}

	/* Private sync word only when explicitly configured (default stays public). */
	if (g_app_config.lrw_network == APP_CONFIG_LRW_NETWORK_PRIVATE) {
		MibRequestConfirm_t mib;

		mib.Type = MIB_PUBLIC_NETWORK;
		mib.Param.EnablePublicNetwork = false;
		lorawan_mac_lock();
		LoRaMacMibSetRequestConfirm(&mib);
		lorawan_mac_unlock();
		LOG_INF("Network type: private (sync word 0x12)");
	}

	LOG_INF("lorawan_join() ret=%d, polling MAC...", ret);
	k_work_schedule_for_queue(app_radio_work_q(), &m_join_complete_work,
				  K_MSEC(JOIN_BUSY_POLL_INTERVAL_MS));
}

/* ======================================================================== */
/* TX backend: app_radio schedules, this sends one frame (doc/plan/460 F4)  */
/* ======================================================================== */

/* struct app_radio_backend.send. Radio work queue only. */
static int lrw_tx_send(const struct app_radio_frame *f, struct app_radio_tx_result *res)
{
	uint8_t budget = refresh_payload_budget(); /* MED-6: per-TX budget, not cached */
	uint8_t port;
	int ret;

	switch (f->kind) {
	case APP_RADIO_FRAME_TELEMETRY:
		port = APP_RADIO_LRW_TELEMETRY_PORT;
		break;
	case APP_RADIO_FRAME_ALARM:
		port = APP_RADIO_LRW_ALARM_PORT;
		break;
	default:
		port = f->port ? f->port : APP_RADIO_LRW_DOWNLINK_CMD_PORT;
		break;
	}

	res->wait_ms = FRAME_RETRY_SEC * MSEC_PER_SEC;
	res->budget = budget;

	if (budget == 0 || f->len == 0) {
		/* H-1 / #409 3a: pending MAC answers (an ADR / channel batch from the
		 * LNS) fill the whole frame, so nothing fits. Waiting deadlocks: no
		 * uplink -> the MAC answers never leave -> the budget stays 0 and the
		 * node goes mute. Send an empty uplink so LoRaMac drains them (in
		 * FOpts, or on port 0 when they overflow); the frame waits for the
		 * budget to come back. */
		LOG_WRN("TX budget 0 (MAC-command flood, port %u): empty uplink to flush MAC",
			port);
		ret = lrw_send(port, f->buf, 0, LORAWAN_MSG_UNCONFIRMED);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lorawan_send (MAC flush)", ret);
		}
		return -EAGAIN;
	}
	if (f->len > budget) {
		/* Zephyr's lorawan_send would send an empty frame and drop the
		 * payload; app_radio recovers the frame by its kind instead. */
		return -EMSGSIZE;
	}

	bool with_link_check = (f->flags & APP_RADIO_FRAME_LINK_CHECK) != 0;

	if (with_link_check) {
		ret = lorawan_request_link_check(false);
		if (ret) {
			LOG_ERR("Link check request failed: %d", ret);
			with_link_check = false;
		} else {
			m_link_check_pending = true;
		}
	}

	if (f->kind == APP_RADIO_FRAME_TELEMETRY) {
		struct app_radio_link link;

		app_radio_get_link(&link);
		LOG_INF("Sending data (msg #%u, %s)...", link.reports + 1,
			(f->flags & APP_RADIO_FRAME_MORE) ? "more pending" : "last frame");
	}

	ret = lrw_send(port, f->buf, (uint8_t)f->len,
		       (f->flags & APP_RADIO_FRAME_CONFIRMED) ? LORAWAN_MSG_CONFIRMED
							      : LORAWAN_MSG_UNCONFIRMED);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lorawan_send", ret);
		if (with_link_check) {
			m_link_check_pending = false;
		}
		switch (ret) {
		case -EMSGSIZE:
			/* The DR fell between the budget query and the send. */
			res->budget = refresh_payload_budget();
			return -EMSGSIZE;
		case -ECONNREFUSED:
			/* Refused by the duty cycle (F29): the MAC is throttled, the
			 * frame is not failing. */
			return -EAGAIN;
		default:
			/* MAC busy, no free channel, confirm timeout: retried after
			 * the backoff, a bounded number of times (#219). */
			return -EIO;
		}
	}

	/* Start the LC timeout only now: lorawan_send() returns after the RX windows
	 * closed, so a LinkCheckAns has already been handed to the radio work queue. Started
	 * before the send, it also had to cover the airtime + RX1/RX2 delays, which
	 * at DR0/SF12 with a 5 s RX1 delay is ~9-10 s — a race against the 10 s
	 * timeout exactly on the bottom rung of the recovery ladder. */
	if (with_link_check) {
		k_timer_start(&m_lc_timeout_timer, K_SECONDS(LINK_CHECK_TIMEOUT_SEC), K_FOREVER);
	}
	if (f->kind != APP_RADIO_FRAME_TELEMETRY) {
		LOG_INF("Sent on port %u (%u B)", port, f->len);
	}
	return 0;
}

static uint8_t lrw_tx_budget(void)
{
	return m_mac_started ? refresh_payload_budget() : 0;
}

/* Once per report, at its first frame: a due link check rides a LinkCheckReq,
 * unless one is still unanswered. */
static uint8_t lrw_tx_report_flags(bool due)
{
	return (due && !m_link_check_pending) ? APP_RADIO_FRAME_LINK_CHECK : 0;
}

/* The WARNING budget ran out (or M-2 found the node mute): rejoin with
 * backoff. ABP cannot rejoin over the air: it stays in WARNING and keeps
 * checking the link every report; M-2 still re-activates it (`forced`). */
static int lrw_tx_rejoin(bool forced)
{
	if (!forced && g_app_config.lrw_activation != APP_CONFIG_LRW_ACTIVATION_OTAA) {
		LOG_WRN("ABP mode - cannot rejoin");
		return -ENOTSUP;
	}
	state_transition(APP_RADIO_STATE_RECONNECT);
	return 0;
}

/* MED-9: a history replay owns the radio; telemetry waits. */
static bool lrw_tx_replay_active(void)
{
	return m_hist_active;
}

const struct app_radio_backend app_radio_lrw_backend = {
	.send = lrw_tx_send,
	.budget = lrw_tx_budget,
	.tx_ready = app_radio_lrw_is_ready,
	.report_flags = lrw_tx_report_flags,
	.replay_active = lrw_tx_replay_active,
	.get_state = app_radio_lrw_get_state,
	.warning_step = lrw_backoff_step,
	.rejoin = lrw_tx_rejoin,
	.in_flight = NULL,  /* lorawan_send() blocks for the whole confirmed exchange */
	.confirm_kinds = 0, /* unconfirmed: the link check is the liveness probe */
	.cmd_transport = APP_CMD_TRANSPORT_LRW,
	.frame_gap_ms = FRAME_GAP_SEC * MSEC_PER_SEC,
};

/* ======================================================================== */
/* History replay                                                           */
/* ======================================================================== */

/* Max samples that fit one frame at the current DR. Uses the exact protobuf
 * envelope overhead (app_cmd_history_sample_capacity) instead of a fixed guess
 * that overflowed m_hist_tx_buf on DR3+ with a synced RTC (#89). frame_index /
 * frame_count are sized with m_hist_frame_bound and t0 with the max varint, so
 * the cap is a stable per-replay lower bound. */
static size_t history_frame_cap(void)
{
	size_t out_cap = MIN((size_t)refresh_payload_budget(), sizeof(m_hist_tx_buf)); /* MED-6 */

	return app_cmd_history_sample_capacity(m_hist_seq, m_hist_frame_bound, m_hist_frame_bound,
					       UINT32_MAX, m_hist_present, m_hist_interval,
					       out_cap);
}

static void history_replay_finish(void)
{
	m_hist_active = false;
	app_history_set_replay_active(false);
	/* Hand the report cadence back to app_report with an immediate uplink. */
	fire_ready_cb();
}

static void m_hist_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!m_hist_active) {
		return;
	}

	enum app_radio_state state = (enum app_radio_state)atomic_get(&m_state);

	if (state == APP_RADIO_STATE_JOINING || state == APP_RADIO_STATE_RECONNECT) {
		LOG_WRN("History replay aborted: %s", state_name(state));
		m_hist_active = false;
		app_history_set_replay_active(false);
		return; /* the (re)join → HEALTHY entry / send path restarts cadence */
	}

	/* A DR drop mid-replay packs fewer records per frame, so frame_index can
	 * outgrow the bound the cap was sized with: fall back to the worst case. */
	if (m_hist_idx >= m_hist_frame_bound) {
		m_hist_frame_bound = UINT32_MAX;
	}

	uint8_t samples[HISTORY_SAMPLES_MAX];
	size_t cap = MIN(history_frame_cap(), sizeof(samples));
	uint32_t t0 = 0;
	bool synced = false;
	uint16_t n = 0;
	uint32_t next = m_hist_cursor;
	size_t slen = 0;

	if (cap > 0) {
		slen = app_history_export_abs(m_hist_from, m_hist_to, m_hist_cursor, m_hist_end,
					      samples, cap, &t0, &synced, &n, &next);
	}
	if (n == 0) {
		if (next >= m_hist_end) {
			/* Nothing left in the window: the records were evicted or the
			 * ring was reset since the previous frame. */
			LOG_INF("History replay complete: %u frames", (unsigned)m_hist_idx);
			history_replay_finish();
			return;
		}
		/* #409 3f: records remain but the DR dropped below one record per
		 * frame. Tell the host instead of going silent mid-stream. */
		LOG_WRN("History replay stop at frame %u/%u (cap=%uB)", (unsigned)m_hist_idx,
			(unsigned)m_hist_count, (unsigned)cap);
		uint8_t err[16];
		size_t err_len;

		if (app_cmd_build_budget_error(m_hist_seq, err, refresh_payload_cap(sizeof(err)),
					       &err_len) == 0) {
			(void)app_radio_tx_queue(APP_RADIO_FRAME_ANSWER, APP_RADIO_TAG_OTHER,
						 APP_RADIO_LRW_DOWNLINK_CMD_PORT, err, err_len);
		}
		history_replay_finish();
		return;
	}

	size_t len;
	/* time_synced is per frame: a frame never spans two history segments, and
	 * each segment (flash page) knows whether its base is unix or uptime. */
	int ret = app_cmd_build_history_frame(m_hist_seq, m_hist_idx, m_hist_count, t0,
					      m_hist_present, m_hist_interval, synced, samples,
					      slen, m_hist_tx_buf, sizeof(m_hist_tx_buf), &len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_cmd_build_history_frame", ret);
		history_replay_finish();
		return;
	}

	ret = lrw_send(APP_RADIO_LRW_DOWNLINK_CMD_PORT, m_hist_tx_buf, len,
		       LORAWAN_MSG_UNCONFIRMED);
	if (ret) {
		/* Duty-cycle / MAC busy — retry the same frame, don't advance. Bounded
		 * so a persistently rejected frame cannot wedge the replay (and the
		 * paused telemetry timer) forever (#89). */
		LOG_ERR_CALL_FAILED_INT("lorawan_send(history)", ret);
		if (++m_hist_retries > HISTORY_MAX_RETRIES) {
			LOG_ERR("History frame %u/%u abandoned after %d retries",
				(unsigned)m_hist_idx, (unsigned)m_hist_count, m_hist_retries - 1);
			history_replay_finish();
			return;
		}
		app_radio_count(APP_RADIO_CNT_RETRY);
		k_work_schedule_for_queue(app_radio_work_q(), &m_hist_work,
					  K_SECONDS(FRAME_RETRY_SEC));
		return;
	}
	m_hist_retries = 0;
	/* M-2: a history frame on air proves the channel is alive just as a telemetry
	 * frame does. Without this, a long replay (which pauses telemetry) can trip the
	 * stale-uplink watchdog and rejoin a perfectly healthy session mid-replay. */
	app_radio_note_uplink();

	LOG_INF("History frame %u/%u sent (%u rec, %zu B)", (unsigned)(m_hist_idx + 1),
		(unsigned)m_hist_count, (unsigned)n, len);
	m_hist_cursor = next;
	m_hist_idx++;

	/* Terminate on cursor exhaustion, not frame_index == frame_count (#89): a DR
	 * change mid-replay alters records-per-frame, so the up-front frame_count is
	 * only an estimate. The host concatenates by frame_index. The export already
	 * skips to the next record in the window, so the frame carrying the window's
	 * last record ends the replay here — no trailing empty attempt (H-4). */
	if (m_hist_cursor < m_hist_end) {
		k_work_schedule_for_queue(app_radio_work_q(), &m_hist_work,
					  K_SECONDS(FRAME_GAP_SEC));
	} else {
		LOG_INF("History replay complete: %u frames", (unsigned)m_hist_idx);
		history_replay_finish();
	}
}

int app_radio_lrw_history_replay_start(uint32_t from_unix, uint32_t to_unix, uint32_t seq)
{
	if (!app_radio_lrw_is_ready()) {
		LOG_WRN("History replay requested but LRW not ready; ignoring");
		return -EAGAIN;
	}

	/* Seed the snapshot fields the cap depends on (seq/present/interval) before
	 * sizing a frame, so counting and sending use an identical per-frame cap. */
	m_hist_from = from_unix;
	m_hist_to = to_unix;
	m_hist_seq = seq;
	m_hist_present = app_history_get_mask();
	m_hist_interval = app_history_get_interval();

	/* #409 3f: size with the worst-case frame_index/count first, then tighten
	 * the bound to that frame count and recount. A bigger cap never needs more
	 * frames, so the final count stays within the bound and counting and
	 * sending keep using one identical per-frame cap. */
	m_hist_frame_bound = UINT32_MAX;
	size_t cap = history_frame_cap();
	uint32_t n = (cap > 0) ? app_history_count_frames(from_unix, to_unix, cap) : 0;

	if (n > 0) {
		m_hist_frame_bound = n;
		n = app_history_count_frames(from_unix, to_unix, history_frame_cap());
	}

	if (n == 0) {
		/* Empty window, or records exist but not one fits the current DR (the
		 * 11 B budget tier)? Probe with the full frame buffer to tell apart. */
		if (app_history_count_frames(from_unix, to_unix, sizeof(m_hist_tx_buf)) > 0) {
			LOG_WRN("History replay: DR budget too small for one record");
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
	 * off. app_report telemetry self-skips while the replay owns the radio. */
	app_history_set_replay_active(true);

	LOG_INF("History replay start: %u frames (window %u..%u)", (unsigned)n, from_unix, to_unix);
	k_work_schedule_for_queue(app_radio_work_q(), &m_hist_work, K_NO_WAIT);
	return 0;
}

/* ======================================================================== */
/* Timer ISR handlers (thin: enqueue the right work, no state decisions)    */
/* ======================================================================== */

static void lc_timeout_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit_to_queue(app_radio_work_q(), &m_link_check_work);
}

static void rejoin_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit_to_queue(app_radio_work_q(), &m_join_work);
}

/* ======================================================================== */
/* LoRaMac callbacks (other contexts: copy + enqueue only)                  */
/* ======================================================================== */

static void downlink_callback(uint8_t port, uint8_t flags, int16_t rssi, int8_t snr, uint8_t len,
			      const uint8_t *data)
{
	LOG_INF("Port %d, Flags 0x%02x, RSSI %d dB, SNR %d dBm", port, flags, rssi, snr);

	m_last_rssi = rssi;
	m_last_snr = snr;
	app_radio_note_downlink(rssi, snr);

	if (data) {
		LOG_HEXDUMP_INF(data, len, "Payload: ");
	}

	/* Set the RTC from the network if this downlink carried a DeviceTimeAns. */
	app_clock_handle_downlink(flags);

	/* Deferred answer to a ClockSync command: once the network time actually
	 * lands, send an Info uplink carrying the freshly-synced unix_time (the
	 * command itself does not ack — saves an uplink, and a bare ack couldn't
	 * carry the synced time yet). The Info encode is pushed to the radio work queue so it
	 * never runs on the callback stack (#219). */
	if ((flags & LORAWAN_TIME_UPDATED) &&
	    atomic_test_and_clear_bit(&m_clock_sync_info_pending, 0)) {
		k_work_submit_to_queue(app_radio_work_q(), &m_clock_sync_info_work);
	}

	if (port == APP_RADIO_LRW_DOWNLINK_CMD_PORT && data && len > 0) {
		if (len <= APP_RADIO_LRW_REQUEST_BUF_SIZE) {
			struct lrw_dl_msg msg;

			msg.len = len;
			memcpy(msg.buf, data, len);
			/* MED-7: queue (don't clobber a single slot). */
			if (k_msgq_put(&m_dl_msgq, &msg, K_NO_WAIT) != 0) {
				LOG_WRN("Downlink command queue full; dropping");
			} else {
				k_work_submit_to_queue(app_radio_work_q(), &m_dl_request_work);
			}
		} else {
			LOG_WRN("Port %u payload too large: %u B (max %d)", port, len,
				APP_RADIO_LRW_REQUEST_BUF_SIZE);
		}
	}

	k_work_submit_to_queue(app_radio_work_q(), &m_downlink_success_work);
}

/* Approximate operational cell window for the DevStatusAns battery level.
 * Tune to the actual cell; values outside are clamped. */
#define BATTERY_EMPTY_V 2.4f
#define BATTERY_FULL_V  3.6f

static uint8_t battery_level_callback(void)
{
	/* LoRaWAN DevStatusAns battery level: 0 = external power, 1..254 = battery
	 * (1 ~ empty, 254 ~ full), 255 = unable to measure. Map the measured cell
	 * voltage linearly over the operational window.
	 *
	 * #340 M9: read the last-measured voltage instead of calling
	 * app_battery_measure() directly. This runs on LoRaMacProcess()'s context
	 * (the system workqueue, via the radio driver's DIO IRQ work item) and a
	 * raw, non-refcounted RESUME/adc_read/SUSPEND racing the sensor thread's
	 * own concurrent app_battery_measure() call can wedge the ADC and hang
	 * that workqueue forever (IWDG reset). */
	float v = app_battery_last_sample();

	if (!isfinite(v) || v <= 0.0f) {
		return 255; /* no sample yet / last measurement failed */
	}

	float frac = CLAMP((v - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V), 0.0f, 1.0f);

	return (uint8_t)(1 + (int)(frac * 253.0f + 0.5f)); /* 1..254 */
}

static void datarate_changed_callback(enum lorawan_datarate dr)
{
	uint8_t max_next = 0, max_now = 0;

	lorawan_get_payload_sizes(&max_next, &max_now);
	m_max_next_payload = max_next;
	LOG_INF("New data rate: DR%d, Maximum payload size: %d", dr, max_now);

	/* #409: a higher DR may now fit the deferred full Info / settings-info. */
	app_radio_announce_kick();
}

static void link_check_callback(uint8_t demod_margin, uint8_t nb_gateways)
{
	LOG_INF("Link check response: margin=%d dB, gateways=%d", demod_margin, nb_gateways);

	m_last_margin = demod_margin;
	m_last_gw_count = nb_gateways;
	app_radio_set_uplink_margin(demod_margin, nb_gateways);
	m_lc_response_gw_count = nb_gateways;

	k_work_submit_to_queue(app_radio_work_q(), &m_lc_response_work);
}

/* ======================================================================== */
/* Region / NVM helpers                                                     */
/* ======================================================================== */

static void clear_stale_lorawan_nvm(void)
{
	static const char *const keys[] = {
		"lorawan/nvm/MacGroup2",
		"lorawan/nvm/RegionGroup1",
		"lorawan/nvm/RegionGroup2",
		"lorawan/nvm/ClassB",
	};

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		int ret = settings_delete(keys[i]);

		if (ret && ret != -ENOENT) {
			LOG_ERR("Call `settings_delete` failed (%s): %d", keys[i], ret);
		}
	}
}

static int apply_subband(int sub_band)
{
	if (sub_band == 0) {
		return 0;
	}
	if (sub_band < 1 || sub_band > 8) {
		LOG_ERR("Invalid sub-band: %d", sub_band);
		return -EINVAL;
	}

	uint16_t mask[6] = {0};
	uint8_t base = (sub_band - 1) * 8;

	for (uint8_t ch = base; ch < base + 8; ch++) {
		mask[ch / 16] |= BIT(ch % 16);
	}
	uint8_t hi_ch = 64 + (sub_band - 1);

	mask[hi_ch / 16] |= BIT(hi_ch % 16);

	/* Install the sub-band as the DEFAULT mask too, not only the active one.
	 * LoRaMac copies ChannelsDefaultMask over ChannelsMask on every OTAA join
	 * (ResetMacParameters) and when its ADR backoff reaches the minimum DR. With
	 * only the active mask set, the default stayed all-64: once the sub-band's
	 * 8 channels were used up, the remaining-channel pool refilled with all 64,
	 * and join requests cycled over all eight sub-bands — one in eight reaching
	 * an 8-channel gateway. Set before the active mask, whose setter trims the
	 * default's 500 kHz word. */
	MibRequestConfirm_t mib;

	mib.Type = MIB_CHANNELS_DEFAULT_MASK;
	mib.Param.ChannelsDefaultMask = mask;
	lorawan_mac_lock();
	LoRaMacStatus_t status = LoRaMacMibSetRequestConfirm(&mib);
	lorawan_mac_unlock();

	if (status != LORAMAC_STATUS_OK) {
		LOG_ERR("Default channel mask rejected (sub-band %d): %d", sub_band, status);
		return -EINVAL;
	}

	int ret = lorawan_set_channels_mask(mask, ARRAY_SIZE(mask));

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lorawan_set_channels_mask", ret);
		return ret;
	}

	LOG_INF("Applied sub-band %d", sub_band);
	return 0;
}

/* Region-specific channel plan on top of the LoRaMac defaults. Called after
 * every lorawan_start() (boot and each rejoin's MAC re-init). */
static int apply_channel_plan(void)
{
	if (g_app_config.lrw_region != APP_CONFIG_LRW_REGION_US915 &&
	    g_app_config.lrw_region != APP_CONFIG_LRW_REGION_AU915) {
		return 0;
	}
	return apply_subband(g_app_config.lrw_sub_band);
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

void app_radio_lrw_suspend(void)
{
	/* Stop every LRW timer so nothing re-arms the radio after this point; the
	 * caller is about to power the MCU off (deep sleep). Pending works on
	 * the radio work queue are simply abandoned — they cannot run once the system is shut
	 * down, and wake is a clean boot. (The report-cadence timer lives in
	 * app_report now; app_power_suspend stops it via app_report_suspend.) */
	k_timer_stop(&m_lc_timeout_timer);
	k_timer_stop(&m_rejoin_timer);
}

/* Map the stored lrw-region to a Zephyr region, but only if that region is
 * compiled into this image (A1, #409). lorawan_set_region() returns -ENOTSUP for
 * a region whose CONFIG_LORAMAC_REGION_* is off; resolving it here lets the
 * caller go radio-silent instead of failing the whole LoRaWAN init. There is
 * deliberately no fallback to another region: a device configured for US915 or
 * AU915 must never transmit on 868 MHz (or vice versa). */
static int resolve_region(enum lorawan_region *region)
{
	switch (g_app_config.lrw_region) {
	case APP_CONFIG_LRW_REGION_EU868:
		if (IS_ENABLED(CONFIG_LORAMAC_REGION_EU868)) {
			*region = LORAWAN_REGION_EU868;
			return 0;
		}
		break;
	case APP_CONFIG_LRW_REGION_US915:
		if (IS_ENABLED(CONFIG_LORAMAC_REGION_US915)) {
			*region = LORAWAN_REGION_US915;
			return 0;
		}
		break;
	case APP_CONFIG_LRW_REGION_AU915:
		if (IS_ENABLED(CONFIG_LORAMAC_REGION_AU915)) {
			*region = LORAWAN_REGION_AU915;
			return 0;
		}
		break;
	case APP_CONFIG_LRW_REGION_AS923:
		/* #409 A6: channel plan AS923-1 (loramac-node default); no sub-band. */
		if (IS_ENABLED(CONFIG_LORAMAC_REGION_AS923)) {
			*region = LORAWAN_REGION_AS923;
			return 0;
		}
		break;
	default:
		LOG_ERR("Invalid lrw-region: %d", g_app_config.lrw_region);
		return -EINVAL;
	}

	return -ENOTSUP;
}

int app_radio_lrw_init(void)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(lora0));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	/* #271: when radio-mode is OFF (or reserved P2P) skip the entire LoRaMac/radio
	 * bring-up. The work queue, works and timers below are still set up so the
	 * public API stays safe (app_radio_lrw_join / send hit the DISABLED guard and
	 * no-op), but clear_stale_lorawan_nvm() / lorawan_set_region() /
	 * lorawan_start() are never called — the SubGHz radio is never powered, so
	 * there is no boot radio burst. (Replaces the #98/#175 DevEUI-zero guard: the
	 * radio is now enabled/disabled explicitly, not inferred from a blank ID.) */
	enum lorawan_region region = LORAWAN_REGION_EU868;

	/* A1 (#409): a stored region missing from this image (or an out-of-range
	 * value) used to fail the whole init, leaving a device with no radio and no
	 * diagnosable state. Go radio-silent instead: DISABLED (lrw_state, and the
	 * lrw_disabled device_status bit) + a loud log; fix by setting a
	 * compiled-in lrw-region or reflashing. */
	bool radio_silent = radio_disabled();

	if (!radio_silent && resolve_region(&region) != 0) {
		LOG_ERR("lrw-region %d is not compiled into this image: radio-silent "
			"(set a supported lrw-region or flash a full image)",
			g_app_config.lrw_region);
		m_region_unsupported = true;
		radio_silent = true;
	}

	if (!radio_silent) {
		clear_stale_lorawan_nvm();

		ret = lorawan_set_region(region);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lorawan_set_region", ret);
			return ret;
		}

		ret = lorawan_start();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lorawan_start", ret);
			return ret;
		}
		m_mac_started = true;

		ret = apply_channel_plan();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("apply_channel_plan", ret);
			return ret;
		}

		static struct lorawan_downlink_cb downlink_cb = {
			.port = LW_RECV_PORT_ANY,
			.cb = downlink_callback,
		};

		lorawan_register_downlink_callback(&downlink_cb);
		lorawan_register_battery_level_callback(battery_level_callback);
		lorawan_register_dr_changed_callback(datarate_changed_callback);
		lorawan_register_link_check_ans_callback(link_check_callback);
	} else if (!m_region_unsupported) {
		LOG_WRN("radio-mode not LORAWAN: skipping LoRaWAN bring-up (radio-silent, #271)");
	}

	k_work_init(&m_join_work, join_work_handler);
	k_work_init_delayable(&m_hist_work, m_hist_work_handler);
	k_work_init(&m_link_check_work, link_check_work_handler);
	k_work_init(&m_downlink_success_work, downlink_success_work_handler);
	k_work_init(&m_clock_sync_info_work, clock_sync_info_work_handler);
	k_work_init(&m_lc_response_work, lc_response_work_handler);
	k_work_init(&m_dl_request_work, dl_request_work_handler);
	k_work_init_delayable(&m_join_complete_work, join_complete_work_handler);
#if defined(CONFIG_SHELL)
	k_work_init(&m_dbg_lc_work, dbg_lc_work_handler);
#endif

	k_timer_init(&m_lc_timeout_timer, lc_timeout_timer_handler, NULL);
	k_timer_init(&m_rejoin_timer, rejoin_timer_handler, NULL);

	/* The radio work queue's liveness heartbeat and the M-2 watchdog (#182). */
	app_radio_heartbeat_start();

	atomic_set(&m_state, radio_silent ? APP_RADIO_STATE_DISABLED : APP_RADIO_STATE_IDLE);
	m_init_join = true;

	return 0;
}

void app_radio_lrw_join(void)
{
	k_work_submit_to_queue(app_radio_work_q(), &m_join_work);
}

void app_radio_lrw_register_ready_cb(void (*cb)(void))
{
	m_ready_cb = cb;
}

enum app_radio_state app_radio_lrw_get_state(void)
{
	return (enum app_radio_state)atomic_get(&m_state);
}

/* Uplink SF of LoRaWAN data rate `dr` in the configured region (LoRa DRs only;
 * 0 for an FSK or unknown DR). Pure -- the regional DR tables of RP002. */
static uint8_t region_dr_sf(int dr)
{
	switch (g_app_config.lrw_region) {
	case APP_CONFIG_LRW_REGION_US915:
		/* DR0..DR3 = SF10..SF7 BW125, DR4 = SF8 BW500 */
		return dr >= 0 && dr <= 3 ? (uint8_t)(10 - dr) : (dr == 4 ? 8 : 0);
	case APP_CONFIG_LRW_REGION_AU915:
		/* DR0..DR5 = SF12..SF7 BW125, DR6 = SF8 BW500 */
		return dr >= 0 && dr <= 5 ? (uint8_t)(12 - dr) : (dr == 6 ? 8 : 0);
	default:
		/* EU868 / AS923: DR0..DR5 = SF12..SF7 BW125, DR6 = SF7 BW250, DR7 FSK */
		return dr >= 0 && dr <= 5 ? (uint8_t)(12 - dr) : (dr == 6 ? 7 : 0);
	}
}

/* Conducted TX power (dBm) LoRaMac derives from TXPower index `idx`:
 * floor(max EIRP - 2 * idx - antenna gain), the region defaults of
 * RegionCommonComputeTxPower() (US915 uses max ERP 30 without antenna gain).
 * Integer centi-dB, no libm. */
static int8_t region_tx_power_dbm(int idx)
{
	int max_cdb = 1600;
	int gain_cdb = 215;

	switch (g_app_config.lrw_region) {
	case APP_CONFIG_LRW_REGION_US915:
		max_cdb = 3000;
		gain_cdb = 0;
		break;
	case APP_CONFIG_LRW_REGION_AU915:
		max_cdb = 3000;
		break;
	default:
		break;
	}

	int v = max_cdb - 200 * idx - gain_cdb;

	return (int8_t)(v >= 0 ? v / 100 : -((-v + 99) / 100));
}

/* Push the MAC's radio parameters and session to app_radio (RadioState, #446):
 * DR / SF / TX power as the next uplink uses them, DevAddr and FCntUp. Called
 * on the radio work queue after every sent uplink and on entering HEALTHY, so the snapshot
 * follows ADR and the ladder. */
static void publish_mac(void)
{
	MibRequestConfirm_t mib;
	uint32_t fcnt_up;

	/* LoRaMac's MIB is only valid once lorawan_start() has run. */
	if (!m_mac_started) {
		return;
	}

	lorawan_mac_lock();
	mib.Type = MIB_CHANNELS_DATARATE;
	if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK) {
		int dr = mib.Param.ChannelsDatarate;

		mib.Type = MIB_CHANNELS_TX_POWER;
		if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK) {
			app_radio_set_params(region_dr_sf(dr), dr,
					     region_tx_power_dbm(mib.Param.ChannelsTxPower));
		}
	}
	mib.Type = MIB_DEV_ADDR;
	if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK && mib.Param.DevAddr != 0 &&
	    LoRaMacCryptoGetFCntUp(&fcnt_up) == LORAMAC_CRYPTO_SUCCESS) {
		app_radio_set_session(mib.Param.DevAddr, fcnt_up);
	}
	lorawan_mac_unlock();
}

bool app_radio_lrw_is_ready(void)
{
	enum app_radio_state state = (enum app_radio_state)atomic_get(&m_state);

	return state == APP_RADIO_STATE_HEALTHY;
}

int app_radio_lrw_get_info(struct app_radio_lrw_info *info)
{
	MibRequestConfirm_t mib_req;

	if (!info) {
		return -EINVAL;
	}

	info->state = app_radio_get_state(); /* with the common WARNING */

	/* #340 L3: radio-mode OFF/P2P (#271) never calls lorawan_start(), so
	 * LoRaMac's own state (incl. CryptoNvm) was never initialized -- querying
	 * it here would deref a NULL CryptoNvm. Zero-fill instead of touching
	 * LoRaMac's MIB/crypto API when the MAC was never started. The same holds
	 * for the boot window before app_radio_lrw_init() has run lorawan_start(): the
	 * state is already IDLE then (HW-seen: shell showed FCntUp 0x080232D6). */
	if (info->state == APP_RADIO_STATE_DISABLED || !m_mac_started) {
		info->dev_addr = 0;
		info->fcnt_up = 0;
		info->datarate = 0;
		info->tx_power = 0;
	} else {
		uint32_t fcnt_up;

		/* Called from shell/NFC/the radio work queue: direct LoRaMac access goes under
		 * the MAC lock (#241). */
		lorawan_mac_lock();
		mib_req.Type = MIB_DEV_ADDR;
		if (LoRaMacMibGetRequestConfirm(&mib_req) == LORAMAC_STATUS_OK) {
			info->dev_addr = mib_req.Param.DevAddr;
		} else {
			info->dev_addr = 0;
		}

		if (LoRaMacCryptoGetFCntUp(&fcnt_up) == LORAMAC_CRYPTO_SUCCESS) {
			info->fcnt_up = fcnt_up;
		} else {
			info->fcnt_up = 0;
		}

		/* Live DR from the MAC, not a copy cached in the DR-changed callback:
		 * Zephyr only fires that callback with ADR on (or on join), so an
		 * ADR-off lorawan_set_datarate() (manual DR, recovery ladder) left
		 * the reported DR stale. */
		mib_req.Type = MIB_CHANNELS_DATARATE;
		if (LoRaMacMibGetRequestConfirm(&mib_req) == LORAMAC_STATUS_OK) {
			info->datarate = mib_req.Param.ChannelsDatarate;
		} else {
			info->datarate = 0;
		}

		mib_req.Type = MIB_CHANNELS_TX_POWER;
		if (LoRaMacMibGetRequestConfirm(&mib_req) == LORAMAC_STATUS_OK) {
			info->tx_power = mib_req.Param.ChannelsTxPower;
		} else {
			info->tx_power = 0;
		}
		lorawan_mac_unlock();
	}

	info->rssi = m_last_rssi;
	info->snr = m_last_snr;
	info->margin = m_last_margin;
	info->gw_count = m_last_gw_count;

	struct app_radio_link link;

	app_radio_get_link(&link);
	info->consecutive_lc_fail = (int)link.fail_streak;
	info->warning_lc_fail_total = (int)link.warning_fails;
	info->message_count = (int)link.reports;
	info->thresh_warning = APP_RADIO_LINK_WARNING_THRESHOLD;
	info->thresh_reconnect = g_app_config.radio_link_check_fail_rejoin;
	info->link_check_interval = g_app_config.radio_link_check_interval;

	return 0;
}

void app_radio_lrw_send_info_on_clock_sync(uint32_t seq)
{
	/* Arm the deferred Info; downlink_callback sends it once LORAWAN_TIME_UPDATED
	 * arrives (the ClockSync command answer). The seq is stored before the bit is
	 * set, so the Info work item never pairs a new request with a stale seq. */
	atomic_set(&m_clock_sync_info_seq, (atomic_val_t)seq);
	atomic_set_bit(&m_clock_sync_info_pending, 0);
}

void app_radio_lrw_clock_sync(uint32_t seq)
{
	app_clock_force_resync();
	app_radio_lrw_send_info_on_clock_sync(seq);
}

int app_radio_lrw_reset_nvm(void)
{
	static const char *const keys[] = {
		"lorawan/nvm/Crypto",        "lorawan/nvm/MacGroup1",    "lorawan/nvm/MacGroup2",
		"lorawan/nvm/SecureElement", "lorawan/nvm/RegionGroup1", "lorawan/nvm/RegionGroup2",
		"lorawan/nvm/ClassB",
	};
	int ret = 0;

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		int err = settings_delete(keys[i]);

		if (err) {
			LOG_WRN("settings_delete(%s) failed: %d", keys[i], err);
			ret = err;
		}
	}

	LOG_INF("LoRaWAN NVM cleared (frame counters + DevNonce); reboot required");
	return ret;
}
