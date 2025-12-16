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

/* Link check configuration from Kconfig */
#define LINK_CHECK_INTERVAL_SEC       CONFIG_APP_LRW_LINK_CHECK_INTERVAL
#define LINK_CHECK_RETRY_INTERVAL_SEC CONFIG_APP_LRW_LINK_CHECK_RETRY_INTERVAL
#define LINK_CHECK_MAX_RETRIES        CONFIG_APP_LRW_LINK_CHECK_MAX_RETRIES
#define LINK_CHECK_TIMEOUT_SEC        CONFIG_APP_LRW_LINK_CHECK_TIMEOUT

static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
static struct k_work_q m_work_q;
static struct k_timer m_send_timer;
static struct k_work m_send_work;
static struct k_work m_join_work;

/* Link check state management */
static enum app_lrw_state m_state = APP_LRW_STATE_IDLE;
static int m_link_check_retry_count;
static enum lorawan_datarate m_current_dr = LORAWAN_DR_0;
static uint32_t m_session_start_s = 0;
static int16_t m_last_rssi = 0;
static int8_t m_last_snr = 0;
static uint8_t m_last_link_check_margin = 0;
static uint8_t m_last_link_check_gateways = 0;
static struct k_work_delayable m_link_check_dwork;
static struct k_work_delayable m_rejoin_dwork;

/* Forward declarations */
static void schedule_next_link_check(void);
static void handle_link_check_failure(void);
static void restart_normal_operation(void);

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

	m_current_dr = dr;
	lorawan_get_payload_sizes(&max_next_payload_size, &max_payload_size);

	LOG_INF("New data rate: DR%d", dr);
	LOG_INF("Maximum payload size: %d", max_payload_size);
}

static void schedule_next_link_check(void)
{
	int timeout = LINK_CHECK_INTERVAL_SEC;

#if defined(CONFIG_ENTROPY_GENERATOR)
	/* Add up to 10% randomness */
	timeout += (int32_t)sys_rand32_get() % (LINK_CHECK_INTERVAL_SEC / 10);
#endif

	k_work_schedule_for_queue(&m_work_q, &m_link_check_dwork, K_SECONDS(timeout));
	LOG_DBG("Next link check in %d seconds", timeout);
}

static void handle_link_check_failure(void)
{
	m_link_check_retry_count++;

	LOG_WRN("Link check failed (attempt %d/%d)", m_link_check_retry_count,
		LINK_CHECK_MAX_RETRIES);

	if (m_link_check_retry_count >= LINK_CHECK_MAX_RETRIES) {
		LOG_ERR("All link check retries failed, initiating rejoin");
		m_state = APP_LRW_STATE_JOINING;
		m_link_check_retry_count = 0;
		k_work_submit_to_queue(&m_work_q, &m_rejoin_dwork.work);
		return;
	}

	/* Schedule retry with randomness */
	m_state = APP_LRW_STATE_LINK_CHECK_RETRY;

	int timeout = LINK_CHECK_RETRY_INTERVAL_SEC;
#if defined(CONFIG_ENTROPY_GENERATOR)
	timeout += (int32_t)sys_rand32_get() % (LINK_CHECK_RETRY_INTERVAL_SEC / 10);
#endif

	k_work_schedule_for_queue(&m_work_q, &m_link_check_dwork, K_SECONDS(timeout));
	LOG_INF("Retrying link check in %d seconds", timeout);
}

static void restart_normal_operation(void)
{
	/* Restart send timer */
	k_timer_start(&m_send_timer, K_SECONDS(5), K_FOREVER);

	/* Schedule link check */
	schedule_next_link_check();
}

static void link_check_callback(uint8_t demod_margin, uint8_t nb_gateways)
{
	LOG_INF("Link check response: margin=%d dB, gateways=%d", demod_margin, nb_gateways);

	/* Store last link check result */
	m_last_link_check_margin = demod_margin;
	m_last_link_check_gateways = nb_gateways;

	/* Cancel pending timeout - this properly cancels both timer and queued work */
	k_work_cancel_delayable(&m_link_check_dwork);

	if (nb_gateways == 0) {
		/* No gateway received our message - treat as failure */
		handle_link_check_failure();
		return;
	}

	/* Success - reset retry counter and return to healthy state */
	m_link_check_retry_count = 0;
	m_state = APP_LRW_STATE_HEALTHY;

	/* Schedule next link check */
	schedule_next_link_check();
}

static void link_check_work_handler(struct k_work *work)
{
	int ret;

	if (m_state == APP_LRW_STATE_JOINING) {
		/* Don't request link check while joining */
		return;
	}

	/*
	 * If we're in LINK_CHECK_PENDING state and this work runs,
	 * it means the timeout fired - treat as failure
	 */
	if (m_state == APP_LRW_STATE_LINK_CHECK_PENDING) {
		LOG_WRN("Link check timeout - no response received");
		handle_link_check_failure();
		return;
	}

	m_state = APP_LRW_STATE_LINK_CHECK_PENDING;

	ret = lorawan_request_link_check(true);
	if (ret) {
		LOG_ERR("Link check request failed: %d", ret);
		handle_link_check_failure();
		return;
	}

	LOG_DBG("Link check requested");

	/* Schedule timeout for response */
	k_work_schedule_for_queue(&m_work_q, &m_link_check_dwork,
				  K_SECONDS(LINK_CHECK_TIMEOUT_SEC));
}

static void rejoin_work_handler(struct k_work *work)
{
	int ret;

	LOG_INF("Starting rejoin process...");

	/* Stop send timer during rejoin */
	k_timer_stop(&m_send_timer);

	/* ABP doesn't need rejoin */
	if (g_app_config.lrw_activation == APP_CONFIG_LRW_ACTIVATION_ABP) {
		LOG_INF("ABP mode - rejoin not applicable");
		m_state = APP_LRW_STATE_HEALTHY;
		restart_normal_operation();
		return;
	}

	/* Perform OTAA join */
	static struct lorawan_join_config config = {0};
	config.dev_eui = g_app_config.lrw_deveui;
	config.mode = LORAWAN_ACT_OTAA;
	config.otaa.join_eui = g_app_config.lrw_joineui;
	config.otaa.nwk_key = g_app_config.lrw_nwkkey;
	config.otaa.app_key = g_app_config.lrw_appkey;

	ret = lorawan_join(&config);
	if (ret) {
		LOG_ERR("Rejoin failed: %d, will retry in 60 seconds", ret);
		/* Schedule rejoin retry after 60 seconds */
		k_work_schedule_for_queue(&m_work_q, &m_rejoin_dwork, K_SECONDS(60));
		return;
	}

	lorawan_enable_adr(g_app_config.lrw_adr);

	LOG_INF("Rejoin successful");
	m_session_start_s = (uint32_t)(k_uptime_get() / 1000);
	m_state = APP_LRW_STATE_HEALTHY;

	restart_normal_operation();
}

static void join_work_handler(struct k_work *work)
{
	int ret;

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
		LOG_ERR_CALL_FAILED_INT("lorawan_join", ret);
		/* Keep state as JOINING, schedule retry */
		k_work_schedule_for_queue(&m_work_q, &m_rejoin_dwork, K_SECONDS(60));
		return;
	}

	lorawan_enable_adr(g_app_config.lrw_adr);

	LOG_INF("LoRaWAN join successful");

	m_state = APP_LRW_STATE_HEALTHY;
	m_session_start_s = (uint32_t)(k_uptime_get() / 1000);

	/* Start link check timer after successful join */
	schedule_next_link_check();
}

static void send_work_handler(struct k_work *work)
{
	int ret;

	/* Block transmissions during joining or link check retry */
	if (m_state == APP_LRW_STATE_JOINING ||
	    m_state == APP_LRW_STATE_LINK_CHECK_RETRY) {
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

	LOG_INF("Sending data...");

	ret = lorawan_send(1, buf, len, LORAWAN_MSG_UNCONFIRMED);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lorawan_send", ret);
	}

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
	k_work_init_delayable(&m_link_check_dwork, link_check_work_handler);
	k_work_init_delayable(&m_rejoin_dwork, rejoin_work_handler);

	k_timer_init(&m_send_timer, send_timer_handler, NULL);
	k_timer_start(&m_send_timer, K_SECONDS(5), K_FOREVER);

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
	return m_state == APP_LRW_STATE_HEALTHY ||
	       m_state == APP_LRW_STATE_LINK_CHECK_PENDING;
}

int app_lrw_get_info(struct app_lrw_info *info)
{
	MibRequestConfirm_t mib_req;

	memset(info, 0, sizeof(*info));

	info->state = m_state;
	info->joined = (m_state != APP_LRW_STATE_IDLE && m_state != APP_LRW_STATE_JOINING);
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

	/* Last link check result */
	info->link_check_margin = m_last_link_check_margin;
	info->link_check_gateways = m_last_link_check_gateways;

	return 0;
}
