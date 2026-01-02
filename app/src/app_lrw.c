/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
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
#include <zephyr/sys/byteorder.h>

/* LoRaMac includes for MIB access */
#include <LoRaMac.h>
#include <LoRaMacCrypto.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_lrw, LOG_LEVEL_DBG);

/* Link check configuration constants UPDATE TO 5 after DEBUG*/
#define LINK_CHECK_INTERVAL    2   //5/* Every N-th message has LC (0 = disabled) */
#define LINK_CHECK_TIMEOUT_SEC 10  /* Timeout for response */

/* State machine thresholds  */
#define FAIL_THRESHOLD_WARNING   3  /* LC failures to enter WARNING */
#define FAIL_THRESHOLD_RECONNECT 2 //5 /* LC failures in WARNING to enter RECONNECT */
#define OK_THRESHOLD_HEALTHY     2  /* LC successes in WARNING to return to HEALTHY */

/* Join/Rejoin backoff configuration - easily adjustable */
#define REJOIN_BACKOFF_BASE_SEC  60   /* Base backoff time in seconds */
#define REJOIN_BACKOFF_MAX_SEC   3600 /* Maximum backoff time (1 hour) */
#define REJOIN_BACKOFF_MULTIPLIER 2   /* Exponential multiplier per attempt */

static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
static struct k_work_q m_work_q;
static struct k_timer m_send_timer;
static struct k_work m_send_work;
static struct k_work m_join_work;

/* Link check state management */
static enum app_lrw_state m_state = APP_LRW_STATE_IDLE;
static struct k_timer m_link_check_timer;
static struct k_work m_link_check_work;
static struct k_work m_rejoin_work;

/* State machine counters (per join.md section 13.1) */
static uint8_t m_consecutive_lc_fail;      /* LC failures in a row (HEALTHY) */
static uint8_t m_consecutive_lc_ok;        /* LC successes in a row (WARNING) */
static uint8_t m_warning_lc_fail_total;    /* Total LC failures in WARNING */
static uint8_t m_force_lc_remaining;       /* Remaining forced LC messages */
static bool m_link_check_pending;          /* Waiting for LC response */
static uint8_t m_message_count;            /* Message counter for N-th LC */
static uint8_t m_rejoin_attempts;          /* Rejoin attempt counter for backoff */

/* Diagnostic data */
static int m_current_dr;
static int16_t m_last_rssi;
static int8_t m_last_snr;
static uint8_t m_last_margin;
static uint8_t m_last_gw_count;

/* Forward declarations */
static void handle_link_check_failure(void);
static void restart_normal_operation(void);

/**
 * @brief Calculate backoff time for rejoin attempts
 *
 * Simple exponential backoff: base * (multiplier ^ attempt)
 * Capped at maximum value. Easy to modify by changing constants above.
 *
 * @param attempt Current attempt number (0-based)
 * @return Backoff time in seconds
 */
static uint32_t calculate_rejoin_backoff(uint8_t attempt)
{
	uint32_t backoff = REJOIN_BACKOFF_BASE_SEC;

	/* Calculate exponential backoff */
	for (uint8_t i = 0; i < attempt; i++) {
		backoff *= REJOIN_BACKOFF_MULTIPLIER;
		if (backoff >= REJOIN_BACKOFF_MAX_SEC) {
			return REJOIN_BACKOFF_MAX_SEC;
		}
	}

	return backoff;
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

	LOG_INF("New data rate: DR%d", dr);
	LOG_INF("Maximum payload size: %d", max_payload_size);

	/*
	 * Detect async join completion:
	 * When lorawan_join() returns immediately (e.g., -116 EBUSY from previous
	 * pending operation), the join may complete asynchronously. This callback
	 * is triggered after successful join - check DevAddr to confirm completion.
	 */
	if (m_state == APP_LRW_STATE_JOINING || m_state == APP_LRW_STATE_RECONNECT) {
		MibRequestConfirm_t mib_req;
		mib_req.Type = MIB_DEV_ADDR;
		if (LoRaMacMibGetRequestConfirm(&mib_req) == LORAMAC_STATUS_OK &&
		    mib_req.Param.DevAddr != 0) {
			LOG_INF("Join completed (DevAddr: %08x) - transitioning to HEALTHY",
				mib_req.Param.DevAddr);
			k_timer_stop(&m_link_check_timer);
			lorawan_enable_adr(g_app_config.lrw_adr);
			m_state = APP_LRW_STATE_HEALTHY;
			restart_normal_operation();
		}
	}
}

static void handle_link_check_failure(void)
{
	m_link_check_pending = false;
	m_consecutive_lc_ok = 0; /* Reset OK streak on any failure */

	switch (m_state) {
	case APP_LRW_STATE_HEALTHY:
		m_consecutive_lc_fail++;
		LOG_WRN("LC FAIL in HEALTHY (streak: %d/%d)",
			m_consecutive_lc_fail, FAIL_THRESHOLD_WARNING);

		if (m_consecutive_lc_fail >= FAIL_THRESHOLD_WARNING) {
			LOG_WRN("Entering WARNING state");
			m_state = APP_LRW_STATE_WARNING;
			m_consecutive_lc_fail = 0;
			m_warning_lc_fail_total = 0;
			m_force_lc_remaining = 0; /* WARNING uses normal N-th interval */
		} else {
			/* Force LC on remaining attempts to recover */
			m_force_lc_remaining = FAIL_THRESHOLD_WARNING - m_consecutive_lc_fail;
		}
		break;

	case APP_LRW_STATE_WARNING:
		m_warning_lc_fail_total++;
		LOG_WRN("LC FAIL in WARNING (total: %d/%d)",
			m_warning_lc_fail_total, FAIL_THRESHOLD_RECONNECT);

		if (m_warning_lc_fail_total >= FAIL_THRESHOLD_RECONNECT) {
			/* Only OTAA can reconnect */
			if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_OTAA) {
				LOG_ERR("Entering RECONNECT state - will rejoin in 5 seconds");
				m_state = APP_LRW_STATE_RECONNECT;
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
		LOG_WRN("LC FAIL in state %d (ignored)", m_state);
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
	m_link_check_pending = false;
	m_consecutive_lc_fail = 0; /* Reset fail streak on any success */

	switch (m_state) {
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
			m_state = APP_LRW_STATE_HEALTHY;
			m_consecutive_lc_ok = 0;
			m_warning_lc_fail_total = 0;
			m_force_lc_remaining = 0;
		} else {
			/* Need more OKs, force LC on next message */
			m_force_lc_remaining = 1;
		}
		break;

	default:
		LOG_INF("LC OK in state %d", m_state);
		break;
	}
}

static void link_check_callback(uint8_t demod_margin, uint8_t nb_gateways)
{
	LOG_INF("Link check response: margin=%d dB, gateways=%d", demod_margin, nb_gateways);

	m_last_margin = demod_margin;
	m_last_gw_count = nb_gateways;

	/* Stop timeout timer */
	k_timer_stop(&m_link_check_timer);

	if (nb_gateways == 0) {
		/* No gateway received our message - treat as failure */
		handle_link_check_failure();
		return;
	}

	handle_link_check_success();
}

static void link_check_timeout_handler(struct k_timer *timer)
{
	/*
	 * Timer fired - this could be either:
	 * 1. Link check timeout (no response received)
	 * 2. Join retry interval elapsed (in JOINING state)
	 * 3. Rejoin retry interval elapsed (in RECONNECT state)
	 */
	if (m_state == APP_LRW_STATE_RECONNECT) {
		/* In RECONNECT: trigger rejoin work */
		k_work_submit_to_queue(&m_work_q, &m_rejoin_work);
	} else if (m_state == APP_LRW_STATE_JOINING) {
		/* In JOINING: trigger join work */
		k_work_submit_to_queue(&m_work_q, &m_join_work);
	} else {
		/* Otherwise: trigger link check work */
		k_work_submit_to_queue(&m_work_q, &m_link_check_work);
	}
}

static void link_check_work_handler(struct k_work *work)
{
	int ret;

	if (m_state == APP_LRW_STATE_JOINING || m_state == APP_LRW_STATE_RECONNECT) {
		/* Don't request link check while joining/reconnecting */
		return;
	}

	/*
	 * If link check is pending and this work runs,
	 * it means the timeout fired - treat as failure
	 */
	if (m_link_check_pending) {
		LOG_WRN("Link check timeout - no response received");
		handle_link_check_failure();
		return;
	}

	m_link_check_pending = true;

	ret = lorawan_request_link_check(false);
	if (ret) {
		LOG_ERR("Link check request failed: %d", ret);
		handle_link_check_failure();
		return;
	}

	LOG_DBG("Link check requested");

	/* Start timeout timer for response */
	k_timer_start(&m_link_check_timer, K_SECONDS(LINK_CHECK_TIMEOUT_SEC), K_FOREVER);
}

static void rejoin_work_handler(struct k_work *work)
{
	int ret;

	LOG_INF("Starting rejoin attempt %d...", m_rejoin_attempts + 1);

	/* Stop send timer during rejoin */
	k_timer_stop(&m_send_timer);

	/* ABP doesn't need rejoin */
	if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_ABP) {
		LOG_INF("ABP mode - rejoin not applicable");
		m_state = APP_LRW_STATE_HEALTHY;
		restart_normal_operation();
		return;
	}

	/*
	 * Reset MAC state machine before rejoin.
	 * This clears any pending operations that could cause EBUSY (-116).
	 * Similar to what happens on device restart.
	 */
	LoRaMacReset();

	/* Perform OTAA join */
	static struct lorawan_join_config config = {0};
	config.dev_eui = g_app_config.lrw_deveui;
	config.mode = LORAWAN_ACT_OTAA;
	config.otaa.join_eui = g_app_config.lrw_joineui;
	config.otaa.nwk_key = g_app_config.lrw_nwkkey;
	config.otaa.app_key = g_app_config.lrw_appkey;

	ret = lorawan_join(&config);
	if (ret) {
		uint32_t backoff = calculate_rejoin_backoff(m_rejoin_attempts);
		LOG_ERR("Rejoin failed: %d, will retry in %u seconds (attempt %d)",
			ret, backoff, m_rejoin_attempts + 1);
		m_rejoin_attempts++;
		/* Schedule retry with exponential backoff */
		k_timer_start(&m_link_check_timer, K_SECONDS(backoff), K_FOREVER);
		return;
	}

	lorawan_enable_adr(g_app_config.lrw_adr);

	LOG_INF("Rejoin successful after %d attempt(s)", m_rejoin_attempts + 1);

	m_state = APP_LRW_STATE_HEALTHY;

	restart_normal_operation();
}

static void join_work_handler(struct k_work *work)
{
	int ret;

	/* Stop send timer to prevent TX during join */
	k_timer_stop(&m_send_timer);

	m_state = APP_LRW_STATE_JOINING;

	static struct lorawan_join_config config = {0};

	config.dev_eui = g_app_config.lrw_deveui;

	if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_OTAA) {
		LOG_INF("Using OTAA activation");
		config.mode = LORAWAN_ACT_OTAA;
		config.otaa.join_eui = g_app_config.lrw_joineui;
		config.otaa.nwk_key = g_app_config.lrw_nwkkey;
		config.otaa.app_key = g_app_config.lrw_appkey;
	} else if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_ABP) {
		LOG_INF("Using ABP activation");
		config.mode = LORAWAN_ACT_ABP;
		config.abp.dev_addr = sys_get_be32(g_app_config.lrw_devaddr);
		config.abp.nwk_skey = g_app_config.lrw_nwkskey;
		config.abp.app_skey = g_app_config.lrw_appskey;
	} else {
		LOG_ERR("Invalid activation mode: %d", g_app_config.lrw_activation);
		return;
	}

	ret = lorawan_join(&config);
	if (ret) {
		uint32_t backoff = calculate_rejoin_backoff(m_rejoin_attempts);
		LOG_ERR("Join failed: %d, will retry in %u seconds (attempt %d)",
			ret, backoff, m_rejoin_attempts + 1);
		m_rejoin_attempts++;
		/* Keep state as JOINING, schedule retry with backoff */
		k_timer_start(&m_link_check_timer, K_SECONDS(backoff), K_FOREVER);
		return;
	}

	lorawan_enable_adr(g_app_config.lrw_adr);

	LOG_INF("LoRaWAN join successful");

	m_state = APP_LRW_STATE_HEALTHY;

	restart_normal_operation();
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
	uint8_t msg_num = m_message_count + 1;

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

	/* Block transmissions during joining or reconnect */
	if (m_state == APP_LRW_STATE_JOINING || m_state == APP_LRW_STATE_RECONNECT) {
		LOG_WRN("TX blocked: state=%d", m_state);
		return;
	}

	int timeout = g_app_config.interval_report;

#if defined(CONFIG_ENTROPY_GENERATOR)
	timeout += (int32_t)sys_rand32_get() % (g_app_config.interval_report / 10);
#endif /* defined(CONFIG_ENTROPY_GENERATOR) */

	LOG_INF("Scheduling next timeout in %d seconds", timeout);

	k_timer_start(&m_send_timer, K_SECONDS(timeout), K_FOREVER);

	if (!g_app_config.interval_sample) {
		app_sensor_sample();
	}

	uint8_t buf[51];
	size_t len;
	ret = app_compose(buf, sizeof(buf), &len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_compose", ret);
		return;
	}

	/* Determine if this message should have link check */
	with_link_check = should_request_link_check();

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

	ret = lorawan_send(1, buf, len, LORAWAN_MSG_UNCONFIRMED);
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

int app_lrw_init(void)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(lora0));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

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
	k_work_init(&m_rejoin_work, rejoin_work_handler);

	k_timer_init(&m_send_timer, send_timer_handler, NULL);
	k_timer_init(&m_link_check_timer, link_check_timeout_handler, NULL);
	k_timer_start(&m_send_timer, K_SECONDS(5), K_FOREVER);

	m_state = APP_LRW_STATE_IDLE;

	return 0;
}

void app_lrw_join(void)
{
	k_work_submit_to_queue(&m_work_q, &m_join_work);
}

void app_lrw_rejoin(void)
{
	m_state = APP_LRW_STATE_RECONNECT;
	m_rejoin_attempts = 0;
	k_work_submit_to_queue(&m_work_q, &m_rejoin_work);
}

void app_lrw_send(void)
{
	k_timer_start(&m_send_timer, K_NO_WAIT, K_FOREVER);
}

void app_lrw_send_with_link_check(void)
{
	int ret = lorawan_request_link_check(false);
	if (ret) {
		LOG_ERR("Link check request failed: %d", ret);
	} else {
		m_link_check_pending = true;
		/* Start timeout timer for response */
		k_timer_start(&m_link_check_timer, K_SECONDS(LINK_CHECK_TIMEOUT_SEC), K_FOREVER);
		LOG_INF("Link check requested, timeout in %d seconds", LINK_CHECK_TIMEOUT_SEC);
	}

	k_timer_start(&m_send_timer, K_NO_WAIT, K_FOREVER);
}

enum app_lrw_state app_lrw_get_state(void)
{
	return m_state;
}

bool app_lrw_is_ready(void)
{
	return m_state == APP_LRW_STATE_HEALTHY || m_state == APP_LRW_STATE_WARNING;
}

int app_lrw_get_info(struct app_lrw_info *info)
{
	MibRequestConfirm_t mib_req;

	if (!info) {
		return -EINVAL;
	}

	info->state = m_state;

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
