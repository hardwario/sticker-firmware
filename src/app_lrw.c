/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "app_lrw.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_lrw, LOG_LEVEL_DBG);

static void downlink_cb(uint8_t port, bool data_pending, int16_t rssi, int8_t snr, uint8_t len,
			const uint8_t *hex_data)
{
	LOG_INF("Port %d, Pending %d, RSSI %d dB, SNR %d dBm", port, data_pending, rssi, snr);

	if (hex_data) {
		LOG_HEXDUMP_INF(hex_data, len, "Payload: ");
	}
}

static uint8_t battery_level_cb(void)
{
	/* TODO Implement */
	return 255;
}

static void datarate_changed_cb(enum lorawan_datarate dr)
{
	uint8_t max_next_payload_size;
	uint8_t max_payload_size;
	lorawan_get_payload_sizes(&max_next_payload_size, &max_payload_size);

	LOG_INF("New datarate: DR_%d / Max payload size: %d", dr, max_payload_size);
}

int app_lrw_join(void)
{
	int ret;

	static struct lorawan_join_config config = {0};

	config.mode = LORAWAN_ACT_ABP;
	config.dev_eui = g_app_config.lrw_deveui;
	config.abp.dev_addr = sys_get_be32(g_app_config.lrw_devaddr);
	config.abp.nwk_skey = g_app_config.lrw_nwkskey;
	config.abp.app_skey = g_app_config.lrw_appskey;

	ret = lorawan_join(&config);
	if (ret) {
		LOG_ERR("Call `lorawan_join` failed: %d", ret);
		return ret;
	}

	return 0;
}

int app_lrw_send(const void *buf, size_t len)
{
	int ret;

	ret = lorawan_send(1, (uint8_t *)buf, len, LORAWAN_MSG_UNCONFIRMED);
	if (ret) {
		LOG_ERR("Call `lorawan_send` failed: %d", ret);
		return ret;
	}

	return 0;
}

static int init(void)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(lora0));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

#if defined(CONFIG_LORAMAC_REGION_EU868)
	ret = lorawan_set_region(LORAWAN_REGION_EU868);
	if (ret < 0) {
		LOG_ERR("Call `lorawan_set_region` failed: %d", ret);
		return ret;
	}
#endif /* defined(CONFIG_LORAMAC_REGION_EU868) */

	ret = lorawan_start();
	if (ret) {
		LOG_ERR("Call `lorawan_start` failed: %d", ret);
		return ret;
	}

	static struct lorawan_downlink_cb cb = {
		.port = LW_RECV_PORT_ANY,
		.cb = downlink_cb,
	};

	lorawan_register_downlink_callback(&cb);
	lorawan_register_battery_level_callback(battery_level_cb);
	lorawan_register_dr_changed_callback(datarate_changed_cb);

	return 0;
}

SYS_INIT(init, APPLICATION, 0);
