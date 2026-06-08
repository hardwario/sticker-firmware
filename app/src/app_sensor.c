/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_accel.h"
#include "app_alarm.h"
#include "app_battery.h"
#include "app_config.h"
#include "app_ds18b20.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_log.h"
#include "app_machine_probe.h"
#include "app_mpl3115a2.h"
#include "app_opt3001.h"
#include "app_pyq1648.h"
#include "app_sensor.h"
#include "app_sht4x.h"
#include "app_w1_slots.h"

/* Zephyr includes */
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

	app_alarm_event(APP_ALARM_SOURCE_PIR_MOTION, true);
}

int app_sensor_init(void)
{
	int ret;
	int res = 0;

	if (g_app_config.cap_light_sensor) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(opt3001));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "opt3001", ret);
			res = res ? res : ret;
		}
	}

	if (g_app_config.cap_barometer) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(mpl3115a2));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "mpl3115a2", ret);
			res = res ? res : ret;
		}
	}

	if (g_app_config.cap_hall_left || g_app_config.cap_hall_right) {
		ret = app_hall_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_hall_init", ret);
			res = res ? res : ret;
		}
	}

	if ((g_app_config.cap_input_a || g_app_config.cap_input_b) &&
	    g_app_config.cap_pir_detector) {
		LOG_WRN("PIR and input share GPIO pins — skipping input init");
	} else if (g_app_config.cap_input_a || g_app_config.cap_input_b) {
		ret = app_input_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_input_init", ret);
			res = res ? res : ret;
		}
	}

	if (g_app_config.cap_pir_detector) {
		ret = app_pyq1648_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_pyq1648_init", ret);
			res = res ? res : ret;
		} else {
			app_pyq1648_set_callback(pyq1648_event_handler, NULL);
		}
	}

	if (g_app_config.cap_1w_thermometer || g_app_config.cap_1w_machine_probe) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds2484", ret);
			res = res ? res : ret;
		}
	}

	if (g_app_config.cap_1w_thermometer) {
		const struct device *dev_0 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_0));

		ret = device_init(dev_0);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds18b20_0", ret);
			res = res ? res : ret;
		}

		const struct device *dev_1 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_1));

		ret = device_init(dev_1);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "ds18b20_1", ret);
			res = res ? res : ret;
		}

		ret = app_ds18b20_scan();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_ds18b20_scan", ret);
			res = res ? res : ret;
		}
	}

	if (g_app_config.cap_1w_machine_probe) {
		const struct device *dev_0 = DEVICE_DT_GET(DT_NODELABEL(machine_probe_0));

		ret = device_init(dev_0);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "machine_probe_0", ret);
			res = res ? res : ret;
		}

		const struct device *dev_1 = DEVICE_DT_GET(DT_NODELABEL(machine_probe_1));

		ret = device_init(dev_1);
		if (ret) {
			LOG_ERR_CALL_FAILED_CTX_INT("device_init", "machine_probe_1", ret);
			res = res ? res : ret;
		}

		ret = app_machine_probe_scan();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_machine_probe_scan", ret);
			res = res ? res : ret;
		}

		int count = app_machine_probe_get_count();

		for (int i = 0; i < count; i++) {
			uint64_t serial_number;
			ret = app_machine_probe_enable_tilt_alert(i, &serial_number, TILT_THRESHOLD,
								  TILT_DURATION);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_machine_probe_enable_tilt_alert", ret);
				res = res ? res : ret;
			}
		}
	}

	/* Bind discovered 1-Wire devices to logical slots by ROM (stable identity,
	 * replaces discovery-order). Must run after both driver scans. */
	if (g_app_config.cap_1w_thermometer || g_app_config.cap_1w_machine_probe) {
		int present = app_w1_slots_rebind();
		LOG_INF("1-Wire slots: %d sensor(s) bound", present);
	}

	k_work_queue_init(&m_sensor_work_q);

	k_work_queue_start(&m_sensor_work_q, m_sensor_work_stack,
			   K_THREAD_STACK_SIZEOF(m_sensor_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	if (g_app_config.interval_sample) {
		k_timer_start(&m_sensor_timer, K_SECONDS(1),
			      K_SECONDS(g_app_config.interval_sample));
	}

	return res;
}

void app_sensor_sample(void)
{
	int ret;

	int orientation = INT_MAX;
	float voltage = NAN;

	float temperature = NAN;
	float humidity = NAN;
	float illuminance = NAN;
	float altitude = NAN;
	float pressure = NAN;

	struct app_hall_data hall_data = {0};
	struct app_input_data input_data = {0};

	float t1_temperature = NAN;
	float t2_temperature = NAN;

	float mp1_temperature = NAN;
	float mp2_temperature = NAN;
	float mp1_humidity = NAN;
	float mp2_humidity = NAN;
	bool mp1_is_tilt_alert = false;
	bool mp2_is_tilt_alert = false;

	struct app_w1_slot_reading w1_local[APP_W1_SLOT_COUNT] = {0};

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
	ret = app_sht4x_read(&temperature, &humidity);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_sht4x_read", ret);
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

	if (g_app_config.cap_input_a || g_app_config.cap_input_b) {
		ret = app_input_get_data(&input_data);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_input_get_data", ret);
		}
	}

	/* Read 1-Wire sensors through the ROM-bound slot table (stable identity,
	 * replaces discovery-order). Dallas slots fill the legacy t1/t2 fields,
	 * machine-probe slots fill mp1/mp2 — P1 keeps the existing telemetry/alarm
	 * fields; the full w1[] migration is P3. */
	if (g_app_config.cap_1w_thermometer || g_app_config.cap_1w_machine_probe) {
		int dallas_n = 0;
		int probe_n = 0;

		for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
			struct app_w1_slot_reading r;

			int rret = app_w1_slots_read(s, &r);

			w1_local[s] = r; /* keep per-slot reading (incl. present flag) */
			if (rret != 0 || !r.present) {
				continue;
			}

			switch (app_w1_slot_get_type(s)) {
			case APP_W1_SLOT_DALLAS:
				LOG_INF("Slot %d (dallas) / Temperature: %.2f C", s + 1,
					(double)r.temperature);
				if (dallas_n == 0) {
					t1_temperature = r.temperature;
				} else if (dallas_n == 1) {
					t2_temperature = r.temperature;
				}
				dallas_n++;
				break;
			case APP_W1_SLOT_MACHINE_PROBE:
				LOG_INF("Slot %d (machine-probe) / %.2f C / %.1f %% / tilt "
					"%sactive",
					s + 1, (double)r.temperature, (double)r.humidity,
					r.is_tilt_alert ? "" : "not ");
				if (probe_n == 0) {
					mp1_temperature = r.temperature;
					mp1_humidity = r.humidity;
					mp1_is_tilt_alert = r.is_tilt_alert;
				} else if (probe_n == 1) {
					mp2_temperature = r.temperature;
					mp2_humidity = r.humidity;
					mp2_is_tilt_alert = r.is_tilt_alert;
				}
				probe_n++;
				break;
			default:
				break;
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

	g_app_sensor_data.input_a_count = input_data.input_a_count;
	g_app_sensor_data.input_b_count = input_data.input_b_count;
	g_app_sensor_data.input_a_is_active = input_data.input_a_is_active;
	g_app_sensor_data.input_b_is_active = input_data.input_b_is_active;

	g_app_sensor_data.t1_temperature = t1_temperature + g_app_config.corr_t1_temperature;
	g_app_sensor_data.t2_temperature = t2_temperature + g_app_config.corr_t2_temperature;

	g_app_sensor_data.mp1_temperature = mp1_temperature;
	g_app_sensor_data.mp2_temperature = mp2_temperature;
	g_app_sensor_data.mp1_humidity = mp1_humidity;
	g_app_sensor_data.mp2_humidity = mp2_humidity;
	g_app_sensor_data.mp1_is_tilt_alert = mp1_is_tilt_alert;
	g_app_sensor_data.mp2_is_tilt_alert = mp2_is_tilt_alert;

	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		g_app_sensor_data.w1[s] = w1_local[s];
	}

	k_mutex_unlock(&g_app_sensor_data_lock);
}
