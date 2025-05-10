/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_accel.h"
#include "app_battery.h"
#include "app_config.h"
#include "app_ds18b20.h"
#include "app_led.h"
#include "app_opt3001.h"
#include "app_pyq1648.h"
#include "app_sensor.h"
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
	.orientation = INT_MAX,
	.voltage = NAN,
	.temperature = NAN,
	.humidity = NAN,
	.illuminance = NAN,
	.ext_temperature_1 = NAN,
	.ext_temperature_2 = NAN,
#if defined(CONFIG_APP_PROFILE_STICKER_MOTION)
	.motion_count = 0,
#endif /* defined(CONFIG_APP_PROFILE_STICKER_MOTION) */
};

K_MUTEX_DEFINE(g_app_sensor_data_lock);

static K_THREAD_STACK_DEFINE(m_sensor_work_stack, 4096);
static struct k_work_q m_sensor_work_q;

void app_sensor_sample(void)
{
	int ret;
	UNUSED(ret);

	int orientation = INT_MAX;
	float voltage = NAN;
	float temperature = NAN;
	float humidity = NAN;
	float illuminance = NAN;
	float ext_temperature_1 = NAN;
	float ext_temperature_2 = NAN;

#if defined(CONFIG_ADC)
	ret = app_battery_measure(&voltage);
	if (ret) {
		LOG_ERR("Call `app_battery_measure` failed: %d", ret);
	}
#endif /* defined(CONFIG_ADC) */

#if defined(CONFIG_LIS2DH)
	ret = app_accel_read(NULL, NULL, NULL, &orientation);
	if (ret) {
		LOG_ERR("Call `app_accel_read` failed: %d", ret);
	}
#endif /* defined(CONFIG_LIS2DH) */

#if defined(CONFIG_SHT4X)
	ret = app_sht40_read(&temperature, &humidity);
	if (ret) {
		LOG_ERR("Call `app_sht40_read` failed: %d", ret);
	}
#endif /* defined(CONFIG_SHT4X) */

#if defined(CONFIG_OPT3001)
	ret = app_opt3001_read(&illuminance);
	if (ret) {
		LOG_ERR("Call `app_opt3001_read` failed: %d", ret);
	}
#endif /* defined(CONFIG_OPT3001) */

#if defined(CONFIG_W1)
	int count = app_ds18b20_get_count();

	for (int i = 0; i < count; i++) {
		uint64_t serial_number;
		float temperature;
		ret = app_ds18b20_read(i, &serial_number, &temperature);
		if (ret) {
			LOG_ERR("Call `app_ds18b20_read` failed: %d", ret);
			continue;
		}

		LOG_INF("Serial number: %llu / Temperature: %.2f C", serial_number,
			(double)temperature);

		if (i == 0) {
			ext_temperature_1 = temperature;
		} else if (i == 1) {
			ext_temperature_2 = temperature;
		}
	}
#endif /* defined(CONFIG_W1) */

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	g_app_sensor_data.orientation = orientation;
	g_app_sensor_data.voltage = voltage;
	g_app_sensor_data.temperature = temperature + g_app_config.corr_temperature;
	g_app_sensor_data.humidity = humidity;
	g_app_sensor_data.illuminance = illuminance;
	g_app_sensor_data.ext_temperature_1 =
		ext_temperature_1 + g_app_config.corr_ext_temperature_1;
	g_app_sensor_data.ext_temperature_2 =
		ext_temperature_2 + g_app_config.corr_ext_temperature_2;

	k_mutex_unlock(&g_app_sensor_data_lock);
}

static void sensor_work_handler(struct k_work *work)
{
	app_sensor_sample();
}

static K_WORK_DEFINE(m_sensor_work, sensor_work_handler);

static void sensor_timer_handler(struct k_timer *timer)
{
	k_work_submit_to_queue(&m_sensor_work_q, &m_sensor_work);
}

static K_TIMER_DEFINE(m_sensor_timer, sensor_timer_handler, NULL);

#if defined(CONFIG_APP_PROFILE_STICKER_MOTION)

static void pyq1648_event_handler(void *user_data)
{
	LOG_INF("Motion detected");

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.motion_count++;
	k_mutex_unlock(&g_app_sensor_data_lock);

	app_led_set(APP_LED_CHANNEL_Y, 1);
	k_sleep(K_MSEC(5));
	app_led_set(APP_LED_CHANNEL_Y, 0);
}

#endif /* defined(CONFIG_APP_PROFILE_STICKER_MOTION) */

static int init(void)
{
	int ret;
	UNUSED(ret);

#if defined(CONFIG_APP_PROFILE_STICKER_MOTION)
	app_pyq1648_set_callback(pyq1648_event_handler, NULL);
#endif /* defined(CONFIG_APP_PROFILE_STICKER_MOTION) */

#if defined(CONFIG_W1)
	ret = app_ds18b20_scan();
	if (ret) {
		LOG_ERR("Call `app_ds18b20_scan` failed: %d", ret);
		return ret;
	}
#endif /* defined(CONFIG_W1) */

	k_work_queue_init(&m_sensor_work_q);

	k_work_queue_start(&m_sensor_work_q, m_sensor_work_stack,
			   K_THREAD_STACK_SIZEOF(m_sensor_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	if (g_app_config.interval_sample) {
		k_timer_start(&m_sensor_timer, K_SECONDS(1),
			      K_SECONDS(g_app_config.interval_sample));
	}

	return 0;
}

SYS_INIT(init, APPLICATION, 99);
