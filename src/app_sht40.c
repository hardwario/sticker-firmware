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

	struct sensor_value t;
	ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &t);
	if (ret) {
		LOG_ERR("Call `sensor_channel_get` failed: %d", ret);
		return ret;
	}

	double temperature_ = sensor_value_to_double(&t);

	LOG_DBG("Temperature: %.2f C", temperature_);

	if (temperature) {
		*temperature = temperature_;
	}

	struct sensor_value h;
	ret = sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &h);
	if (ret) {
		LOG_ERR("Call `sensor_channel_get` failed: %d", ret);
		return ret;
	}

	double humidity_ = sensor_value_to_double(&h);

	LOG_DBG("Humidity: %.1f %%", humidity_);

	if (humidity) {
		*humidity = humidity_;
	}

	return 0;
}
