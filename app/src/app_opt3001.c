/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_opt3001.h"
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

LOG_MODULE_REGISTER(app_opt3001, LOG_LEVEL_DBG);

int app_opt3001_read(float *illuminance)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(opt3001));
	static const enum sensor_channel chans[] = {SENSOR_CHAN_LIGHT};
	float vals[ARRAY_SIZE(chans)];

	int ret = app_sensor_read_channels(dev, chans, vals, ARRAY_SIZE(chans));
	if (ret) {
		return ret;
	}

	LOG_DBG("Illuminance: %d lux", APP_FP0(vals[0]));

	if (illuminance) {
		*illuminance = vals[0];
	}

	return 0;
}
