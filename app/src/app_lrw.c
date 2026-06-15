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
#include "app_lrw.h"
#include "app_sensor.h"
#include "app_settings.h"

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
 * Three independent timers, each with a single meaning (no more inferring a
 * timer's purpose from the current state):
 *   - m_send_timer        : periodic report cadence
 *   - m_lc_timeout_timer  : link-check response timeout only
 *   - m_rejoin_timer      : rejoin backoff only
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

static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
static struct k_work_q m_work_q;

/* --- Timers (each one meaning only) --- */
static struct k_timer m_send_timer;
static struct k_timer m_lc_timeout_timer;
static struct k_timer m_rejoin_timer;

/* --- Works --- */
static struct k_work m_send_work;
static struct k_work_delayable m_frame_work; /* multi-frame snapshot continuation */
static struct k_work m_join_work;
static struct k_work m_link_check_work;       /* LC timeout (from m_lc_timeout_timer) */
static struct k_work m_downlink_success_work; /* deferred from downlink_callback */
static struct k_work m_lc_response_work;      /* deferred from link_check_callback */
static struct k_work m_send_with_lc_work;
static struct k_work m_dl_request_work; /* drains m_dl_msgq (port-85 commands) */
static struct k_work_delayable m_post_cmd_work;
static struct k_work_delayable m_join_complete_work;
static struct k_work_delayable m_hist_work;

/* Multi-frame telemetry: gap before the next frame of the same snapshot (covers
 * RX1/RX2 windows) and backoff when a send is refused (duty cycle / MAC busy). */
#define FRAME_GAP_SEC   3
#define FRAME_RETRY_SEC 15

static uint8_t m_frame_buf[64];
static size_t m_frame_len;
static bool m_frame_more;
static bool m_frame_first;
static bool m_frame_resend;

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
#define APP_LRW_REQUEST_BUF_SIZE  64
#define APP_LRW_DOWNLINK_CMD_PORT 85
#define APP_LRW_ALARM_PORT        3
#define APP_LRW_TX_QUEUE_DEPTH    4
#define APP_LRW_DL_QUEUE_DEPTH    4

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
static bool m_clock_sync_info_pending;

/* Forward declarations */
static void on_join_success(void);
static void on_join_failure(void);
static void on_lc_success(void);
static void on_lc_failure(void);
static void on_lc_timeout(void);
static void on_downlink_received(void);
static void state_transition(enum app_lrw_state new_state);
static bool should_request_link_check(void);
static void send_with_lc_work_handler(struct k_work *work);

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
		k_timer_stop(&m_send_timer);
		k_timer_stop(&m_lc_timeout_timer);
		k_timer_stop(&m_rejoin_timer);
		break;

	case APP_LRW_STATE_JOINING:
		k_timer_stop(&m_send_timer);
		m_hist_active = false; /* drop any in-flight history replay across (re)join */
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
	uint8_t info_buf[APP_LRW_RESPONSE_BUF_SIZE];
	size_t info_len;
	if (app_cmd_build_info(info_buf, sizeof(info_buf), &info_len) == 0) {
		(void)app_lrw_queue_response(APP_LRW_DOWNLINK_CMD_PORT, info_buf, info_len);
	} else {
		LOG_WRN("app_cmd_build_info failed; skipping GetInfo-on-join");
	}

	/* Send first message immediately after join (with LC). */
	k_work_submit_to_queue(&m_work_q, &m_send_work);
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
		app_settings_factory_reset();
		break;
	case APP_CMD_ACTION_REBOOT:
		LOG_INF("Command: reboot");
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case APP_CMD_ACTION_ALARM_RULES_SAVE:
		/* Persist the alarm-rule blob, no reboot. Deferred here (off the
		 * command-handle stack frame) because settings_save_one is too
		 * stack-heavy to run inline on the m_work_q. */
		LOG_INF("Command: saving alarm rules");
		app_alarm_rules_save();
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

static void join_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int ret;

	/* MED-10: ignore re-entry while a join is already in progress. */
	if ((enum app_lrw_state)atomic_get(&m_state) == APP_LRW_STATE_JOINING) {
		LOG_WRN("Join already in progress, ignoring request");
		return;
	}

	state_transition(APP_LRW_STATE_JOINING); /* stops send timer, drops history */

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
			k_timer_start(&m_send_timer, K_SECONDS(g_app_config.interval_report),
				      K_FOREVER);
			return;
		}
		m_frame_first = first_frame;
	}

	bool with_link_check =
		m_frame_first && !m_link_check_pending && should_request_link_check();

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
		/* Likely duty-cycle / MAC busy — retry the same frame shortly. */
		m_frame_resend = true;
		k_work_schedule_for_queue(&m_work_q, &m_frame_work, K_SECONDS(FRAME_RETRY_SEC));
		return;
	}

	m_frame_resend = false;
	m_message_count++;

	if (m_frame_more) {
		k_work_schedule_for_queue(&m_work_q, &m_frame_work, K_SECONDS(FRAME_GAP_SEC));
	} else {
		int timeout = g_app_config.interval_report;
#if defined(CONFIG_ENTROPY_GENERATOR)
		timeout += (int32_t)sys_rand32_get() % (g_app_config.interval_report / 10);
#endif
		LOG_INF("Snapshot complete; next report in %d s", timeout);
		k_timer_start(&m_send_timer, K_SECONDS(timeout), K_FOREVER);
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

static void send_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int ret;

	/* Block normal transmissions during calibration mode (flag-based). */
	if (g_app_config.calibration) {
		return;
	}

	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state == APP_LRW_STATE_JOINING || state == APP_LRW_STATE_RECONNECT) {
		LOG_WRN("TX blocked: %s", state_name(state));
		return;
	}

	struct lrw_tx_msg tx;

	/* Priority drain: command response (port 85) first, then alarm (port 3).
	 * One TX per call; if more are queued, re-submit. The periodic timer is
	 * only (re)started when nothing is queued AND no history replay owns it. */
	if (k_msgq_get(&m_response_msgq, &tx, K_NO_WAIT) == 0) {
		ret = lorawan_send(tx.port, tx.buf, tx.len, LORAWAN_MSG_UNCONFIRMED);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lorawan_send(response)", ret);
		} else {
			LOG_INF("Response sent on port %u (%u B)", tx.port, tx.len);
		}
		if (k_msgq_num_used_get(&m_response_msgq) || k_msgq_num_used_get(&m_alarm_msgq)) {
			k_work_submit_to_queue(&m_work_q, &m_send_work);
		} else if (!m_hist_active) {
			k_timer_start(&m_send_timer, K_SECONDS(g_app_config.interval_report),
				      K_FOREVER);
		}
		return;
	}

	if (k_msgq_get(&m_alarm_msgq, &tx, K_NO_WAIT) == 0) {
		ret = lorawan_send(APP_LRW_ALARM_PORT, tx.buf, tx.len, LORAWAN_MSG_UNCONFIRMED);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lorawan_send(alarm)", ret);
		} else {
			LOG_INF("Alarm batch sent on port %u (%u B)", APP_LRW_ALARM_PORT, tx.len);
		}
		if (k_msgq_num_used_get(&m_alarm_msgq)) {
			k_work_submit_to_queue(&m_work_q, &m_send_work);
		} else if (!m_hist_active) {
			k_timer_start(&m_send_timer, K_SECONDS(g_app_config.interval_report),
				      K_FOREVER);
		}
		return;
	}

	/* MED-9: history replay owns the send cadence; don't inject telemetry. */
	if (m_hist_active) {
		return;
	}

	if (!g_app_config.interval_sample) {
		app_sensor_sample();
	}
	app_history_capture();

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
	k_timer_start(&m_send_timer, K_SECONDS(g_app_config.interval_report), K_FOREVER);
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

	k_timer_stop(&m_send_timer); /* replay owns the cadence */
	LOG_INF("History replay start: %u frames (window %u..%u)", (unsigned)n, from_unix, to_unix);
	k_work_schedule_for_queue(&m_work_q, &m_hist_work, K_NO_WAIT);
	return true;
}

/* ======================================================================== */
/* Timer ISR handlers (thin: enqueue the right work, no state decisions)    */
/* ======================================================================== */

static void send_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit_to_queue(&m_work_q, &m_send_work);
}

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
	 * carry the synced time yet). */
	if (m_clock_sync_info_pending && (flags & LORAWAN_TIME_UPDATED)) {
		m_clock_sync_info_pending = false;
		uint8_t info_buf[APP_LRW_RESPONSE_BUF_SIZE];
		size_t info_len;
		if (app_cmd_build_info(info_buf, sizeof(info_buf), &info_len) == 0) {
			(void)app_lrw_queue_response(APP_LRW_DOWNLINK_CMD_PORT, info_buf, info_len);
		}
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

int app_lrw_init(void)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(lora0));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

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

	k_work_queue_init(&m_work_q);
	k_work_queue_start(&m_work_q, m_work_stack, K_THREAD_STACK_SIZEOF(m_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_work_init(&m_join_work, join_work_handler);
	k_work_init(&m_send_work, send_work_handler);
	k_work_init_delayable(&m_frame_work, frame_work_handler);
	k_work_init_delayable(&m_hist_work, m_hist_work_handler);
	k_work_init(&m_link_check_work, link_check_work_handler);
	k_work_init(&m_downlink_success_work, downlink_success_work_handler);
	k_work_init(&m_lc_response_work, lc_response_work_handler);
	k_work_init(&m_send_with_lc_work, send_with_lc_work_handler);
	k_work_init(&m_dl_request_work, dl_request_work_handler);
	k_work_init_delayable(&m_post_cmd_work, post_cmd_work_handler);
	k_work_init_delayable(&m_join_complete_work, join_complete_work_handler);
#if defined(CONFIG_SHELL)
	k_work_init(&m_dbg_lc_work, dbg_lc_work_handler);
#endif

	k_timer_init(&m_send_timer, send_timer_handler, NULL);
	k_timer_init(&m_lc_timeout_timer, lc_timeout_timer_handler, NULL);
	k_timer_init(&m_rejoin_timer, rejoin_timer_handler, NULL);

	atomic_set(&m_state, APP_LRW_STATE_IDLE);
	m_init_join = true;

	return 0;
}

void app_lrw_join(void)
{
	k_work_submit_to_queue(&m_work_q, &m_join_work);
}

void app_lrw_send(void)
{
	k_work_submit_to_queue(&m_work_q, &m_send_work);
}

static void send_with_lc_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state != APP_LRW_STATE_HEALTHY && state != APP_LRW_STATE_WARNING) {
		LOG_WRN("Cannot send with link check in %s", state_name(state));
		return;
	}

	int ret = lorawan_request_link_check(false);

	if (ret) {
		LOG_ERR("Link check request failed: %d", ret);
		m_force_lc_remaining = 1; /* retry LC on the next message */
	} else {
		m_link_check_pending = true;
		k_timer_start(&m_lc_timeout_timer, K_SECONDS(LINK_CHECK_TIMEOUT_SEC), K_FOREVER);
		LOG_INF("Link check requested, timeout in %d s", LINK_CHECK_TIMEOUT_SEC);
	}

	k_work_submit_to_queue(&m_work_q, &m_send_work);
}

void app_lrw_send_with_link_check(void)
{
	k_work_submit_to_queue(&m_work_q, &m_send_with_lc_work);
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
	m_clock_sync_info_pending = true;
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
