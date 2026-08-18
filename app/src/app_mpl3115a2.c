/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_mpl3115a2.h"
#include "app_log.h"
#include "app_sensor_read.h"

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
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(mpl3115a2));
	static const enum sensor_channel chans[] = {
		SENSOR_CHAN_ALTITUDE,
		SENSOR_CHAN_PRESS,
		SENSOR_CHAN_AMBIENT_TEMP,
	};
	float vals[ARRAY_SIZE(chans)];

	int ret = app_sensor_read_channels(dev, chans, vals, ARRAY_SIZE(chans));
	if (ret) {
		return ret;
	}

	LOG_DBG("Altitude: %s%d.%01d m", APP_FP1(vals[0]));
	LOG_DBG("Pressure: %s%d.%02d hPa", APP_FP2(vals[1] * 10.f));
	LOG_DBG("Temperature: %s%d.%02d C", APP_FP2(vals[2]));

	if (altitude) {
		*altitude = vals[0];
	}

	if (pressure) {
		*pressure = vals[1];
	}

	if (temperature) {
		*temperature = vals[2];
	}

	return 0;
}
