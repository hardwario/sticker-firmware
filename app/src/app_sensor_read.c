/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_sensor_read.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>

LOG_MODULE_REGISTER(app_sensor_read, LOG_LEVEL_DBG);

int app_sensor_read_channels(const struct device *dev, const enum sensor_channel *chans, float *out,
			     size_t n)
{
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	int ret = sensor_sample_fetch(dev);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_sample_fetch", ret);
		return ret;
	}

	for (size_t i = 0; i < n; i++) {
		struct sensor_value val;

		ret = sensor_channel_get(dev, chans[i], &val);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("sensor_channel_get", ret);
			return ret;
		}

		out[i] = sensor_value_to_float(&val);
	}

	return 0;
}
