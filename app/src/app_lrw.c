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
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_lrw, LOG_LEVEL_DBG);

static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
static struct k_work_q m_work_q;
static struct k_timer m_send_timer;
static struct k_work m_send_work;
static struct k_work m_join_work;

static void downlink_callback(uint8_t port, uint8_t flags, int16_t rssi, int8_t snr, uint8_t len,
			      const uint8_t *data)
{
	LOG_INF("Port %d, Flags 0x%02x, RSSI %d dB, SNR %d dBm", port, flags, rssi, snr);

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

	LOG_INF("New data rate: DR%d", dr);
	LOG_INF("Maximum payload size: %d", max_payload_size);
}

static void join_work_handler(struct k_work *work)
{
	int ret;

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
		return;
	}

	lorawan_enable_adr(g_app_config.lrw_adr);

	LOG_INF("LoRaWAN join successful");
}

static void send_work_handler(struct k_work *work)
{
	int ret;

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

	k_work_queue_init(&m_work_q);
	k_work_queue_start(&m_work_q, m_work_stack, K_THREAD_STACK_SIZEOF(m_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_work_init(&m_join_work, join_work_handler);
	k_work_init(&m_send_work, send_work_handler);

	k_timer_init(&m_send_timer, send_timer_handler, NULL);
	k_timer_start(&m_send_timer, K_SECONDS(5), K_FOREVER);

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
