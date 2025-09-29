/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_config.h"
#include "app_sensor.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stdbool.h>

LOG_MODULE_REGISTER(app_alarm, LOG_LEVEL_DBG);

bool app_alarm_is_active(void)
{
	static bool alarm_temperature = false;

	if (isnan(g_app_sensor_data.temperature)) {
		alarm_temperature = false;
	} else if (alarm_temperature) {
		if (g_app_sensor_data.temperature > (g_app_config.alarm_temperature_lo +
						     g_app_config.alarm_temperature_hst) &&
		    g_app_sensor_data.temperature < (g_app_config.alarm_temperature_hi -
						     g_app_config.alarm_temperature_hst)) {
			LOG_INF("Deactivated alarm for internal temperature");

			alarm_temperature = false;
		}
	} else {
		if (g_app_sensor_data.temperature < (g_app_config.alarm_temperature_lo -
						     g_app_config.alarm_temperature_hst) ||
		    g_app_sensor_data.temperature > (g_app_config.alarm_temperature_hi +
						     g_app_config.alarm_temperature_hst)) {
			LOG_INF("Activated alarm for internal temperature");

			alarm_temperature = true;
		}
	}

	static bool alarm_humidity = false;

	if (isnan(g_app_sensor_data.humidity)) {
		alarm_humidity = false;
	} else if (alarm_humidity) {
		if (g_app_sensor_data.humidity >
			    (g_app_config.alarm_humidity_lo + g_app_config.alarm_humidity_hst) &&
		    g_app_sensor_data.humidity <
			    (g_app_config.alarm_humidity_hi - g_app_config.alarm_humidity_hst)) {
			LOG_INF("Deactivated alarm for humidity");

			alarm_humidity = false;
		}
	} else {
		if (g_app_sensor_data.humidity <
			    (g_app_config.alarm_humidity_lo - g_app_config.alarm_humidity_hst) ||
		    g_app_sensor_data.humidity >
			    (g_app_config.alarm_humidity_hi + g_app_config.alarm_humidity_hst)) {
			LOG_INF("Activated alarm for humidity");

			alarm_humidity = true;
		}
	}

	static bool alarm_pressure = false;

	if (isnan(g_app_sensor_data.pressure)) {
		alarm_pressure = false;
	} else if (alarm_pressure) {
		if (g_app_sensor_data.pressure / 100.f >
			    (g_app_config.alarm_pressure_lo + g_app_config.alarm_pressure_hst) &&
		    g_app_sensor_data.pressure / 100.f <
			    (g_app_config.alarm_pressure_hi - g_app_config.alarm_pressure_hst)) {
			LOG_INF("Deactivated alarm for pressure");

			alarm_pressure = false;
		}
	} else {
		if (g_app_sensor_data.pressure / 100.f <
			    (g_app_config.alarm_pressure_lo - g_app_config.alarm_pressure_hst) ||
		    g_app_sensor_data.pressure / 100.f >
			    (g_app_config.alarm_pressure_hi + g_app_config.alarm_pressure_hst)) {
			LOG_INF("Activated alarm for pressure");

			alarm_pressure = true;
		}
	}

	static bool alarm_t1_temperature = false;

	if (isnan(g_app_sensor_data.t1_temperature)) {
		alarm_t1_temperature = false;
	} else if (alarm_t1_temperature) {
		if (g_app_sensor_data.t1_temperature > (g_app_config.alarm_t1_temperature_lo +
							g_app_config.alarm_t1_temperature_hst) &&
		    g_app_sensor_data.t1_temperature < (g_app_config.alarm_t1_temperature_hi -
							g_app_config.alarm_t1_temperature_hst)) {
			LOG_INF("Deactivated alarm for external temperature 1");

			alarm_t1_temperature = false;
		}
	} else {
		if (g_app_sensor_data.t1_temperature < (g_app_config.alarm_t1_temperature_lo -
							g_app_config.alarm_t1_temperature_hst) ||
		    g_app_sensor_data.t1_temperature > (g_app_config.alarm_t1_temperature_hi +
							g_app_config.alarm_t1_temperature_hst)) {
			LOG_INF("Activated alarm for external temperature 1");

			alarm_t1_temperature = true;
		}
	}

	static bool alarm_t2_temperature = false;

	if (isnan(g_app_sensor_data.t2_temperature)) {
		alarm_t2_temperature = false;
	} else if (alarm_t2_temperature) {
		if (g_app_sensor_data.t2_temperature > (g_app_config.alarm_t2_temperature_lo +
							g_app_config.alarm_t2_temperature_hst) &&
		    g_app_sensor_data.t2_temperature < (g_app_config.alarm_t2_temperature_hi -
							g_app_config.alarm_t2_temperature_hst)) {
			LOG_INF("Deactivated alarm for external temperature 2");

			alarm_t2_temperature = false;
		}
	} else {
		if (g_app_sensor_data.t2_temperature < (g_app_config.alarm_t2_temperature_lo -
							g_app_config.alarm_t2_temperature_hst) ||
		    g_app_sensor_data.t2_temperature > (g_app_config.alarm_t2_temperature_hi +
							g_app_config.alarm_t2_temperature_hst)) {
			LOG_INF("Activated alarm for external temperature 2");

			alarm_t2_temperature = true;
		}
	}

	bool alarm = false;

	if (g_app_config.alarm_temperature_enabled) {
		alarm = alarm_temperature ? true : alarm;
	}

	if (g_app_config.alarm_humidity_enabled) {
		alarm = alarm_humidity ? true : alarm;
	}

	if (g_app_config.alarm_pressure_enabled) {
		alarm = alarm_pressure ? true : alarm;
	}

	if (g_app_config.alarm_t1_temperature_enabled) {
		alarm = alarm_t1_temperature ? true : alarm;
	}

	if (g_app_config.alarm_t2_temperature_enabled) {
		alarm = alarm_t2_temperature ? true : alarm;
	}

	return alarm;
}
