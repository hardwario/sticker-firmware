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
#include "app_counters.h"
#include "app_history.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_settings.h"
#include "app_wdog.h"

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
#include <zephyr/sys/reboot.h>

/* LoRaMac includes */
#include <LoRaMac.h>
#include <LoRaMacCrypto.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_lrw, LOG_LEVEL_DBG);

/*
 * State-machine refactor (#71).
 *
 * Concurrency model: ALL state and counters are mutated only on m_work_q
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
 * The periodic report cadence lives in app_report (#126): it samples, captures
 * history and triggers app_lrw_send_telemetry(). app_lrw stays transport — it
 * composes the snapshot (app_compose), splits it into DR-budget frames, sends
 * them with the LinkCheckReq piggyback + duty-cycle retry, drains the
 * response/alarm queues and streams a history replay. On a link-ready edge (join
 * success / replay finish) app_lrw kicks app_report via the registered callback.
 */

/* Link check configuration constants.
 * The LC cadence (every N-th uplink) and the failures-before-rejoin threshold
 * are runtime-configurable: g_app_config.lrw_link_check_interval and
 * lrw_link_check_fail_rejoin (config keys lrw-link-check-interval /
 * lrw-link-check-fail-rejoin). */
#define LINK_CHECK_TIMEOUT_SEC 10 /* Timeout for response */

/* State machine thresholds  */
#define FAIL_THRESHOLD_WARNING 3 /* LC failures to enter WARNING */
#define OK_THRESHOLD_HEALTHY   1 /* LC successes in WARNING to return to HEALTHY */

/* Join/Rejoin backoff configuration - easily adjustable */
#define REJOIN_BACKOFF_BASE_SEC   60   /* Base backoff time in seconds */
#define REJOIN_BACKOFF_MAX_SEC    3600 /* Maximum backoff time (1 hour) */
#define REJOIN_BACKOFF_MULTIPLIER 2    /* Exponential multiplier per attempt */

/* 4096 B (was 2048): the TX path nests lorawan_send → LoRaMac → nanopb encode →
 * mbedTLS AES-CCM, which is deep, and release builds carry no stack canary
 * (CONFIG_INIT_STACKS is debug-only) so an overflow would silently corrupt RAM
 * and mimic the TX-stop wedge rather than fault cleanly (#187). */
static K_THREAD_STACK_DEFINE(m_work_stack, 4096);
static struct k_work_q m_work_q;

#if defined(CONFIG_WATCHDOG)
/* Liveness heartbeat (#182): a self-rearming work item proves m_work_q is still
 * draining. If the queue wedges (e.g. lorawan_send blocks forever on the MAC
 * confirm semaphore, #181) this work can no longer run, the channel goes stale
 * and app_wdog stops feeding the IWDG → SoC reset + rejoin. The timeout is far
 * above the worst-case legitimate single-send blocking (~7 s on TTN with a 5 s
 * RX1 delay), so only a true wedge trips it. */
#define LRW_HEARTBEAT_PERIOD_SEC 5
#define LRW_HEARTBEAT_TIMEOUT_MS 30000
static int m_wdog_channel = -1;
static struct k_work_delayable m_heartbeat_work;
#endif

/* --- Timers (each one meaning only) --- */
static struct k_timer m_lc_timeout_timer;
static struct k_timer m_rejoin_timer;

/* --- Works --- */
static struct k_work m_send_work;            /* drains response/alarm, then composes telemetry */
static struct k_work_delayable m_frame_work; /* multi-frame snapshot continuation */
static struct k_work m_join_work;
static struct k_work m_link_check_work;       /* LC timeout (from m_lc_timeout_timer) */
static struct k_work m_downlink_success_work; /* deferred from downlink_callback */
static struct k_work m_clock_sync_info_work;  /* deferred ClockSync Info uplink (#219) */
static struct k_work m_lc_response_work;      /* deferred from link_check_callback */
static struct k_work m_force_lc_work;         /* arm a forced LC on the next telemetry */
static struct k_work m_dl_request_work;       /* drains m_dl_msgq (port-85 commands) */
static struct k_work_delayable m_post_cmd_work;
static struct k_work_delayable m_join_complete_work;
static struct k_work_delayable m_hist_work;
static struct k_work_delayable
	m_tx_retry_work; /* re-drains response/alarm after a -EAGAIN backoff */

/* Multi-frame telemetry: gap before the next frame of the same snapshot (covers
 * RX1/RX2 windows) and backoff when a send is refused (duty cycle / MAC busy). */
#define FRAME_GAP_SEC     3
#define FRAME_RETRY_SEC   15
/* Duty-cycle/MAC-busy retries before abandoning a telemetry frame (#219); mirrors
 * HISTORY_MAX_RETRIES so a permanent TX error can't be retried forever. */
#define FRAME_MAX_RETRIES 8

static uint8_t m_frame_buf[64];
static size_t m_frame_len;
static bool m_frame_more;
static bool m_frame_first;
static bool m_frame_resend;
static int m_frame_retries;  /* consecutive lorawan_send failures on the current frame (#219) */
static bool m_frame_with_lc; /* #188: link-check decision, computed once at compose,
			      * reused on resend (should_request_link_check() has a side
			      * effect — must not re-run on a duty-cycle retry) */

static void tx_telemetry_frame(bool first_frame);

/* History replay (#52): one ReqHistory streams all matching records back as N
 * HistoryFrame uplinks on the command port, ASAP. */
#define HISTORY_SAMPLES_MAX 48 /* nanopb Response.HistoryFrame.samples bound */
#define HISTORY_MAX_RETRIES 8  /* duty-cycle/MAC-busy retries before aborting a frame (#89) */

static bool m_hist_active;
static uint32_t m_hist_from, m_hist_to, m_hist_seq;
static uint32_t m_hist_count;
static uint32_t m_hist_idx;
static size_t m_hist_cursor;
static uint32_t m_hist_present; /* shared sensor mask (uint32), snapshot at replay start */
static uint32_t m_hist_interval;
static int m_hist_retries; /* consecutive lorawan_send failures on the current frame (#89) */
/* Full encoded HistoryFrame Response + version byte; the old 64 B overflowed
 * once samples filled (frame ~70-90 B) so replay silently died on DR3+ (#89). */
static uint8_t m_hist_tx_buf[APP_CMD_HISTORY_FRAME_BUF_SIZE];

static void m_hist_work_handler(struct k_work *work);

/* --- State machine --- */
static atomic_t m_state = ATOMIC_INIT(APP_LRW_STATE_IDLE);
static int m_consecutive_lc_fail;   /* LC failures in a row (HEALTHY) */
static int m_consecutive_lc_ok;     /* LC successes in a row (WARNING) */
static int m_warning_lc_fail_total; /* Total LC failures in WARNING */
static int m_force_lc_remaining;    /* Remaining forced LC messages */
static bool m_link_check_pending;   /* Waiting for LC response */
static int m_message_count;         /* Message counter for N-th LC */
static int m_rejoin_attempts;       /* Rejoin attempt counter for backoff */
static int m_join_busy_polls;       /* Counter for MAC busy polling */
static bool m_init_join;            /* True for first join after boot */

#define JOIN_BUSY_POLL_INTERVAL_MS 500
#define JOIN_BUSY_MAX_POLLS        30

static int m_current_dr;
/* Application-payload budget (bytes), refreshed from lorawan_get_payload_sizes()
 * before each composing TX (MED-6: was cached only on DR-change/join). */
static uint8_t m_max_next_payload;
static int16_t m_last_rssi;
static int8_t m_last_snr;
static uint8_t m_last_margin;
static uint8_t m_last_gw_count;
static uint8_t m_lc_response_gw_count;

/* --- TX queues (MED-7/8: were single overwrite-able slots) --- */
#define APP_LRW_RESPONSE_BUF_SIZE 64
/* Incoming command buffer. The network can deliver up to the LoRaWAN MTU
 * (~222 B at the highest DR) on a single downlink; a realistic SetParam with
 * deveui+joineui+appkey encodes to ~90 B. 224 covers the full MTU so large
 * commands are never silently dropped (#93.4 — was 64, which dropped them with
 * only a LOG_WRN and the host never learned the command wasn't processed). */
#define APP_LRW_REQUEST_BUF_SIZE  224
#define APP_LRW_DOWNLINK_CMD_PORT 85
#define APP_LRW_ALARM_PORT        3
#define APP_LRW_TX_QUEUE_DEPTH    4
/* Downlink-command FIFO. Each slot is a full-MTU lrw_dl_msg (~228 B), and the
 * network delivers at most one port-85 command per RX window, drained promptly by
 * m_dl_request_work. Depth 2 absorbs a back-to-back pair while halving the buffer
 * vs the old depth 4 (saves ~456 B RAM, #221.4). */
#define APP_LRW_DL_QUEUE_DEPTH    2

/* The request buffer must hold a full-MTU downlink so the largest command the
 * network can deliver still fits (#93.4/#93.7). Response/frame buffers are
 * deliberately NOT asserted against the nanopb *_size bounds: those frames are
 * DR-budget-limited and paged (get_config/get_param/history), so the on-air
 * size is always far below the protobuf worst case. */
BUILD_ASSERT(APP_LRW_REQUEST_BUF_SIZE >= 222, "request buffer below LoRaWAN MTU");

struct lrw_tx_msg {
	uint8_t port;
	uint16_t len;
	uint8_t buf[APP_LRW_RESPONSE_BUF_SIZE];
};
struct lrw_dl_msg {
	uint16_t len;
	uint8_t buf[APP_LRW_REQUEST_BUF_SIZE];
};

K_MSGQ_DEFINE(m_response_msgq, sizeof(struct lrw_tx_msg), APP_LRW_TX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(m_alarm_msgq, sizeof(struct lrw_tx_msg), APP_LRW_TX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(m_dl_msgq, sizeof(struct lrw_dl_msg), APP_LRW_DL_QUEUE_DEPTH, 4);

/* Deferred reboot/save requested by a command handler; runs after the Ack TX. */
static enum app_cmd_action m_post_cmd_action;

/* Set by a ClockSync command; the next network time-update sends an Info uplink
 * (with the synced unix_time) instead of the command acking immediately. */
/* #193: set from a command-handler thread, test-and-cleared in the LoRaMac
 * downlink callback (another context) — an atomic bit closes the lost-update race. */
static atomic_t m_clock_sync_info_pending;

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
static void state_transition(enum app_lrw_state new_state);
static bool should_request_link_check(void);
static void force_lc_work_handler(struct k_work *work);

static void fire_ready_cb(void)
{
	if (m_ready_cb) {
		m_ready_cb();
	}
}

static uint32_t calculate_rejoin_backoff(int attempt)
{
	uint32_t backoff = REJOIN_BACKOFF_BASE_SEC;

	for (int i = 0; i < attempt; i++) {
		backoff *= REJOIN_BACKOFF_MULTIPLIER;
		if (backoff >= REJOIN_BACKOFF_MAX_SEC) {
			return REJOIN_BACKOFF_MAX_SEC;
		}
	}
	return backoff;
}

static uint8_t refresh_payload_budget(void)
{
	uint8_t max_next = 0, max_now = 0;

	lorawan_get_payload_sizes(&max_next, &max_now);
	m_max_next_payload = max_next;
	return max_next;
}

uint8_t app_lrw_get_max_payload(void)
{
	return m_max_next_payload;
}

/* ======================================================================== */
/* State machine                                                            */
/* ======================================================================== */

static const char *state_name(enum app_lrw_state s)
{
	switch (s) {
	case APP_LRW_STATE_IDLE:
		return "IDLE";
	case APP_LRW_STATE_JOINING:
		return "JOINING";
	case APP_LRW_STATE_HEALTHY:
		return "HEALTHY";
	case APP_LRW_STATE_WARNING:
		return "WARNING";
	case APP_LRW_STATE_RECONNECT:
		return "RECONNECT";
	case APP_LRW_STATE_DISABLED:
		return "DISABLED";
	default:
		return "?";
	}
}

/* The ONLY place m_state changes. Runs exit action of the old state then entry
 * action of the new state. Must be called on m_work_q. */
static void state_transition(enum app_lrw_state new_state)
{
	enum app_lrw_state old = (enum app_lrw_state)atomic_get(&m_state);

	/* --- Exit actions --- */
	switch (old) {
	case APP_LRW_STATE_JOINING:
		k_work_cancel_delayable(&m_join_complete_work);
		break;
	case APP_LRW_STATE_HEALTHY:
	case APP_LRW_STATE_WARNING:
		k_timer_stop(&m_lc_timeout_timer);
		m_link_check_pending = false;
		break;
	case APP_LRW_STATE_RECONNECT:
		k_timer_stop(&m_rejoin_timer);
		break;
	default:
		break;
	}

	atomic_set(&m_state, new_state);
	LOG_INF("State: %s -> %s", state_name(old), state_name(new_state));

	/* --- Entry actions --- */
	switch (new_state) {
	case APP_LRW_STATE_IDLE:
	case APP_LRW_STATE_DISABLED:
		/* Radio-silent: no link-check, no rejoin. (The report cadence is
		 * app_report's; it self-pauses while not app_lrw_is_ready().) */
		k_timer_stop(&m_lc_timeout_timer);
		k_timer_stop(&m_rejoin_timer);
		break;

	case APP_LRW_STATE_JOINING:
		/* Drop any in-flight history replay across (re)join. */
		m_hist_active = false;
		app_history_set_replay_active(false);
		break;

	case APP_LRW_STATE_HEALTHY:
		/* Link confirmed: clear all LC/recovery state. */
		m_consecutive_lc_fail = 0;
		m_consecutive_lc_ok = 0;
		m_warning_lc_fail_total = 0;
		m_force_lc_remaining = 0;
		m_rejoin_attempts = 0;
		m_link_check_pending = false;
		break;

	case APP_LRW_STATE_WARNING:
		m_consecutive_lc_ok = 0;
		m_warning_lc_fail_total = 0;
		m_force_lc_remaining = 0;
		break;

	case APP_LRW_STATE_RECONNECT: {
		/* Single, consistent rejoin policy (HIGH-4): always escalate the
		 * backoff and arm the rejoin timer here — never reset attempts. */
		uint32_t backoff = calculate_rejoin_backoff(m_rejoin_attempts);

		m_rejoin_attempts++;
		LOG_WRN("Rejoin in %u s (attempt %d)", backoff, m_rejoin_attempts);
		k_timer_start(&m_rejoin_timer, K_SECONDS(backoff), K_FOREVER);
		break;
	}

	default:
		break;
	}
}

/* ======================================================================== */
/* Event handlers (run on m_work_q)                                         */
/* ======================================================================== */

/* Build a GetInfo response and stage it on the command port. Shared by the
 * on-join announce and the deferred clock-sync uplink so the encode buffer and
 * queue call live in one place (#220.F). Returns the app_cmd_build_info() result. */
static int queue_info_uplink(void)
{
	uint8_t info_buf[APP_LRW_RESPONSE_BUF_SIZE];
	size_t info_len;
	int ret = app_cmd_build_info(info_buf, sizeof(info_buf), &info_len);
	if (ret == 0) {
		(void)app_lrw_queue_response(APP_LRW_DOWNLINK_CMD_PORT, info_buf, info_len);
	}
	return ret;
}

static void on_join_success(void)
{
	LOG_INF("Join successful");
	m_init_join = false; /* Next join will be a rejoin with MAC reset */
	lorawan_enable_adr(g_app_config.lrw_adr);

	/* Capture the initial DR's payload budget; the DR-changed callback may not
	 * fire on join. */
	refresh_payload_budget();

	state_transition(APP_LRW_STATE_HEALTHY); /* resets all counters + attempts */
	m_message_count = 0;

	/* Request network time once joined; the answer sets the RTC asynchronously. */
	app_clock_request_sync();

	/* Autonomous GetInfo on join: announce identity/firmware on fPort 85 before
	 * the first telemetry. send_work drains queued responses first. */
	if (queue_info_uplink() != 0) {
		LOG_WRN("app_cmd_build_info failed; skipping GetInfo-on-join");
	}

	/* Kick app_report to start the report cadence with an immediate uplink (its
	 * cycle samples, captures and triggers app_lrw_send_telemetry; the first
	 * telemetry frame carries LC, msg #1). The queued GetInfo above drains first
	 * via m_send_work. */
	fire_ready_cb();
}

static void on_join_failure(void)
{
	/* MAC activation failure (start/join error or not-activated). Applies to
	 * both OTAA and ABP — this is about getting the MAC session up, not link
	 * health. The RECONNECT entry action arms the backoff. */
	state_transition(APP_LRW_STATE_RECONNECT);
}

static void on_lc_failure(void)
{
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	k_timer_stop(&m_lc_timeout_timer);
	m_link_check_pending = false;
	m_consecutive_lc_ok = 0;

	switch (state) {
	case APP_LRW_STATE_HEALTHY:
		m_consecutive_lc_fail++;
		LOG_WRN("LC FAIL in HEALTHY (streak: %d/%d)", m_consecutive_lc_fail,
			FAIL_THRESHOLD_WARNING);
		if (m_consecutive_lc_fail >= FAIL_THRESHOLD_WARNING) {
			state_transition(APP_LRW_STATE_WARNING);
		}
		break;

	case APP_LRW_STATE_WARNING:
		m_warning_lc_fail_total++;
		LOG_WRN("LC FAIL in WARNING (total: %d/%d)", m_warning_lc_fail_total,
			g_app_config.lrw_link_check_fail_rejoin);
		if (m_warning_lc_fail_total >= g_app_config.lrw_link_check_fail_rejoin) {
			if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_OTAA) {
				state_transition(APP_LRW_STATE_RECONNECT);
			} else {
				/* ABP cannot rejoin (no OTA). Stay in WARNING and keep
				 * trying the normal N-th-message link check; recover if
				 * the link returns. */
				LOG_WRN("ABP mode - cannot rejoin, staying in WARNING");
				m_warning_lc_fail_total = 0;
			}
		}
		break;

	default:
		LOG_DBG("LC FAIL in %s (ignored)", state_name(state));
		break;
	}
}

static void on_lc_success(void)
{
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	k_timer_stop(&m_lc_timeout_timer);
	m_link_check_pending = false;
	m_consecutive_lc_fail = 0;

	switch (state) {
	case APP_LRW_STATE_HEALTHY:
		LOG_INF("LC OK in HEALTHY");
		m_force_lc_remaining = 0;
		break;

	case APP_LRW_STATE_WARNING:
		m_consecutive_lc_ok++;
		LOG_INF("LC OK in WARNING (streak: %d/%d)", m_consecutive_lc_ok,
			OK_THRESHOLD_HEALTHY);
		if (m_consecutive_lc_ok >= OK_THRESHOLD_HEALTHY) {
			state_transition(APP_LRW_STATE_HEALTHY);
		} else {
			m_force_lc_remaining = 1; /* force LC on next message */
		}
		break;

	default:
		LOG_DBG("LC OK in %s", state_name(state));
		break;
	}
}

static void on_lc_timeout(void)
{
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state != APP_LRW_STATE_HEALTHY && state != APP_LRW_STATE_WARNING) {
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
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state != APP_LRW_STATE_HEALTHY && state != APP_LRW_STATE_WARNING) {
		return;
	}
	if (m_link_check_pending) {
		/* A received downlink also confirms the link is alive. */
		on_lc_success();
		LOG_INF("Connection confirmed via downlink (LC pending)");
	} else {
		LOG_INF("Downlink received (no LC pending)");
	}
}

#if defined(CONFIG_SHELL)
/* Debug: inject a synthetic link-check outcome onto m_work_q so the state
 * machine can be driven deterministically from the shell (`ats lrw lc ...`)
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

void app_lrw_debug_inject_lc(bool ok)
{
	m_dbg_lc_ok = ok;
	k_work_submit_to_queue(&m_work_q, &m_dbg_lc_work);
}
#endif /* CONFIG_SHELL */

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
 * Runs on m_work_q like every other TX-side work item. */
static void clock_sync_info_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)queue_info_uplink();
}

static void lc_response_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	/* HIGH-3: ignore a stale/late LC answer outside HEALTHY/WARNING so it can
	 * never cancel the rejoin timer or mutate counters in JOINING/RECONNECT. */
	if (state != APP_LRW_STATE_HEALTHY && state != APP_LRW_STATE_WARNING) {
		LOG_DBG("LC answer in %s ignored", state_name(state));
		return;
	}

	if (m_lc_response_gw_count == 0) {
		on_lc_failure();
	} else {
		on_lc_success();
	}
}

static void post_cmd_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	switch (m_post_cmd_action) {
	case APP_CMD_ACTION_SETTINGS_SAVE:
		LOG_INF("Command: saving settings + reboot");
		app_settings_save(true);
		break;
	case APP_CMD_ACTION_FACTORY_RESET:
		LOG_INF("Command: factory reset (keep identity + LoRaWAN) + reboot");
		app_settings_reset();
		break;
	case APP_CMD_ACTION_REBOOT:
		LOG_INF("Command: reboot");
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case APP_CMD_ACTION_ENTER_CALIBRATION:
		/* Persist calibration=true + reboot; main() enters calibration
		 * mode on the next boot (app_calibration_init() clears the flag).
		 * Write the staging config (app_config()) — that is what settings_save
		 * persists; g_app_config is only the boot-time read copy. */
		LOG_INF("Command: entering calibration mode + reboot");
		app_config()->calibration = true;
		app_settings_save(true);
		break;
	case APP_CMD_ACTION_LRW_RESET:
		/* Wipe the LoRaWAN NVM (frame counters + DevNonce + session), then cold
		 * reboot so the MAC re-initialises from a clean NVM (#109). Same path as
		 * `ats lrw reset`. The Ack uplink has already left (deferred 8s). */
		LOG_INF("Command: LoRaWAN reset (NVM wipe) + reboot");
		app_lrw_reset_nvm();
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case APP_CMD_ACTION_LRW_JOIN:
		/* Force a (re)join now instead of waiting for the next attempt (#109).
		 * No reboot — app_lrw_join() just queues a join work item. */
		LOG_INF("Command: forced LoRaWAN join");
		app_lrw_join();
		break;
	case APP_CMD_ACTION_COUNTERS_SAVE:
		/* Persist the (reset) pulse totalizers, no reboot. Deferred for the
		 * same stack reason as the alarm-rule save above. */
		LOG_INF("Command: saving counters");
		app_counters_save(true);
		break;
	default:
		break;
	}
}

static void dl_request_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct lrw_dl_msg msg;

	/* Drain every queued command (MED-7: was a single overwrite-able slot). */
	while (k_msgq_get(&m_dl_msgq, &msg, K_NO_WAIT) == 0) {
		static uint8_t resp[APP_LRW_RESPONSE_BUF_SIZE];
		size_t resp_len = 0;
		enum app_cmd_action action = APP_CMD_ACTION_NONE;

		int ret = app_cmd_handle(APP_CMD_TRANSPORT_LRW, msg.buf, msg.len, resp,
					 sizeof(resp), &resp_len, &action);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_cmd_handle", ret);
			continue;
		}

		if (resp_len) {
			ret = app_lrw_queue_response(APP_LRW_DOWNLINK_CMD_PORT, resp, resp_len);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_lrw_queue_response", ret);
			}
		}

		/* Defer reboot/save so the Ack uplink + its RX window finish first. */
		if (action != APP_CMD_ACTION_NONE) {
			m_post_cmd_action = action;
			k_work_schedule_for_queue(&m_work_q, &m_post_cmd_work, K_SECONDS(8));
			LOG_INF("Post-command action %d scheduled in 8s", (int)action);
		}
	}

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	size_t unused;
	if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
		LOG_INF("m_work_q stack: %zu B unused after cmd handle", unused);
	}
#endif
}

static void join_complete_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if ((enum app_lrw_state)atomic_get(&m_state) != APP_LRW_STATE_JOINING) {
		LOG_DBG("Join complete handler: not JOINING, ignoring");
		return;
	}

	/* MAC layer still busy (join request in progress)? */
	if (LoRaMacIsBusy()) {
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
		k_work_schedule_for_queue(&m_work_q, &m_join_complete_work,
					  K_MSEC(JOIN_BUSY_POLL_INTERVAL_MS));
		return;
	}

	/* MAC ready - verify activation status. */
	MibRequestConfirm_t mib_req;

	mib_req.Type = MIB_NETWORK_ACTIVATION;
	if (LoRaMacMibGetRequestConfirm(&mib_req) != LORAMAC_STATUS_OK ||
	    mib_req.Param.NetworkActivation == ACTIVATION_TYPE_NONE) {
		LOG_ERR("Join failed (not activated)");
		on_join_failure();
		return;
	}

	on_join_success();
}

static bool deveui_is_zero(void)
{
	for (size_t i = 0; i < sizeof(g_app_config.lrw_deveui); i++) {
		if (g_app_config.lrw_deveui[i] != 0) {
			return false;
		}
	}
	return true;
}

static bool devaddr_is_zero(void)
{
	for (size_t i = 0; i < sizeof(g_app_config.lrw_devaddr); i++) {
		if (g_app_config.lrw_devaddr[i] != 0) {
			return false;
		}
	}
	return true;
}

/* "Never provisioned" — the identifier that MUST be set for the configured
 * activation mode is all-zero. For OTAA that is the DevEUI; for ABP the DevEUI is
 * unused (and legitimately left blank), so provisioning is keyed on the DevAddr
 * instead. Applying the DevEUI test to ABP (the #98 radio-silent guard did) left
 * a correctly-configured ABP device permanently DISABLED — no uplinks, but a
 * healthy-looking green LED (C-1). */
static bool lrw_unprovisioned(void)
{
	if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_ABP) {
		return devaddr_is_zero();
	}
	return deveui_is_zero();
}

static void join_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int ret;

	/* Radio-silent mode (#98): an unprovisioned device (OTAA with no DevEUI, or
	 * ABP with no DevAddr) can never succeed — don't burn power on join requests.
	 * Enter DISABLED and stay there until reprovisioned + rebooted. */
	if (lrw_unprovisioned()) {
		if ((enum app_lrw_state)atomic_get(&m_state) != APP_LRW_STATE_DISABLED) {
			LOG_WRN("LoRaWAN not provisioned (%s): disabled (radio-silent)",
				g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_ABP
					? "ABP/DevAddr"
					: "OTAA/DevEUI");
			state_transition(APP_LRW_STATE_DISABLED);
		}
		return;
	}

	/* MED-10: ignore re-entry while a join is already in progress. */
	if ((enum app_lrw_state)atomic_get(&m_state) == APP_LRW_STATE_JOINING) {
		LOG_WRN("Join already in progress, ignoring request");
		return;
	}

	state_transition(APP_LRW_STATE_JOINING); /* stops send timer, drops history */

	/* Discard any in-progress telemetry snapshot: a rejoin must not resume a
	 * pre-outage snapshot with stale sensor data and no indication (#93.5). */
	app_compose_reset();

	if (m_init_join) {
		LOG_INF("Initial join after boot");
	} else {
		LOG_INF("Rejoin attempt %d...", m_rejoin_attempts);
		LOG_INF("Deinitializing MAC...");
		LoRaMacDeInitialization();

		ret = lorawan_start();
		if (ret) {
			LOG_ERR("lorawan_start failed: %d", ret);
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
		state_transition(APP_LRW_STATE_IDLE);
		return;
	}

	m_join_busy_polls = 0;

	ret = lorawan_join(&config);
	if (ret && ret != -ETIMEDOUT) {
		LOG_ERR("Join failed: %d", ret);
		on_join_failure();
		return;
	}

	/* ABP: explicit RX delays matching the LNS (1s/2s defaults are fine). */
	if (config.mode == LORAWAN_ACT_ABP) {
		MibRequestConfirm_t mib;

		mib.Type = MIB_RECEIVE_DELAY_1;
		mib.Param.ReceiveDelay1 = 1000;
		LoRaMacMibSetRequestConfirm(&mib);

		mib.Type = MIB_RECEIVE_DELAY_2;
		mib.Param.ReceiveDelay2 = 2000;
		LoRaMacMibSetRequestConfirm(&mib);

		LOG_INF("RX delays set: RX1=1s, RX2=2s");
	}

	/* Private sync word only when explicitly configured (default stays public). */
	if (g_app_config.lrw_network == APP_CONFIG_LRW_NETWORK_PRIVATE) {
		MibRequestConfirm_t mib;

		mib.Type = MIB_PUBLIC_NETWORK;
		mib.Param.EnablePublicNetwork = false;
		LoRaMacMibSetRequestConfirm(&mib);
		LOG_INF("Network type: private (sync word 0x12)");
	}

	LOG_INF("lorawan_join() ret=%d, polling MAC...", ret);
	k_work_schedule_for_queue(&m_work_q, &m_join_complete_work,
				  K_MSEC(JOIN_BUSY_POLL_INTERVAL_MS));
}

static bool should_request_link_check(void)
{
	int interval = g_app_config.lrw_link_check_interval;

	if (m_force_lc_remaining > 0) {
		m_force_lc_remaining--;
		return true;
	}
	if (interval <= 0) {
		return false;
	}
	int msg_num = m_message_count + 1;

	if (msg_num == 1 || (msg_num % interval) == 0) {
		return true;
	}
	return false;
}

/* Send one telemetry frame on fPort 2. */
static void tx_telemetry_frame(bool first_frame)
{
	int ret;

	if (!m_frame_resend) {
		refresh_payload_budget(); /* MED-6: per-TX budget, not cached */
		ret = app_compose(m_frame_buf, sizeof(m_frame_buf), &m_frame_len, &m_frame_more);
		if (ret == -EAGAIN) {
			LOG_DBG("Telemetry budget unavailable, skipping TX");
			return;
		}
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_compose", ret);
			return;
		}
		if (m_frame_len == 0) {
			/* Nothing to report (e.g. all sensors NaN pre-sample); app_report
			 * owns the cadence, so just return. */
			return;
		}
		m_frame_retries = 0; /* fresh frame composed; reset the retry budget (#219) */
		m_frame_first = first_frame;
		/* #188: decide the link-check piggyback exactly once, here on the fresh
		 * compose. should_request_link_check() decrements m_force_lc_remaining and
		 * advances the modulo, so re-evaluating it on the resend path would
		 * double-consume / lose a forced link check. */
		m_frame_with_lc =
			m_frame_first && !m_link_check_pending && should_request_link_check();
	}

	bool with_link_check = m_frame_with_lc;

	if (with_link_check) {
		ret = lorawan_request_link_check(false);
		if (ret) {
			LOG_ERR("Link check request failed: %d", ret);
			with_link_check = false;
		} else {
			m_link_check_pending = true;
			k_timer_start(&m_lc_timeout_timer, K_SECONDS(LINK_CHECK_TIMEOUT_SEC),
				      K_FOREVER);
		}
	}

	LOG_INF("Sending data (msg #%u, %s)...", m_message_count + 1,
		m_frame_more ? "more pending" : "last frame");

	ret = lorawan_send(2, m_frame_buf, m_frame_len, LORAWAN_MSG_UNCONFIRMED);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lorawan_send", ret);
		if (with_link_check) {
			m_link_check_pending = false;
			k_timer_stop(&m_lc_timeout_timer);
		}
		/* Likely duty-cycle / MAC busy — retry the same frame shortly, but bound
		 * the attempts so a permanent TX error (e.g. misconfigured duty cycle)
		 * can't be retried indefinitely. After the budget is spent, drop the
		 * frame and let app_report re-arm the cadence cleanly (#219). */
		if (++m_frame_retries > FRAME_MAX_RETRIES) {
			LOG_ERR("Telemetry frame abandoned after %d retries", m_frame_retries - 1);
			m_frame_resend = false;
			m_frame_more = false;
			m_frame_retries = 0;
			return;
		}
		m_frame_resend = true;
		k_work_schedule_for_queue(&m_work_q, &m_frame_work, K_SECONDS(FRAME_RETRY_SEC));
		return;
	}

	m_frame_resend = false;
	m_frame_retries = 0;
	m_message_count++;

	if (m_frame_more) {
		k_work_schedule_for_queue(&m_work_q, &m_frame_work, K_SECONDS(FRAME_GAP_SEC));
	} else {
		/* Snapshot complete; app_report's timer schedules the next report. */
		LOG_INF("Snapshot complete");
	}
}

static void frame_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state == APP_LRW_STATE_JOINING || state == APP_LRW_STATE_RECONNECT) {
		LOG_WRN("Frame continuation aborted: %s", state_name(state));
		return;
	}
	tx_telemetry_frame(false);
}

static void tx_retry_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_work_submit_to_queue(&m_work_q, &m_send_work);
}

/* Send one queued response/alarm with a DR-budget guard and a bounded
 * duty-cycle retry (#93.1). Returns true if the message was consumed (sent or
 * dropped); false if it was requeued for a later retry (a backoff re-drain is
 * already scheduled, so the caller must not touch the timers). */
static bool tx_send_queued(struct k_msgq *q, struct lrw_tx_msg *tx, uint8_t port)
{
	uint8_t budget = refresh_payload_budget();

	if (tx->len > budget) {
		/* Won't fit at this DR — Zephyr's lorawan_send would transmit an empty
		 * frame and drop the payload anyway, so drop it explicitly with a log
		 * rather than burning airtime on an empty uplink. */
		LOG_ERR("TX %u B over DR budget %u B (port %u); dropped", tx->len, budget, port);
		return true;
	}

	int ret = lorawan_send(port, tx->buf, tx->len, LORAWAN_MSG_UNCONFIRMED);
	if (ret == 0) {
		LOG_INF("Sent on port %u (%u B)", port, tx->len);
		return true;
	}

	/* Likely duty-cycle / MAC busy: keep the payload and retry after a backoff
	 * instead of losing it (the slot was already consumed by k_msgq_get). */
	LOG_ERR_CALL_FAILED_INT("lorawan_send", ret);
	if (k_msgq_put(q, tx, K_NO_WAIT) != 0) {
		LOG_WRN("TX requeue failed (port %u); dropped", port);
		return true;
	}
	k_work_schedule_for_queue(&m_work_q, &m_tx_retry_work, K_SECONDS(FRAME_RETRY_SEC));
	return false;
}

static void send_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Block normal transmissions during calibration mode (flag-based). */
	if (g_app_config.calibration) {
		return;
	}

	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	/* Radio-silent (#98/#175): the stack was never started, never transmit. */
	if (state == APP_LRW_STATE_DISABLED) {
		return;
	}

	if (state == APP_LRW_STATE_JOINING || state == APP_LRW_STATE_RECONNECT) {
		LOG_WRN("TX blocked: %s", state_name(state));
		return;
	}

	struct lrw_tx_msg tx;

	/* Priority drain: command response (port 85) first, then alarm (port 3). One
	 * TX per call; if more are queued, re-submit. A drain never falls through to
	 * telemetry — telemetry is composed only when this handler runs with both
	 * queues already empty (i.e. when app_report triggered the send). */
	if (k_msgq_get(&m_response_msgq, &tx, K_NO_WAIT) == 0) {
		if (!tx_send_queued(&m_response_msgq, &tx, tx.port)) {
			return; /* requeued; a backoff retry is scheduled */
		}
		if (k_msgq_num_used_get(&m_response_msgq) || k_msgq_num_used_get(&m_alarm_msgq)) {
			k_work_submit_to_queue(&m_work_q, &m_send_work);
		}
		return;
	}

	if (k_msgq_get(&m_alarm_msgq, &tx, K_NO_WAIT) == 0) {
		if (!tx_send_queued(&m_alarm_msgq, &tx, APP_LRW_ALARM_PORT)) {
			return; /* requeued; a backoff retry is scheduled */
		}
		if (k_msgq_num_used_get(&m_alarm_msgq)) {
			k_work_submit_to_queue(&m_work_q, &m_send_work);
		}
		return;
	}

	/* MED-9: history replay owns the radio; don't inject telemetry. */
	if (m_hist_active) {
		return;
	}

	/* #189: a multi-frame snapshot continuation may still be in flight — a
	 * frame_work fire is scheduled after a frame gap (m_frame_more) or a resend
	 * backoff. Re-entering compose now would clobber the snapshot cursor flags
	 * (m_frame_resend/m_frame_first/m_frame_with_lc) and the LC piggyback, yielding
	 * a malformed multi-frame sequence. Let the in-flight snapshot finish; app_report
	 * re-triggers the next one. */
	if (k_work_delayable_is_pending(&m_frame_work)) {
		LOG_DBG("Snapshot continuation pending; skipping new telemetry compose");
		return;
	}

	/* Both queues empty: compose + split the telemetry snapshot built from the
	 * sensor data app_report just sampled/captured. */
	m_frame_resend = false; /* start a new snapshot */
	tx_telemetry_frame(true);
}

/* ======================================================================== */
/* History replay                                                           */
/* ======================================================================== */

/* Max samples that fit one frame at the current DR. Uses the exact protobuf
 * envelope overhead (app_cmd_history_sample_capacity) instead of a fixed guess
 * that overflowed m_hist_tx_buf on DR3+ with a synced RTC (#89). Worst-case
 * (max-varint) frame_index/count/t0 give a stable per-replay lower bound. */
static size_t history_frame_cap(void)
{
	size_t out_cap = MIN((size_t)refresh_payload_budget(), sizeof(m_hist_tx_buf)); /* MED-6 */

	return app_cmd_history_sample_capacity(m_hist_seq, UINT32_MAX, UINT32_MAX, UINT32_MAX,
					       m_hist_present, m_hist_interval, out_cap);
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

	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state == APP_LRW_STATE_JOINING || state == APP_LRW_STATE_RECONNECT) {
		LOG_WRN("History replay aborted: %s", state_name(state));
		m_hist_active = false;
		return; /* the (re)join → HEALTHY entry / send path restarts cadence */
	}

	size_t cap = history_frame_cap();
	uint8_t samples[HISTORY_SAMPLES_MAX];
	uint32_t t0 = 0;
	uint16_t n = 0;
	size_t next = m_hist_cursor;
	size_t slen = 0;

	if (cap > 0) {
		slen = app_history_export_page(m_hist_from, m_hist_to, m_hist_cursor, samples, cap,
					       &t0, &n, &next);
	}
	if (n == 0) {
		LOG_WRN("History replay stop at frame %u/%u (cap=%uB)", (unsigned)m_hist_idx,
			(unsigned)m_hist_count, (unsigned)cap);
		history_replay_finish();
		return;
	}

	size_t len;
	int ret = app_cmd_build_history_frame(m_hist_seq, m_hist_idx, m_hist_count, t0,
					      m_hist_present, m_hist_interval, samples, slen,
					      m_hist_tx_buf, sizeof(m_hist_tx_buf), &len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_cmd_build_history_frame", ret);
		history_replay_finish();
		return;
	}

	ret = lorawan_send(APP_LRW_DOWNLINK_CMD_PORT, m_hist_tx_buf, len, LORAWAN_MSG_UNCONFIRMED);
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
		k_work_schedule_for_queue(&m_work_q, &m_hist_work, K_SECONDS(FRAME_RETRY_SEC));
		return;
	}
	m_hist_retries = 0;

	LOG_INF("History frame %u/%u sent (%u rec, %zu B)", (unsigned)(m_hist_idx + 1),
		(unsigned)m_hist_count, (unsigned)n, len);
	m_hist_cursor = next;
	m_hist_idx++;

	/* Terminate on cursor exhaustion, not frame_index == frame_count (#89): a DR
	 * change mid-replay alters records-per-frame, so the up-front frame_count is
	 * only an estimate. The host concatenates by frame_index. */
	if (m_hist_cursor < app_history_count()) {
		k_work_schedule_for_queue(&m_work_q, &m_hist_work, K_SECONDS(FRAME_GAP_SEC));
	} else {
		LOG_INF("History replay complete: %u frames", (unsigned)m_hist_idx);
		history_replay_finish();
	}
}

bool app_lrw_start_history_replay(uint32_t from_unix, uint32_t to_unix, uint32_t seq)
{
	if (!app_lrw_is_ready()) {
		LOG_WRN("History replay requested but LRW not ready; ignoring");
		return false;
	}

	/* Seed the snapshot fields the cap depends on (seq/present/interval) before
	 * sizing a frame, so counting and sending use an identical per-frame cap. */
	m_hist_from = from_unix;
	m_hist_to = to_unix;
	m_hist_seq = seq;
	m_hist_present = app_history_get_mask();
	m_hist_interval = app_history_get_interval();

	size_t cap = history_frame_cap();
	uint32_t n = (cap > 0) ? app_history_count_frames(from_unix, to_unix, cap) : 0;

	if (n == 0) {
		LOG_INF("History replay: no records in window (or DR too low)");
		return false;
	}

	m_hist_count = n;
	m_hist_idx = 0;
	m_hist_cursor = 0;
	m_hist_retries = 0;
	m_hist_active = true;
	app_history_set_replay_active(true); /* pause capture; app_report telemetry self-skips */

	LOG_INF("History replay start: %u frames (window %u..%u)", (unsigned)n, from_unix, to_unix);
	k_work_schedule_for_queue(&m_work_q, &m_hist_work, K_NO_WAIT);
	return true;
}

/* ======================================================================== */
/* Timer ISR handlers (thin: enqueue the right work, no state decisions)    */
/* ======================================================================== */

static void lc_timeout_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit_to_queue(&m_work_q, &m_link_check_work);
}

static void rejoin_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit_to_queue(&m_work_q, &m_join_work);
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

	if (data) {
		LOG_HEXDUMP_INF(data, len, "Payload: ");
	}

	/* Set the RTC from the network if this downlink carried a DeviceTimeAns. */
	app_clock_handle_downlink(flags);

	/* Deferred answer to a ClockSync command: once the network time actually
	 * lands, send an Info uplink carrying the freshly-synced unix_time (the
	 * command itself does not ack — saves an uplink, and a bare ack couldn't
	 * carry the synced time yet). The Info encode is pushed to m_work_q so it
	 * never runs on the callback stack (#219). */
	if ((flags & LORAWAN_TIME_UPDATED) &&
	    atomic_test_and_clear_bit(&m_clock_sync_info_pending, 0)) {
		k_work_submit_to_queue(&m_work_q, &m_clock_sync_info_work);
	}

	if (port == APP_LRW_DOWNLINK_CMD_PORT && data && len > 0) {
		if (len <= APP_LRW_REQUEST_BUF_SIZE) {
			struct lrw_dl_msg msg;

			msg.len = len;
			memcpy(msg.buf, data, len);
			/* MED-7: queue (don't clobber a single slot). */
			if (k_msgq_put(&m_dl_msgq, &msg, K_NO_WAIT) != 0) {
				LOG_WRN("Downlink command queue full; dropping");
			} else {
				k_work_submit_to_queue(&m_work_q, &m_dl_request_work);
			}
		} else {
			LOG_WRN("Port %u payload too large: %u B (max %d)", port, len,
				APP_LRW_REQUEST_BUF_SIZE);
		}
	}

	k_work_submit_to_queue(&m_work_q, &m_downlink_success_work);
}

/* Approximate operational cell window for the DevStatusAns battery level.
 * Tune to the actual cell; values outside are clamped. */
#define BATTERY_EMPTY_V 2.4f
#define BATTERY_FULL_V  3.6f

static uint8_t battery_level_callback(void)
{
	/* LoRaWAN DevStatusAns battery level: 0 = external power, 1..254 = battery
	 * (1 ~ empty, 254 ~ full), 255 = unable to measure. Map the measured cell
	 * voltage linearly over the operational window. */
	float v;

	if (app_battery_measure(&v) != 0) {
		return 255; /* can't measure */
	}

	float frac = CLAMP((v - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V), 0.0f, 1.0f);

	return (uint8_t)(1 + (int)(frac * 253.0f + 0.5f)); /* 1..254 */
}

static void datarate_changed_callback(enum lorawan_datarate dr)
{
	uint8_t max_next = 0, max_now = 0;

	lorawan_get_payload_sizes(&max_next, &max_now);
	m_current_dr = dr;
	m_max_next_payload = max_next;
	LOG_INF("New data rate: DR%d, Maximum payload size: %d", dr, max_now);
}

static void link_check_callback(uint8_t demod_margin, uint8_t nb_gateways)
{
	LOG_INF("Link check response: margin=%d dB, gateways=%d", demod_margin, nb_gateways);

	m_last_margin = demod_margin;
	m_last_gw_count = nb_gateways;
	m_lc_response_gw_count = nb_gateways;

	k_work_submit_to_queue(&m_work_q, &m_lc_response_work);
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

	int ret = lorawan_set_channels_mask(mask, ARRAY_SIZE(mask));

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lorawan_set_channels_mask", ret);
		return ret;
	}

	LOG_INF("Applied sub-band %d", sub_band);
	return 0;
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

void app_lrw_suspend(void)
{
	/* Stop every LRW timer so nothing re-arms the radio after this point; the
	 * caller is about to power the MCU off (deep sleep). Pending works on
	 * m_work_q are simply abandoned — they cannot run once the system is shut
	 * down, and wake is a clean boot. (The report-cadence timer lives in
	 * app_report now; app_power_suspend stops it via app_report_suspend.) */
	k_timer_stop(&m_lc_timeout_timer);
	k_timer_stop(&m_rejoin_timer);
}

#if defined(CONFIG_WATCHDOG)
static void heartbeat_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	app_wdog_ping(m_wdog_channel);
	k_work_schedule_for_queue(&m_work_q, &m_heartbeat_work,
				  K_SECONDS(LRW_HEARTBEAT_PERIOD_SEC));
}
#endif /* defined(CONFIG_WATCHDOG) */

int app_lrw_init(void)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(lora0));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	/* #175: when the device is unprovisioned (#98 radio-silent) skip the entire
	 * LoRaMac/radio bring-up. The work queue, works and timers below are still set
	 * up so the public API stays safe (app_lrw_join / send hit the DISABLED guard
	 * and no-op), but clear_stale_lorawan_nvm() / lorawan_set_region() /
	 * lorawan_start() are never called — the SubGHz radio is never powered, so
	 * there is no boot radio burst. Provisioning is activation-aware (C-1): OTAA
	 * needs a DevEUI, ABP needs a DevAddr. */
	const bool radio_silent = lrw_unprovisioned();

	if (!radio_silent) {
		clear_stale_lorawan_nvm();

		enum lorawan_region region;

		switch (g_app_config.lrw_region) {
		case APP_CONFIG_LRW_REGION_EU868:
			region = LORAWAN_REGION_EU868;
			break;
		case APP_CONFIG_LRW_REGION_US915:
			region = LORAWAN_REGION_US915;
			break;
		case APP_CONFIG_LRW_REGION_AU915:
			region = LORAWAN_REGION_AU915;
			break;
		default:
			LOG_ERR("Invalid region: %d", g_app_config.lrw_region);
			return -EINVAL;
		}

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

		if (g_app_config.lrw_region == APP_CONFIG_LRW_REGION_US915 ||
		    g_app_config.lrw_region == APP_CONFIG_LRW_REGION_AU915) {
			ret = apply_subband(g_app_config.lrw_sub_band);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("apply_subband", ret);
				return ret;
			}
		}

		static struct lorawan_downlink_cb downlink_cb = {
			.port = LW_RECV_PORT_ANY,
			.cb = downlink_callback,
		};

		lorawan_register_downlink_callback(&downlink_cb);
		lorawan_register_battery_level_callback(battery_level_callback);
		lorawan_register_dr_changed_callback(datarate_changed_callback);
		lorawan_register_link_check_ans_callback(link_check_callback);
	} else {
		LOG_WRN("DevEUI is all-zero: skipping LoRaWAN bring-up (radio-silent, #98/#175)");
	}

	k_work_queue_init(&m_work_q);
	k_work_queue_start(&m_work_q, m_work_stack, K_THREAD_STACK_SIZEOF(m_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_work_init(&m_join_work, join_work_handler);
	k_work_init(&m_send_work, send_work_handler);
	k_work_init_delayable(&m_frame_work, frame_work_handler);
	k_work_init_delayable(&m_hist_work, m_hist_work_handler);
	k_work_init(&m_link_check_work, link_check_work_handler);
	k_work_init(&m_downlink_success_work, downlink_success_work_handler);
	k_work_init(&m_clock_sync_info_work, clock_sync_info_work_handler);
	k_work_init(&m_lc_response_work, lc_response_work_handler);
	k_work_init(&m_force_lc_work, force_lc_work_handler);
	k_work_init(&m_dl_request_work, dl_request_work_handler);
	k_work_init_delayable(&m_post_cmd_work, post_cmd_work_handler);
	k_work_init_delayable(&m_join_complete_work, join_complete_work_handler);
	k_work_init_delayable(&m_tx_retry_work, tx_retry_work_handler);
#if defined(CONFIG_SHELL)
	k_work_init(&m_dbg_lc_work, dbg_lc_work_handler);
#endif

	k_timer_init(&m_lc_timeout_timer, lc_timeout_timer_handler, NULL);
	k_timer_init(&m_rejoin_timer, rejoin_timer_handler, NULL);

#if defined(CONFIG_WATCHDOG)
	/* Start the liveness heartbeat on m_work_q so a wedged queue is detected and
	 * the IWDG resets us (#181/#182). Registered even when radio-silent: the
	 * queue still runs and a wedge there should still recover. */
	m_wdog_channel = app_wdog_register(LRW_HEARTBEAT_TIMEOUT_MS);
	if (m_wdog_channel < 0) {
		LOG_ERR_CALL_FAILED_INT("app_wdog_register", m_wdog_channel);
	}
	k_work_init_delayable(&m_heartbeat_work, heartbeat_work_handler);
	k_work_schedule_for_queue(&m_work_q, &m_heartbeat_work, K_NO_WAIT);
#endif /* defined(CONFIG_WATCHDOG) */

	atomic_set(&m_state, radio_silent ? APP_LRW_STATE_DISABLED : APP_LRW_STATE_IDLE);
	m_init_join = true;

	return 0;
}

void app_lrw_join(void)
{
	k_work_submit_to_queue(&m_work_q, &m_join_work);
}

void app_lrw_send_telemetry(void)
{
	/* Compose + split + send a telemetry snapshot from the current sensor data.
	 * Runs on m_work_q; send_work_handler drains response/alarm first, then falls
	 * through to the telemetry compose when both queues are empty. */
	k_work_submit_to_queue(&m_work_q, &m_send_work);
}

void app_lrw_register_ready_cb(void (*cb)(void))
{
	m_ready_cb = cb;
}

/* Arm a forced LinkCheckReq on the next telemetry first-frame (shell/test path;
 * pair with app_report_trigger() to actually emit the uplink). */
static void force_lc_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	m_force_lc_remaining = 1;
}

void app_lrw_force_link_check(void)
{
	k_work_submit_to_queue(&m_work_q, &m_force_lc_work);
}

enum app_lrw_state app_lrw_get_state(void)
{
	return (enum app_lrw_state)atomic_get(&m_state);
}

bool app_lrw_is_ready(void)
{
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	return state == APP_LRW_STATE_HEALTHY || state == APP_LRW_STATE_WARNING;
}

int app_lrw_get_info(struct app_lrw_info *info)
{
	MibRequestConfirm_t mib_req;

	if (!info) {
		return -EINVAL;
	}

	info->state = (enum app_lrw_state)atomic_get(&m_state);

	mib_req.Type = MIB_DEV_ADDR;
	if (LoRaMacMibGetRequestConfirm(&mib_req) == LORAMAC_STATUS_OK) {
		info->dev_addr = mib_req.Param.DevAddr;
	} else {
		info->dev_addr = 0;
	}

	uint32_t fcnt_up;

	if (LoRaMacCryptoGetFCntUp(&fcnt_up) == LORAMAC_CRYPTO_SUCCESS) {
		info->fcnt_up = fcnt_up;
	} else {
		info->fcnt_up = 0;
	}

	info->datarate = m_current_dr;
	info->rssi = m_last_rssi;
	info->snr = m_last_snr;
	info->margin = m_last_margin;
	info->gw_count = m_last_gw_count;
	info->consecutive_lc_fail = m_consecutive_lc_fail;
	info->consecutive_lc_ok = m_consecutive_lc_ok;
	info->warning_lc_fail_total = m_warning_lc_fail_total;
	info->message_count = m_message_count;
	info->thresh_warning = FAIL_THRESHOLD_WARNING;
	info->thresh_healthy = OK_THRESHOLD_HEALTHY;
	info->thresh_reconnect = g_app_config.lrw_link_check_fail_rejoin;
	info->link_check_interval = g_app_config.lrw_link_check_interval;

	return 0;
}

int app_lrw_queue_response(uint8_t port, const uint8_t *buf, size_t len)
{
	if (!buf || len == 0) {
		return -EINVAL;
	}
	if (len > APP_LRW_RESPONSE_BUF_SIZE) {
		LOG_ERR("Response too large: %zu B (max %d)", len, APP_LRW_RESPONSE_BUF_SIZE);
		return -EMSGSIZE;
	}

	struct lrw_tx_msg msg;

	msg.port = port;
	msg.len = len;
	memcpy(msg.buf, buf, len);

	if (k_msgq_put(&m_response_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("Response queue full (port %u); dropping", port);
		return -ENOMEM;
	}

	/* Wake send path so the response leaves at the next jitter window. */
	k_work_submit_to_queue(&m_work_q, &m_send_work);
	return 0;
}

void app_lrw_send_info_on_clock_sync(void)
{
	/* Arm the deferred Info; downlink_callback sends it once LORAWAN_TIME_UPDATED
	 * arrives (the ClockSync command answer). */
	atomic_set_bit(&m_clock_sync_info_pending, 0);
}

int app_lrw_send_alarm(const uint8_t *buf, size_t len)
{
	if (!buf || len == 0) {
		return -EINVAL;
	}
	if (len > APP_LRW_RESPONSE_BUF_SIZE) {
		LOG_ERR("Alarm batch too large: %zu B (max %d)", len, APP_LRW_RESPONSE_BUF_SIZE);
		return -EMSGSIZE;
	}

	struct lrw_tx_msg msg;

	msg.port = APP_LRW_ALARM_PORT;
	msg.len = len;
	memcpy(msg.buf, buf, len);

	if (k_msgq_put(&m_alarm_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("Alarm queue full; dropping batch");
		return -ENOMEM;
	}

	k_work_submit_to_queue(&m_work_q, &m_send_work);
	return 0;
}

int app_lrw_reset_nvm(void)
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
