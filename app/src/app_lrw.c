/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_compose.h"
#include "app_config.h"
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
#include <LoRaMac.h>
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_lrw, LOG_LEVEL_DBG);

static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
static struct k_work_q m_work_q;
static struct k_timer m_send_timer;
static struct k_work m_send_work;
static struct k_work m_join_work;

/* State management */
static enum app_lrw_state m_state = APP_LRW_STATE_IDLE;
static enum lorawan_datarate m_current_dr = LORAWAN_DR_0;
static uint32_t m_session_start_s = 0;
static int16_t m_last_rssi = 0;
static int8_t m_last_snr = 0;
static bool m_lrw_bypass = false;

/* Confirmed message tracking */
static uint32_t m_msg_count = 0;
static uint32_t m_ack_failure_count = 0;

/* Forward declarations */
static void handle_ack_success(void);
static void handle_ack_failure(void);

static void downlink_callback(uint8_t port, uint8_t flags, int16_t rssi, int8_t snr, uint8_t len,
			      const uint8_t *data)
{
	m_last_rssi = rssi;
	m_last_snr = snr;

	if (port != 0) {
		LOG_INF("Downlink: port=%d, RSSI=%d dB, SNR=%d dB", port, rssi, snr);
		if (data) {
			LOG_HEXDUMP_INF(data, len, "Payload: ");
		}
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

	m_current_dr = dr;
	lorawan_get_payload_sizes(&max_next_payload_size, &max_payload_size);

	LOG_INF("New data rate: DR%d", dr);
	LOG_INF("Maximum payload size: %d", max_payload_size);
}

static void handle_ack_success(void)
{
	LOG_INF("ACK OK (failures reset: %u -> 0)", m_ack_failure_count);
	m_ack_failure_count = 0;

	if (m_state == APP_LRW_STATE_FAILURE) {
		LOG_INF("FAILURE -> HEALTHY (recovered)");
		m_state = APP_LRW_STATE_HEALTHY;
	}
}

static void handle_ack_failure(void)
{
	m_ack_failure_count++;
	LOG_WRN("ACK FAIL #%u/%d", m_ack_failure_count, CONFIG_APP_LRW_ACK_FAILURES_MAX);

	if (m_state == APP_LRW_STATE_HEALTHY) {
		LOG_ERR("HEALTHY -> FAILURE");
		m_state = APP_LRW_STATE_FAILURE;
		m_ack_failure_count = 1;
	} else if (m_state == APP_LRW_STATE_FAILURE) {
		if (m_ack_failure_count >= CONFIG_APP_LRW_ACK_FAILURES_MAX) {
			if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_OTAA) {
				LOG_ERR("FAILURE -> JOINING (max %d fails, OTAA rejoin)",
					CONFIG_APP_LRW_ACK_FAILURES_MAX);
				app_lrw_join();
			} else {
				LOG_WRN("ABP: max %d fails, reset counter (rejoin useless)",
					CONFIG_APP_LRW_ACK_FAILURES_MAX);
				m_ack_failure_count = 0;
			}
		}
	}
}

static void join_work_handler(struct k_work *work)
{
	int ret;

	LOG_INF("=== JOIN START (counters reset) ===");

	/* Stop send timer during join */
	k_timer_stop(&m_send_timer);

	m_state = APP_LRW_STATE_JOINING;
	m_msg_count = 0;
	m_ack_failure_count = 0;

	/* If bypass is active, stay in JOINING but don't actually join */
	if (m_lrw_bypass) {
		LOG_WRN("BYPASS: join blocked, staying in JOINING");
		return;
	}

	/* Configure join */
	static struct lorawan_join_config config = {0};
	config.dev_eui = g_app_config.lrw_deveui;

	if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_OTAA) {
		LOG_INF("Using OTAA");
		config.mode = LORAWAN_ACT_OTAA;
		config.otaa.join_eui = g_app_config.lrw_joineui;
		config.otaa.nwk_key = g_app_config.lrw_nwkkey;
		config.otaa.app_key = g_app_config.lrw_appkey;
	} else {
		LOG_INF("Using ABP");
		config.mode = LORAWAN_ACT_ABP;
		config.abp.dev_addr = sys_get_be32(g_app_config.lrw_devaddr);
		config.abp.nwk_skey = g_app_config.lrw_nwkskey;
		config.abp.app_skey = g_app_config.lrw_appskey;
	}

	ret = lorawan_join(&config);
	if (ret) {
		/* OTAA: cannot send without session keys, wait longer for retry */
		int wait_s = g_app_config.interval_report * CONFIG_APP_LRW_CONFIRMED_INTERVAL;
		LOG_ERR("JOIN FAIL (ret=%d), retry in %d s", ret, wait_s);
		k_timer_start(&m_send_timer, K_SECONDS(wait_s), K_FOREVER);
		return;
	}

	lorawan_enable_adr(g_app_config.lrw_adr);
	m_session_start_s = (uint32_t)(k_uptime_get() / 1000);

	if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_OTAA) {
		LOG_INF("=== OTAA JOIN OK -> HEALTHY ===");
	} else {
		LOG_INF("=== ABP configured -> HEALTHY (first msg will be CONF) ===");
	}
	m_state = APP_LRW_STATE_HEALTHY;

	/* Start send timer */
	k_timer_start(&m_send_timer, K_NO_WAIT, K_SECONDS(g_app_config.interval_report));
}

static void send_work_handler(struct k_work *work)
{
	int ret;
	bool use_confirmed = false;

	/* Block only in IDLE */
	if (m_state == APP_LRW_STATE_IDLE) {
		LOG_WRN("Send blocked - not joined yet");
		return;
	}

	/* JOINING state: only OTAA can be here, retry join */
	if (m_state == APP_LRW_STATE_JOINING) {
		LOG_INF("OTAA JOINING: retrying join...");
		k_work_submit_to_queue(&m_work_q, &m_join_work);
		return;
	}

	/* Schedule next send */
	int timeout = g_app_config.interval_report;

#if defined(CONFIG_ENTROPY_GENERATOR)
	timeout += (int32_t)sys_rand32_get() % (g_app_config.interval_report / 10 + 1);
#endif

	LOG_INF("Next send in %d seconds", timeout);
	k_timer_start(&m_send_timer, K_SECONDS(timeout), K_FOREVER);

	/* Determine if confirmed message needed */
	if (CONFIG_APP_LRW_CONFIRMED_INTERVAL > 0) {
		if (m_msg_count == 0 &&
		    g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_ABP) {
			/* ABP: first message must be confirmed to verify network */
			use_confirmed = true;
		} else if ((m_msg_count % CONFIG_APP_LRW_CONFIRMED_INTERVAL) == 0) {
			/* Every Nth message */
			use_confirmed = true;
		}
	}

	/* Bypass - simulate ACK failure for confirmed messages */
	if (m_lrw_bypass && use_confirmed) {
		LOG_WRN("BYPASS: simulating ACK fail for msg #%u", m_msg_count);
		handle_ack_failure();
		m_msg_count++;
		return;
	}

	/* Sample sensors if not using interval_sample */
	if (!g_app_config.interval_sample) {
		app_sensor_sample();
	}

	/* Build payload */
	uint8_t buf[51];
	size_t len;
	ret = app_compose(buf, sizeof(buf), &len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_compose", ret);
		return;
	}

	enum lorawan_message_type msg_type = use_confirmed ?
		LORAWAN_MSG_CONFIRMED : LORAWAN_MSG_UNCONFIRMED;

	uint32_t next_confirmed = CONFIG_APP_LRW_CONFIRMED_INTERVAL > 0 ?
		CONFIG_APP_LRW_CONFIRMED_INTERVAL - (m_msg_count % CONFIG_APP_LRW_CONFIRMED_INTERVAL) : 0;

	LOG_INF("TX #%u %s (%zu B), next conf in %u msgs, fails=%u/%d",
		m_msg_count,
		use_confirmed ? "CONF" : "UNCONF",
		len,
		use_confirmed ? CONFIG_APP_LRW_CONFIRMED_INTERVAL : next_confirmed,
		m_ack_failure_count,
		CONFIG_APP_LRW_ACK_FAILURES_MAX);

	ret = lorawan_send(1, buf, len, msg_type);
	m_msg_count++;

	if (use_confirmed) {
		if (ret == 0) {
			handle_ack_success();
		} else {
			LOG_WRN("TX #%u CONF failed (ret=%d)", m_msg_count - 1, ret);
			handle_ack_failure();
		}
	} else if (ret != 0) {
		LOG_ERR("TX #%u failed (ret=%d)", m_msg_count - 1, ret);
	} else {
		LOG_DBG("TX #%u OK", m_msg_count - 1);
	}
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

	k_work_queue_init(&m_work_q);
	k_work_queue_start(&m_work_q, m_work_stack, K_THREAD_STACK_SIZEOF(m_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_work_init(&m_join_work, join_work_handler);
	k_work_init(&m_send_work, send_work_handler);
	k_timer_init(&m_send_timer, send_timer_handler, NULL);

	m_state = APP_LRW_STATE_IDLE;

	return 0;
}

void app_lrw_join(void)
{
	k_work_submit_to_queue(&m_work_q, &m_join_work);
}

void app_lrw_send(void)
{
	k_timer_start(&m_send_timer, K_NO_WAIT, K_FOREVER);
}

enum app_lrw_state app_lrw_get_state(void)
{
	return m_state;
}

bool app_lrw_is_ready(void)
{
	return m_state == APP_LRW_STATE_HEALTHY;
}

int app_lrw_get_info(struct app_lrw_info *info)
{
	MibRequestConfirm_t mib_req;

	if (!info) {
		return -EINVAL;
	}

	memset(info, 0, sizeof(*info));

	info->state = m_state;
	info->datarate = (int)m_current_dr;
	info->min_datarate = (int)lorawan_get_min_datarate();
	lorawan_get_payload_sizes(&info->max_next_payload, &info->max_payload);

	uint32_t now_s = (uint32_t)(k_uptime_get() / 1000);
	if (m_session_start_s > 0 && now_s >= m_session_start_s) {
		info->session_uptime_s = now_s - m_session_start_s;
	} else {
		info->session_uptime_s = 0;
	}

	/* Get DevAddr */
	mib_req.Type = MIB_DEV_ADDR;
	if (LoRaMacMibGetRequestConfirm(&mib_req) == LORAMAC_STATUS_OK) {
		info->dev_addr = mib_req.Param.DevAddr;
	}

	/* Get NwkSKey (FNwkSIntKey) */
	mib_req.Type = MIB_F_NWK_S_INT_KEY;
	if (LoRaMacMibGetRequestConfirm(&mib_req) == LORAMAC_STATUS_OK) {
		memcpy(info->nwk_s_key, mib_req.Param.FNwkSIntKey, 16);
	}

	/* Get AppSKey */
	mib_req.Type = MIB_APP_S_KEY;
	if (LoRaMacMibGetRequestConfirm(&mib_req) == LORAMAC_STATUS_OK) {
		memcpy(info->app_s_key, mib_req.Param.AppSKey, 16);
	}

	/* Last received signal quality */
	info->last_rssi = m_last_rssi;
	info->last_snr = m_last_snr;

	/* Message statistics */
	info->msg_count = m_msg_count;
	info->ack_failure_count = m_ack_failure_count;

	return 0;
}

void app_lrw_set_bypass(bool enable)
{
	m_lrw_bypass = enable;
	LOG_INF("LoRaWAN bypass %s", enable ? "enabled" : "disabled");
}

bool app_lrw_get_bypass(void)
{
	return m_lrw_bypass;
}
