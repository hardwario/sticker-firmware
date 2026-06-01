/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_calibration.h"
#include "app_clock.h"
#include "app_cmd.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_led.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_sensor.h"

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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_lrw, LOG_LEVEL_DBG);

/* Link check configuration constants */
#define LINK_CHECK_INTERVAL    5   /* Every N-th message has LC (0 = disabled) */
#define LINK_CHECK_TIMEOUT_SEC 10  /* Timeout for response */

/* State machine thresholds  */
#define FAIL_THRESHOLD_WARNING   3  /* LC failures to enter WARNING */
#define FAIL_THRESHOLD_RECONNECT 5  /* LC failures in WARNING to enter RECONNECT */
#define OK_THRESHOLD_HEALTHY     1  /* LC successes in WARNING to return to HEALTHY */

/* Join/Rejoin backoff configuration - easily adjustable */
#define REJOIN_BACKOFF_BASE_SEC  60   /* Base backoff time in seconds */
#define REJOIN_BACKOFF_MAX_SEC   3600 /* Maximum backoff time (1 hour) */
#define REJOIN_BACKOFF_MULTIPLIER 2   /* Exponential multiplier per attempt */

static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
static struct k_work_q m_work_q;
static struct k_timer m_send_timer;
static struct k_work m_send_work;
static struct k_work m_join_work;

static atomic_t m_state = ATOMIC_INIT(APP_LRW_STATE_IDLE);
static struct k_timer m_link_check_timer;
static struct k_work m_link_check_work;
static int m_consecutive_lc_fail;          /* LC failures in a row (HEALTHY) */
static int m_consecutive_lc_ok;            /* LC successes in a row (WARNING) */
static int m_warning_lc_fail_total;        /* Total LC failures in WARNING */
static int m_force_lc_remaining;           /* Remaining forced LC messages */
static bool m_link_check_pending;          /* Waiting for LC response */
static int m_message_count;                /* Message counter for N-th LC */
static int m_rejoin_attempts;              /* Rejoin attempt counter for backoff */
static int m_join_busy_polls;              /* Counter for MAC busy polling */
static bool m_init_join;                   /* True for first join after boot */

static struct k_work_delayable m_join_complete_work;

#define JOIN_BUSY_POLL_INTERVAL_MS  500
#define JOIN_BUSY_MAX_POLLS         30

static int m_current_dr;
/* Application-payload budget (bytes) for the next TX, sourced from the LoRaWAN
 * stack (lorawan_get_payload_sizes); 0 until the first DR callback / join. */
static uint8_t m_max_next_payload;
static int16_t m_last_rssi;
static int8_t m_last_snr;
static uint8_t m_last_margin;
static uint8_t m_last_gw_count;

static void handle_link_check_failure(void);
static void handle_link_check_success(void);
static void restart_normal_operation(void);
static void join_complete_work_handler(struct k_work *work);
static void send_with_lc_work_handler(struct k_work *work);

static struct k_work m_downlink_success_work;
static struct k_work m_lc_response_work;
static struct k_work m_send_with_lc_work;
static uint8_t m_lc_response_gw_count;

/* Pending response staged by app_lrw_queue_response(). send_work_handler()
 * drains this slot first. Single slot: a new queue call before the previous
 * response leaves overwrites with a warning. */
#define APP_LRW_RESPONSE_BUF_SIZE 64
static uint8_t m_pending_response_buf[APP_LRW_RESPONSE_BUF_SIZE];
static size_t  m_pending_response_len;
static uint8_t m_pending_response_port;
static K_MUTEX_DEFINE(m_pending_response_lock);

/* Inbound downlink request on port 85 captured from downlink_callback().
 * The callback runs on the LoRaMac stack (restricted), so it only copies the
 * payload and defers parsing to m_dl_request_work on m_work_q. */
#define APP_LRW_REQUEST_BUF_SIZE 64
#define APP_LRW_DOWNLINK_CMD_PORT 85
static uint8_t m_dl_request_buf[APP_LRW_REQUEST_BUF_SIZE];
static size_t  m_dl_request_len;
static struct k_work m_dl_request_work;


static uint32_t calculate_rejoin_backoff(int attempt)
{
	uint32_t backoff = REJOIN_BACKOFF_BASE_SEC;

	/* Calculate exponential backoff */
	for (int i = 0; i < attempt; i++) {
		backoff *= REJOIN_BACKOFF_MULTIPLIER;
		if (backoff >= REJOIN_BACKOFF_MAX_SEC) {
			return REJOIN_BACKOFF_MAX_SEC;
		}
	}
	return backoff;
}

static void downlink_success_work_handler(struct k_work *work)
{
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state == APP_LRW_STATE_HEALTHY || state == APP_LRW_STATE_WARNING) {
		if (m_link_check_pending) {
			k_timer_stop(&m_link_check_timer);
			handle_link_check_success();
			LOG_INF("Connection confirmed via downlink (LC pending)");
		} else {
			LOG_INF("Downlink received (no LC pending)");
		}
	}
}

static void dl_request_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	static uint8_t resp[APP_LRW_RESPONSE_BUF_SIZE];
	size_t resp_len = 0;

	int ret = app_cmd_handle(APP_CMD_TRANSPORT_LRW, m_dl_request_buf, m_dl_request_len,
				 resp, sizeof(resp), &resp_len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_cmd_handle", ret);
		return;
	}

	if (resp_len) {
		ret = app_lrw_queue_response(APP_LRW_DOWNLINK_CMD_PORT, resp, resp_len);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_lrw_queue_response", ret);
		}
	}

	/* Diagnostic: stack headroom after the heaviest work this queue runs.
	 * Helps decide if 2048 B m_work_stack needs growing as more handlers
	 * land in later phases. Only built when stack info is enabled (debug). */
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	size_t unused;
	if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
		LOG_INF("m_work_q stack: %zu B unused after cmd handle", unused);
	}
#endif
}

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

	if (port == APP_LRW_DOWNLINK_CMD_PORT && data && len > 0) {
		if (len <= sizeof(m_dl_request_buf)) {
			memcpy(m_dl_request_buf, data, len);
			m_dl_request_len = len;
			k_work_submit_to_queue(&m_work_q, &m_dl_request_work);
		} else {
			LOG_WRN("Port %u payload too large: %u B (max %zu)",
				port, len, sizeof(m_dl_request_buf));
		}
	}

	k_work_submit_to_queue(&m_work_q, &m_downlink_success_work);
}

static uint8_t battery_level_callback(void)
{
	/* TODO Implement */
	return 255;
}

static void datarate_changed_callback(enum lorawan_datarate dr)
{
	uint8_t max_next_payload_size;
	uint8_t max_payload_size;

	lorawan_get_payload_sizes(&max_next_payload_size, &max_payload_size);
	m_current_dr = dr;
	m_max_next_payload = max_next_payload_size;
	LOG_INF("New data rate: DR%d, Maximum payload size: %d", dr, max_payload_size);
}

uint8_t app_lrw_get_max_payload(void)
{
	return m_max_next_payload;
}

static void join_complete_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Verify we're still in JOINING state (might have changed) */
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state != APP_LRW_STATE_JOINING) {
		LOG_DBG("Join complete handler: state changed to %d, ignoring", (int)state);
		return;
	}

	/* Check if MAC layer is still busy (join request in progress) */
	if (LoRaMacIsBusy()) {
		m_join_busy_polls++;

		/* Check for timeout (10 seconds) */
		if (m_join_busy_polls >= JOIN_BUSY_MAX_POLLS) {
			LOG_ERR("MAC busy timeout after %d ms - triggering reconnect",
				JOIN_BUSY_MAX_POLLS * JOIN_BUSY_POLL_INTERVAL_MS);
			atomic_set(&m_state, APP_LRW_STATE_RECONNECT);
			m_rejoin_attempts = 0;
			k_timer_start(&m_link_check_timer,
				      K_SECONDS(REJOIN_BACKOFF_BASE_SEC), K_FOREVER);
			return;
		}

		/* Log only every 10th poll (5 seconds) to reduce noise */
		if ((m_join_busy_polls % 10) == 0) {
			LOG_INF("MAC still busy (%d/%d)...",
				m_join_busy_polls, JOIN_BUSY_MAX_POLLS);
		}
		k_work_schedule_for_queue(&m_work_q, &m_join_complete_work,
					  K_MSEC(JOIN_BUSY_POLL_INTERVAL_MS));
		return;
	}

	/* MAC is ready - verify join actually succeeded via network activation status */
	MibRequestConfirm_t mib_req;
	mib_req.Type = MIB_NETWORK_ACTIVATION;
	if (LoRaMacMibGetRequestConfirm(&mib_req) != LORAMAC_STATUS_OK ||
	    mib_req.Param.NetworkActivation == ACTIVATION_TYPE_NONE) {
		/* Join failed - not activated */
		uint32_t backoff = calculate_rejoin_backoff(m_rejoin_attempts);
		LOG_ERR("Join failed (not activated), retry in %u seconds", backoff);
		m_rejoin_attempts++;
		atomic_set(&m_state, APP_LRW_STATE_RECONNECT);
		k_timer_start(&m_link_check_timer, K_SECONDS(backoff), K_FOREVER);
		return;
	}

	/* Join succeeded - transition to HEALTHY */
	LOG_INF("Join successful - transitioning to HEALTHY");
	m_init_join = false;  /* Next join will be rejoin with MAC reset */
	lorawan_enable_adr(g_app_config.lrw_adr);

	/* Capture the initial DR's payload budget; the DR-changed callback may not
	 * fire on join, so query it explicitly here. */
	{
		uint8_t max_next, max_now;
		lorawan_get_payload_sizes(&max_next, &max_now);
		m_max_next_payload = max_next;
	}
	atomic_set(&m_state, APP_LRW_STATE_HEALTHY);
	m_rejoin_attempts = 0;

	/* Request network time once joined; the answer sets the RTC asynchronously
	 * in downlink_callback(). */
	app_clock_request_sync();

	restart_normal_operation();
}

static void handle_link_check_failure(void)
{
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	m_link_check_pending = false;
	m_consecutive_lc_ok = 0; /* Reset OK streak on any failure */

	switch (state) {
	case APP_LRW_STATE_HEALTHY:
		m_consecutive_lc_fail++;
		LOG_WRN("LC FAIL in HEALTHY (streak: %d/%d)",
			m_consecutive_lc_fail, FAIL_THRESHOLD_WARNING);

		if (m_consecutive_lc_fail >= FAIL_THRESHOLD_WARNING) {
			LOG_WRN("Entering WARNING state");
			atomic_set(&m_state, APP_LRW_STATE_WARNING);
			m_consecutive_lc_fail = 0;
			m_warning_lc_fail_total = 0;
			m_force_lc_remaining = 0; /* WARNING uses normal N-th interval */
		}
		/* No force LC in HEALTHY - wait for normal N-th interval */
		break;

	case APP_LRW_STATE_WARNING:
		m_warning_lc_fail_total++;
		LOG_WRN("LC FAIL in WARNING (total: %d/%d)",
			m_warning_lc_fail_total, FAIL_THRESHOLD_RECONNECT);

		if (m_warning_lc_fail_total >= FAIL_THRESHOLD_RECONNECT) {
			/* Only OTAA can reconnect */
			if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_OTAA) {
				LOG_ERR("Entering RECONNECT state - will rejoin in 5 seconds");
				atomic_set(&m_state, APP_LRW_STATE_RECONNECT);
				m_warning_lc_fail_total = 0;
				m_rejoin_attempts = 0; /* Reset backoff counter */
				/* Schedule first rejoin after 5 seconds (non-blocking) */
				k_timer_start(&m_link_check_timer, K_SECONDS(5), K_FOREVER);
				return;
			}
			/* ABP: stay in WARNING, reset counter */
			LOG_WRN("ABP mode - cannot rejoin, staying in WARNING");
			m_warning_lc_fail_total = 0;
		}
		/* WARNING: NO force, use normal N-th interval */
		break;

	default:
		LOG_WRN("LC FAIL in state %d (ignored)", (int)state);
		return;
	}
}

static void restart_normal_operation(void)
{
	/* Reset all state machine counters */
	m_consecutive_lc_fail = 0;
	m_consecutive_lc_ok = 0;
	m_warning_lc_fail_total = 0;
	m_force_lc_remaining = 0;
	m_message_count = 0;
	m_rejoin_attempts = 0;

	/* Send first message immediately after join/rejoin (with LC) */
	k_work_submit_to_queue(&m_work_q, &m_send_work);
}

static void handle_link_check_success(void)
{
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	m_link_check_pending = false;
	m_consecutive_lc_fail = 0; /* Reset fail streak on any success */

	switch (state) {
	case APP_LRW_STATE_HEALTHY:
		LOG_INF("LC OK in HEALTHY");
		/* Recovery successful, back to normal N-th interval */
		m_force_lc_remaining = 0;
		break;

	case APP_LRW_STATE_WARNING:
		m_consecutive_lc_ok++;
		LOG_INF("LC OK in WARNING (streak: %d/%d)",
			m_consecutive_lc_ok, OK_THRESHOLD_HEALTHY);

		if (m_consecutive_lc_ok >= OK_THRESHOLD_HEALTHY) {
			LOG_INF("Returning to HEALTHY state");
			atomic_set(&m_state, APP_LRW_STATE_HEALTHY);
			m_consecutive_lc_ok = 0;
			m_warning_lc_fail_total = 0;
			m_force_lc_remaining = 0;
		} else {
			/* Need more OKs, force LC on next message */
			m_force_lc_remaining = 1;
		}
		break;

	default:
		LOG_INF("LC OK in state %d", (int)state);
		break;
	}
}

static void lc_response_work_handler(struct k_work *work)
{
	/* Stop timeout timer */
	k_timer_stop(&m_link_check_timer);

	if (m_lc_response_gw_count == 0) {
		handle_link_check_failure();
		return;
	}

	handle_link_check_success();
}

static void link_check_callback(uint8_t demod_margin, uint8_t nb_gateways)
{
	LOG_INF("Link check response: margin=%d dB, gateways=%d", demod_margin, nb_gateways);

	m_last_margin = demod_margin;
	m_last_gw_count = nb_gateways;
	m_lc_response_gw_count = nb_gateways;

	k_work_submit_to_queue(&m_work_q, &m_lc_response_work);
}

static void link_check_timeout_handler(struct k_timer *timer)
{
	if (atomic_get(&m_state) == APP_LRW_STATE_RECONNECT) {
		k_work_submit_to_queue(&m_work_q, &m_join_work);
	} else {
		k_work_submit_to_queue(&m_work_q, &m_link_check_work);
	}
}

static void link_check_work_handler(struct k_work *work)
{
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state == APP_LRW_STATE_JOINING || state == APP_LRW_STATE_RECONNECT) {
		return;
	}

	if (m_link_check_pending) {
		LOG_WRN("Link check timeout - no response received");
		handle_link_check_failure();
		return;
	}

	/* Not pending — LC response already arrived and was handled before
	 * the timeout timer fired. Nothing to do. */
	LOG_DBG("Link check timeout fired but LC already resolved");
}

static void join_work_handler(struct k_work *work)
{
	int ret;

	/* Stop send timer to prevent TX during join */
	k_timer_stop(&m_send_timer);
	atomic_set(&m_state, APP_LRW_STATE_JOINING);

	if (m_init_join) {
		LOG_INF("Initial join after boot");
	} else {
		LOG_INF("Rejoin attempt %d...", m_rejoin_attempts + 1);

		/* ABP doesn't need rejoin - just restart normal operation */
		if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_ABP) {
			LOG_INF("ABP mode - rejoin not applicable");
			atomic_set(&m_state, APP_LRW_STATE_HEALTHY);
			restart_normal_operation();
			return;
		}

		LOG_INF("Deinitializing MAC...");
		LoRaMacDeInitialization();

		ret = lorawan_start();
		if (ret) {
			LOG_ERR("lorawan_start failed: %d", ret);
			uint32_t backoff = calculate_rejoin_backoff(m_rejoin_attempts);
			m_rejoin_attempts++;
			atomic_set(&m_state, APP_LRW_STATE_RECONNECT);
			k_timer_start(&m_link_check_timer, K_SECONDS(backoff), K_FOREVER);
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
		/* LoRaWAN 1.1.x: NwkKey and AppKey are distinct root keys. */
		config.otaa.nwk_key = g_app_config.lrw_nwkkey;
#else
		/* LoRaWAN 1.0.x: NwkKey == AppKey. LoRaMac computes the
		 * JoinRequest MIC from the NwkKey slot, so feeding AppKey into
		 * both slots keeps the device compatible with 1.0.x network
		 * servers (TTN, ChirpStack, Helium). */
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
		atomic_set(&m_state, APP_LRW_STATE_IDLE);
		return;
	}

	/* Reset counter before lorawan_join() */
	m_join_busy_polls = 0;

	ret = lorawan_join(&config);
	if (ret && ret != -ETIMEDOUT) {
		/* Hard error (not ETIMEDOUT) - retry with backoff */
		uint32_t backoff = calculate_rejoin_backoff(m_rejoin_attempts);
		LOG_ERR("Join failed: %d, will retry in %u seconds (attempt %d)",
			ret, backoff, m_rejoin_attempts + 1);
		m_rejoin_attempts++;
		atomic_set(&m_state, APP_LRW_STATE_RECONNECT);
		k_timer_start(&m_link_check_timer, K_SECONDS(backoff), K_FOREVER);
		return;
	}

	/* For ABP, explicitly set RX delays to match network configuration. The
	 * LNS round-trip is fast (~170 ms on this gateway), so the LoRaWAN default
	 * 1s/2s windows are sufficient. Must match the LNS rx1_delay setting. */
	if (config.mode == LORAWAN_ACT_ABP) {
		MibRequestConfirm_t mib_req;

		mib_req.Type = MIB_RECEIVE_DELAY_1;
		mib_req.Param.ReceiveDelay1 = 1000; /* 1 second in ms */
		LoRaMacMibSetRequestConfirm(&mib_req);

		mib_req.Type = MIB_RECEIVE_DELAY_2;
		mib_req.Param.ReceiveDelay2 = 2000; /* 2 seconds in ms */
		LoRaMacMibSetRequestConfirm(&mib_req);

		LOG_INF("RX delays set: RX1=1s, RX2=2s");
	}

	/* Switch to private sync word only when explicitly configured.
	 * Default (and any unknown value) leaves Zephyr's MIB_PUBLIC_NETWORK setting intact,
	 * which is public (0x34) per CONFIG_LORAWAN_PUBLIC_NETWORK. This way the radio always
	 * prefers public unless the user opts into private via `config lrw-network private`. */
	if (g_app_config.lrw_network == APP_CONFIG_LRW_NETWORK_PRIVATE) {
		MibRequestConfirm_t mib_req;

		mib_req.Type = MIB_PUBLIC_NETWORK;
		mib_req.Param.EnablePublicNetwork = false;
		LoRaMacMibSetRequestConfirm(&mib_req);

		LOG_INF("Network type: private (sync word 0x12)");
	}

	LOG_INF("lorawan_join() ret=%d, polling MAC...", ret);
	k_work_schedule_for_queue(&m_work_q, &m_join_complete_work,
				  K_MSEC(JOIN_BUSY_POLL_INTERVAL_MS));
}

static bool should_request_link_check(void)
{
	/* Disabled if LINK_CHECK_INTERVAL = 0 */
	if (LINK_CHECK_INTERVAL == 0) {
		return false;
	}

	/* Force LC for recovery attempts */
	if (m_force_lc_remaining > 0) {
		m_force_lc_remaining--;
		return true;
	}

	/* Message number to be sent (1-based) */
	int msg_num = m_message_count + 1;

	/* First message always has LC, then every N-th (5, 10, 15...) */
	if (msg_num == 1 || (msg_num % LINK_CHECK_INTERVAL) == 0) {
		return true;
	}

	return false;
}

static void send_work_handler(struct k_work *work)
{
	int ret;
	bool with_link_check;

	/* Block normal transmissions during calibration mode */
	if (g_app_config.calibration) {
		return;
	}

	/* Block transmissions during joining or reconnect */
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state == APP_LRW_STATE_JOINING || state == APP_LRW_STATE_RECONNECT) {
		LOG_WRN("TX blocked: state=%d", (int)state);
		return;
	}

	/* Drain pending command response before composing telemetry. The
	 * response leaves at the next jitter window; regular telemetry resumes
	 * with the next periodic timer fire. */
	k_mutex_lock(&m_pending_response_lock, K_FOREVER);
	if (m_pending_response_len) {
		uint8_t buf[APP_LRW_RESPONSE_BUF_SIZE];
		size_t  len  = m_pending_response_len;
		uint8_t port = m_pending_response_port;
		memcpy(buf, m_pending_response_buf, len);
		m_pending_response_len = 0;
		k_mutex_unlock(&m_pending_response_lock);

		ret = lorawan_send(port, buf, len, LORAWAN_MSG_UNCONFIRMED);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lorawan_send(response)", ret);
		} else {
			LOG_INF("Response sent on port %u (%zu B)", port, len);
		}
		k_timer_start(&m_send_timer,
			      K_SECONDS(g_app_config.interval_report), K_FOREVER);
		return;
	}
	k_mutex_unlock(&m_pending_response_lock);

	int timeout = g_app_config.interval_report;

#if defined(CONFIG_ENTROPY_GENERATOR)
	timeout += (int32_t)sys_rand32_get() % (g_app_config.interval_report / 10);
#endif /* defined(CONFIG_ENTROPY_GENERATOR) */

	LOG_INF("Scheduling next timeout in %d seconds", timeout);

	k_timer_start(&m_send_timer, K_SECONDS(timeout), K_FOREVER);

	if (!g_app_config.interval_sample) {
		app_sensor_sample();
	}

	uint8_t buf[64];
	size_t len;
	ret = app_compose(buf, sizeof(buf), &len);
	if (ret == -EAGAIN) {
		/* Budget unknown (not joined yet) — skip this TX. */
		LOG_DBG("Telemetry budget unavailable, skipping TX");
		return;
	}
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_compose", ret);
		return;
	}

	/* Determine if this message should have link check.
	 * Skip if LC is already pending (e.g. from send_with_lc path). */
	with_link_check = !m_link_check_pending && should_request_link_check();

	if (with_link_check) {
		ret = lorawan_request_link_check(false);
		if (ret) {
			LOG_ERR("Link check request failed: %d", ret);
		} else {
			m_link_check_pending = true;
			/* Start timeout timer for response */
			k_timer_start(&m_link_check_timer,
				      K_SECONDS(LINK_CHECK_TIMEOUT_SEC), K_FOREVER);
			LOG_INF("Sending data with LC (msg #%u)...", m_message_count + 1);
		}
	} else {
		LOG_INF("Sending data (msg #%u)...", m_message_count + 1);
	}

	/* Protobuf Telemetry on fPort 2 (legacy bitmap used fPort 1). */
	ret = lorawan_send(2, buf, len, LORAWAN_MSG_UNCONFIRMED);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lorawan_send", ret);
		if (with_link_check) {
			m_link_check_pending = false;
			k_timer_stop(&m_link_check_timer);
		}
		return;
	}

	/* Increment message counter after successful send */
	m_message_count++;

	LOG_INF("Data sent");
}

static void send_timer_handler(struct k_timer *timer)
{
	k_work_submit_to_queue(&m_work_q, &m_send_work);
}

/* Clear the subset of Zephyr's LoRaWAN NVM keys (zephyr/subsys/lorawan/nvm/
 * lorawan_nvm_settings.c:35-43) that would otherwise let lorawan_nvm_data_restore()
 * inside lorawan_start() override the region we just set via lorawan_set_region().
 * Crypto (DevNonce monotonicity), MacGroup1, and SecureElement are kept so the
 * device does not look like a fresh provisioning to the network server.
 * NetworkActivation lives in MacGroup2 -> device joins fresh every boot, which
 * is the intended behaviour. */
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
			/* Continue - a leftover stale key is better than aborting boot. */
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
	k_work_init(&m_link_check_work, link_check_work_handler);
	k_work_init(&m_downlink_success_work, downlink_success_work_handler);
	k_work_init(&m_lc_response_work, lc_response_work_handler);
	k_work_init(&m_send_with_lc_work, send_with_lc_work_handler);
	k_work_init(&m_dl_request_work, dl_request_work_handler);
	k_work_init_delayable(&m_join_complete_work, join_complete_work_handler);

	k_timer_init(&m_send_timer, send_timer_handler, NULL);
	k_timer_init(&m_link_check_timer, link_check_timeout_handler, NULL);
	/* Don't start send timer here - wait for join completion via DR callback */

	atomic_set(&m_state, APP_LRW_STATE_IDLE);
	m_init_join = true;  /* First join after boot */

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
	enum app_lrw_state state = (enum app_lrw_state)atomic_get(&m_state);

	if (state != APP_LRW_STATE_HEALTHY && state != APP_LRW_STATE_WARNING) {
		LOG_WRN("Cannot send with link check in state %d", (int)state);
		return;
	}

	int ret = lorawan_request_link_check(false);
	if (ret) {
		LOG_ERR("Link check request failed: %d", ret);
		/* Ensure send_work_handler retries LC on this message */
		m_force_lc_remaining = 1;
	} else {
		m_link_check_pending = true;
		k_timer_start(&m_link_check_timer, K_SECONDS(LINK_CHECK_TIMEOUT_SEC), K_FOREVER);
		LOG_INF("Link check requested, timeout in %d seconds", LINK_CHECK_TIMEOUT_SEC);
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

	/* Get DevAddr from MIB */
	mib_req.Type = MIB_DEV_ADDR;
	if (LoRaMacMibGetRequestConfirm(&mib_req) == LORAMAC_STATUS_OK) {
		info->dev_addr = mib_req.Param.DevAddr;
	} else {
		info->dev_addr = 0;
	}

	/* Get FCntUp from crypto module */
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
	info->thresh_reconnect = FAIL_THRESHOLD_RECONNECT;
	info->link_check_interval = LINK_CHECK_INTERVAL;

	return 0;
}

int app_lrw_queue_response(uint8_t port, const uint8_t *buf, size_t len)
{
	if (!buf || len == 0) {
		return -EINVAL;
	}
	if (len > sizeof(m_pending_response_buf)) {
		LOG_ERR("Response too large: %zu B (max %zu)", len,
			sizeof(m_pending_response_buf));
		return -EMSGSIZE;
	}

	k_mutex_lock(&m_pending_response_lock, K_FOREVER);
	if (m_pending_response_len) {
		LOG_WRN("Overwriting unsent response on port %u (%zu B)",
			m_pending_response_port, m_pending_response_len);
	}
	memcpy(m_pending_response_buf, buf, len);
	m_pending_response_len = len;
	m_pending_response_port = port;
	k_mutex_unlock(&m_pending_response_lock);

	/* Wake send path so the response leaves at the next jitter window
	 * instead of waiting for the periodic timer. */
	k_work_submit_to_queue(&m_work_q, &m_send_work);
	return 0;
}

int app_lrw_reset_nvm(void)
{
	/* Keys mirror the LoRaWAN NVM subtree members in
	 * zephyr/subsys/lorawan/nvm/lorawan_nvm_settings.c. Deleting them clears
	 * frame counters (FCntUp/Down), DevNonce and the cached session; a reboot
	 * then makes the MAC re-init from a clean NVM. */
	static const char *const keys[] = {
		"lorawan/nvm/Crypto",       "lorawan/nvm/MacGroup1",
		"lorawan/nvm/MacGroup2",    "lorawan/nvm/SecureElement",
		"lorawan/nvm/RegionGroup1", "lorawan/nvm/RegionGroup2",
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
