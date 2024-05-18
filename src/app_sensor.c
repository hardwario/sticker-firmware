/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_sensor.h"

#include "app_accel.h"
#include "app_ds18b20.h"
#include "app_opt3001.h"
#include "app_sht40.h"

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_sensor, LOG_LEVEL_DBG);

struct app_sensor_data g_app_sensor_data = {
	.temperature = NAN,
	.humidity = NAN,
	.illuminance = NAN,
	.orientation = INT_MAX,
};

K_MUTEX_DEFINE(g_app_sensor_data_lock);

static void sensor_work_handler(struct k_work *work)
{
	int ret;

	float temperature = NAN;
	float humidity = NAN;
	float illuminance = NAN;
	int orientation = INT_MAX;

	ret = app_sht40_read(&temperature, &humidity);
	if (ret) {
		LOG_ERR("Call `app_sht40_read` failed: %d", ret);
	}

	ret = app_opt3001_read(&illuminance);
	if (ret) {
		LOG_ERR("Call `app_opt3001_read` failed: %d", ret);
	}

	ret = app_accel_read(NULL, NULL, NULL, &orientation);
	if (ret) {
		LOG_ERR("Call `app_accel_read` failed: %d", ret);
	}

#if defined(CONFIG_W1)
	int count = app_ds18b20_get_count();

	for (int i = 0; i < count; i++) {
		uint64_t serial_number;
		float temperature;
		ret = app_ds18b20_read(i, &serial_number, &temperature);
		if (ret) {
			LOG_ERR("Call `app_ds18b20_read` failed: %d", ret);
		} else {
			LOG_INF("Serial number: %llu / Temperature: %.2f C", serial_number,
				(double)temperature);
		}
	}
#endif

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.temperature = temperature;
	g_app_sensor_data.humidity = humidity;
	g_app_sensor_data.illuminance = illuminance;
	g_app_sensor_data.orientation = orientation;
	k_mutex_unlock(&g_app_sensor_data_lock);
}

static K_WORK_DEFINE(m_sensor_work, sensor_work_handler);

static void sensor_timer_handler(struct k_timer *timer)
{
	k_work_submit(&m_sensor_work);
}

static K_TIMER_DEFINE(m_sensor_timer, sensor_timer_handler, NULL);

static int init(void)
{
	int ret;

#if defined(CONFIG_W1)
	ret = app_ds18b20_scan();
	if (ret) {
		LOG_ERR("Call `app_ds18b20_scan` failed: %d", ret);
		return ret;
	}
#endif /* defined(CONFIG_W1) */

	k_timer_start(&m_sensor_timer, K_SECONDS(1), K_SECONDS(10));

	return 0;
}

SYS_INIT(init, APPLICATION, 99);
