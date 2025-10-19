/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_accel.h"
#include "app_battery.h"
#include "app_config.h"
#include "app_ds18b20.h"
#include "app_hall.h"
#include "app_led.h"
#include "app_log.h"
#include "app_machine_probe.h"
#include "app_mpl3115a2.h"
#include "app_opt3001.h"
#include "app_pyq1648.h"
#include "app_sensor.h"
#include "app_sht40.h"

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_sensor, LOG_LEVEL_DBG);

#define TILT_THRESHOLD 7
#define TILT_DURATION  1

struct app_sensor_data g_app_sensor_data = {
	.orientation = INT_MAX,
	.voltage = NAN,
	.temperature = NAN,
	.humidity = NAN,
	.illuminance = NAN,
	.altitude = NAN,
	.pressure = NAN,
	.t1_temperature = NAN,
	.t2_temperature = NAN,
	.mp1_temperature = NAN,
	.mp2_temperature = NAN,
	.mp1_humidity = NAN,
	.mp2_humidity = NAN,
};

K_MUTEX_DEFINE(g_app_sensor_data_lock);

static K_THREAD_STACK_DEFINE(m_sensor_work_stack, 2048);
static struct k_work_q m_sensor_work_q;

void app_sensor_sample(void)
{
	int ret;
	int count;
	UNUSED(ret);
	UNUSED(count);

	int orientation = INT_MAX;
	float voltage = NAN;

	float temperature = NAN;
	float humidity = NAN;
	float illuminance = NAN;
	float altitude = NAN;
	float pressure = NAN;

	struct app_hall_data hall_data = {0};

	float t1_temperature = NAN;
	float t2_temperature = NAN;

	float mp1_temperature = NAN;
	float mp2_temperature = NAN;
	float mp1_humidity = NAN;
	float mp2_humidity = NAN;
	bool mp1_is_tilt_alert = false;
	bool mp2_is_tilt_alert = false;

#if defined(CONFIG_ADC)
	ret = app_battery_measure(&voltage);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_battery_measure", ret);
	}
#endif /* defined(CONFIG_ADC) */

#if defined(CONFIG_LIS2DH)
	ret = app_accel_read(NULL, NULL, NULL, &orientation);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_accel_read", ret);
	}
#endif /* defined(CONFIG_LIS2DH) */

#if defined(CONFIG_SHT4X)
	ret = app_sht40_read(&temperature, &humidity);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_sht40_read", ret);
	}
#endif /* defined(CONFIG_SHT4X) */

	if (g_app_config.cap_light_sensor) {
		ret = app_opt3001_read(&illuminance);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_opt3001_read", ret);
		}
	}

	if (g_app_config.cap_barometer) {
		ret = app_mpl3115a2_read(&altitude, &pressure, NULL);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_mpl3115a2_read", ret);
		}
	}

	if (g_app_config.cap_hall_left || g_app_config.cap_hall_right) {
		ret = app_hall_get_data(&hall_data);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_hall_get_data", ret);
		}
	}

	if (g_app_config.cap_1w_thermometer) {
		count = app_ds18b20_get_count();

		for (int i = 0; i < count; i++) {
			uint64_t serial_number;
			float temperature;
			ret = app_ds18b20_read(i, &serial_number, &temperature);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_ds18b20_read", ret);
				continue;
			}

			LOG_INF("Serial number: %llu / Temperature: %.2f C", serial_number,
				(double)temperature);

			if (i == 0) {
				t1_temperature = temperature;
			} else if (i == 1) {
				t2_temperature = temperature;
			}
		}
	}

	if (g_app_config.cap_1w_machine_probe) {
		count = app_machine_probe_get_count();

		for (int i = 0; i < count; i++) {
			uint64_t serial_number;
			float hygrometer_temperature;
			float hygrometer_humidity;
			bool is_tilt_alert;
			ret = app_machine_probe_read_hygrometer(
				i, &serial_number, &hygrometer_temperature, &hygrometer_humidity);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_machine_probe_read_hygrometer", ret);
				continue;
			}

			ret = app_machine_probe_get_tilt_alert(i, &serial_number, &is_tilt_alert);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_machine_probe_get_tilt_alert", ret);
				continue;
			}

			LOG_INF("Serial number: %llu / Hygrometer / Temperature: "
				"%.2f C",
				serial_number, (double)hygrometer_temperature);
			LOG_INF("Serial number: %llu / Hygrometer / Humidity: %.1f "
				"%%",
				serial_number, (double)hygrometer_humidity);
			LOG_INF("Serial number: %llu / Tilt alert is %sactive", serial_number,
				is_tilt_alert ? "" : "not ");

			if (i == 0) {
				mp1_temperature = hygrometer_temperature;
				mp1_humidity = hygrometer_humidity;
				mp1_is_tilt_alert = is_tilt_alert;
			} else if (i == 1) {
				mp2_temperature = hygrometer_temperature;
				mp2_humidity = hygrometer_humidity;
				mp2_is_tilt_alert = is_tilt_alert;
			}
		}
	}

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	g_app_sensor_data.orientation = orientation;
	g_app_sensor_data.voltage = voltage;

	g_app_sensor_data.temperature = temperature + g_app_config.corr_temperature;
	g_app_sensor_data.humidity = humidity;
	g_app_sensor_data.illuminance = illuminance;
	g_app_sensor_data.altitude = altitude;
	g_app_sensor_data.pressure = pressure;

	g_app_sensor_data.hall_left_count = hall_data.left_count;
	g_app_sensor_data.hall_right_count = hall_data.right_count;
	g_app_sensor_data.hall_left_is_active = hall_data.left_is_active;
	g_app_sensor_data.hall_right_is_active = hall_data.right_is_active;

	g_app_sensor_data.t1_temperature = t1_temperature + g_app_config.corr_t1_temperature;
	g_app_sensor_data.t2_temperature = t2_temperature + g_app_config.corr_t2_temperature;

	g_app_sensor_data.mp1_temperature = mp1_temperature;
	g_app_sensor_data.mp2_temperature = mp2_temperature;
	g_app_sensor_data.mp1_humidity = mp1_humidity;
	g_app_sensor_data.mp2_humidity = mp2_humidity;
	g_app_sensor_data.mp1_is_tilt_alert = mp1_is_tilt_alert;
	g_app_sensor_data.mp2_is_tilt_alert = mp2_is_tilt_alert;

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

static void pyq1648_event_handler(void *user_data)
{
	LOG_INF("Motion detected");

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	g_app_sensor_data.motion_count++;
	k_mutex_unlock(&g_app_sensor_data_lock);

	struct app_led_blink_req req = {
		.color = APP_LED_CHANNEL_Y, .duration = 5, .space = 0, .repetitions = 1};
	app_led_blink(&req);
}

static int init(void)
{
	int ret;
	UNUSED(ret);

	if (g_app_config.cap_light_sensor) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(opt3001));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "opt3001", ret);
			return ret;
		}
	}

	if (g_app_config.cap_barometer) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(mpl3115a2));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "mpl3115a2", ret);
			return ret;
		}
	}

	if (g_app_config.cap_hall_left || g_app_config.cap_hall_right) {
		ret = app_hall_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_hall_init", ret);
			return ret;
		}
	}

	if (g_app_config.cap_pir_detector) {
		ret = app_pyq1648_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_pyq1648_init", ret);
			return ret;
		}

		app_pyq1648_set_callback(pyq1648_event_handler, NULL);
	}

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

		int count = app_machine_probe_get_count();

		for (int i = 0; i < count; i++) {
			uint64_t serial_number;
			ret = app_machine_probe_enable_tilt_alert(i, &serial_number, TILT_THRESHOLD,
								  TILT_DURATION);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_machine_probe_enable_tilt_alert", ret);
				return ret;
			}
		}
	}

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
