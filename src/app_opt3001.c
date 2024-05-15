/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_opt3001.h"

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
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(opt3001));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(dev);
	if (ret) {
		LOG_ERR("Call `sensor_sample_fetch` failed: %d", ret);
		return ret;
	}

	struct sensor_value val;
	ret = sensor_channel_get(dev, SENSOR_CHAN_LIGHT, &val);
	if (ret) {
		LOG_ERR("Call `sensor_channel_get` failed: %d", ret);
		return ret;
	}

	float illuminance_ = sensor_value_to_float(&val);

	LOG_DBG("Illuminance: %.0f lux", (double)illuminance_);

	if (illuminance) {
		*illuminance = illuminance_;
	}

	return 0;
}
