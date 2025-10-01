/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_mpl3115a2.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>

LOG_MODULE_REGISTER(app_mpl3115a2, LOG_LEVEL_DBG);

int app_mpl3115a2_read(float *altitude, float *pressure, float *temperature)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(mpl3115a2));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(dev);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_sample_fetch", ret);
		return ret;
	}

	struct sensor_value val;

	ret = sensor_channel_get(dev, SENSOR_CHAN_ALTITUDE, &val);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_channel_get", ret);
		return ret;
	}

	float altitude_ = sensor_value_to_float(&val);

	LOG_DBG("Altitude: %.1f m", (double)altitude_);

	if (altitude) {
		*altitude = altitude_;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_PRESS, &val);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_channel_get", ret);
		return ret;
	}

	float pressure_ = sensor_value_to_float(&val);

	LOG_DBG("Pressure: %.2f hPa", (double)(pressure_ / 100.f));

	if (pressure) {
		*pressure = pressure_;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_channel_get", ret);
		return ret;
	}

	float temperature_ = sensor_value_to_float(&val);

	LOG_DBG("Temperature: %.2f C", (double)temperature_);

	if (temperature) {
		*temperature = temperature_;
	}

	return 0;
}
