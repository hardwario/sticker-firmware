/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "app_ds18b20.h"
#include "app_led.h"
#include "app_log.h"
#include "app_machine_probe.h"
#include "app_sht40.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <stdint.h>

LOG_MODULE_REGISTER(app_calibration, LOG_LEVEL_DBG);

static void read_ds18b20(int count)
{
	for (int i = 0; i < count; i++) {
		uint64_t serial_number;
		float temperature;
		int ret = app_ds18b20_read(i, &serial_number, &temperature);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_ds18b20_read", ret);
			continue;
		}

		LOG_INF("DS18B20 Serial number: %llu / Temperature: %.2f C", serial_number,
			(double)temperature);

		printk("Serial number: %010u / Temperature: %.2f C / Humidity: %.1f %%\n",
		       g_app_config.serial_number, (double)temperature, (double)0);
	}
}

static void read_machine_probe(int count)
{
	int ret;

	for (int i = 0; i < count; i++) {
		uint64_t serial_number;
		float temperature;
		float humidity;
		ret = app_machine_probe_read_hygrometer(i, &serial_number, &temperature, &humidity);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_machine_probe_read_hygrometer", ret);
			continue;
		}

		LOG_INF("Machine Probe Serial number: %llu / Hygrometer / "
			"Temperature: %.2f C",
			serial_number, (double)temperature);

		LOG_INF("Machine Probe Serial number: %llu / Hygrometer / "
			"Humidity: %.1f %%",
			serial_number, (double)humidity);

		printk("Serial number: %010u / Temperature: %.2f C / Humidity: %.1f %%\n",
		       g_app_config.serial_number, (double)temperature, (double)humidity);
	}
}

static void read_sht40(void)
{
	int ret;

	float temperature;
	float humidity;
	ret = app_sht40_read(&temperature, &humidity);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_sht40_read", ret);
		return;
	}

	LOG_INF("SHT40 Temperature: %.2f C", (double)temperature);
	LOG_INF("SHT40 Humidity: %.1f %%", (double)humidity);

	printk("Serial number: %010u / Temperature: %.2f C / Humidity: %.1f %%\n",
	       g_app_config.serial_number, (double)temperature, (double)humidity);
}

static int init(void)
{
	int ret;

	int count_ds18b20 = 0;
	int count_machine_probe = 0;

	if (!g_app_config.calibration) {
		return 0;
	}

	LOG_WRN("Calibration mode is enabled");

	if (g_app_config.cap_1w_thermometer || g_app_config.cap_1w_machine_probe) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds2484", ret);
			return ret;
		}
	}

	if (g_app_config.cap_1w_thermometer) {
		const struct device *dev_0 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_0));

		ret = device_init(dev_0);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds18b20_0", ret);
			return ret;
		}

		const struct device *dev_1 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_1));

		ret = device_init(dev_1);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds18b20_1", ret);
			return ret;
		}

		ret = app_ds18b20_scan();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_ds18b20_scan", ret);
			return ret;
		}

		count_ds18b20 = app_ds18b20_get_count();
	}

	if (g_app_config.cap_1w_machine_probe) {
		const struct device *dev_0 = DEVICE_DT_GET(DT_NODELABEL(machine_probe_0));

		ret = device_init(dev_0);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "machine_probe_0", ret);
			return ret;
		}

		const struct device *dev_1 = DEVICE_DT_GET(DT_NODELABEL(machine_probe_1));

		ret = device_init(dev_1);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "machine_probe_1", ret);
			return ret;
		}

		ret = app_machine_probe_scan();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_machine_probe_scan", ret);
			return ret;
		}

		count_machine_probe = app_machine_probe_get_count();
	}

	for (;;) {
		LOG_INF("Alive");

#if defined(CONFIG_WATCHDOG)
		ret = app_wdog_feed();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_wdog_feed", ret);
			return ret;
		}
#endif /* defined(CONFIG_WATCHDOG) */

		struct app_led_blink_req req = {
			.color = APP_LED_CHANNEL_Y,
			.duration = 100,
			.space = 0,
			.repetitions = 1
		};
		app_led_blink(&req);

		if (count_ds18b20 > 0) {
			LOG_INF("Reading DS18B20 sensors...");
			read_ds18b20(count_ds18b20);
		} else if (count_machine_probe > 0) {
			LOG_INF("Reading Machine Probe sensors...");
			read_machine_probe(count_machine_probe);
		} else {
			LOG_INF("Reading SHT40 sensor...");
			read_sht40();
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}

SYS_INIT(init, APPLICATION, 1);
