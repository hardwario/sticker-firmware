/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_sht40.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>

LOG_MODULE_REGISTER(app_sht40, LOG_LEVEL_DBG);

int app_sht40_read(float *temperature, float *humidity)
{
	int ret;

	struct sensor_value val;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(sht40));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(dev);
	if (ret) {
		LOG_ERR("Call `sensor_sample_fetch` failed: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	if (ret) {
		LOG_ERR("Call `sensor_channel_get` failed: %d", ret);
		return ret;
	}

	float temperature_ = sensor_value_to_float(&val);

	LOG_DBG("Temperature: %.2f C", (double)temperature_);

	if (temperature) {
		*temperature = temperature_;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &val);
	if (ret) {
		LOG_ERR("Call `sensor_channel_get` failed: %d", ret);
		return ret;
	}

	float humidity_ = sensor_value_to_float(&val);

	LOG_DBG("Humidity: %.1f %%", (double)humidity_);

	if (humidity) {
		*humidity = humidity_;
	}

	return 0;
}
