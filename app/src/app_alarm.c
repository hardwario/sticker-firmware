/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_sensor.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stdbool.h>

LOG_MODULE_REGISTER(app_alarm, LOG_LEVEL_DBG);

#define ALARM_INT_TEMP_THR_LO   (CONFIG_APP_ALARM_INT_TEMP_THR_LO_MUL_100 / 100.f)
#define ALARM_INT_TEMP_THR_HI   (CONFIG_APP_ALARM_INT_TEMP_THR_HI_MUL_100 / 100.f)
#define ALARM_INT_TEMP_HST      (CONFIG_APP_ALARM_INT_TEMP_HST_MUL_100 / 100.f)
#define ALARM_EXT_TEMP_1_THR_LO (CONFIG_APP_ALARM_EXT_TEMP_1_THR_LO_MUL_100 / 100.f)
#define ALARM_EXT_TEMP_1_THR_HI (CONFIG_APP_ALARM_EXT_TEMP_1_THR_HI_MUL_100 / 100.f)
#define ALARM_EXT_TEMP_1_HST    (CONFIG_APP_ALARM_EXT_TEMP_1_HST_MUL_100 / 100.f)
#define ALARM_EXT_TEMP_2_THR_LO (CONFIG_APP_ALARM_EXT_TEMP_2_THR_LO_MUL_100 / 100.f)
#define ALARM_EXT_TEMP_2_THR_HI (CONFIG_APP_ALARM_EXT_TEMP_2_THR_HI_MUL_100 / 100.f)
#define ALARM_EXT_TEMP_2_HST    (CONFIG_APP_ALARM_EXT_TEMP_2_HST_MUL_100 / 100.f)
#define ALARM_HUMIDITY_THR_LO   (CONFIG_APP_ALARM_HUMIDITY_THR_LO_MUL_10 / 10.f)
#define ALARM_HUMIDITY_THR_HI   (CONFIG_APP_ALARM_HUMIDITY_THR_HI_MUL_10 / 10.f)
#define ALARM_HUMIDITY_HST      (CONFIG_APP_ALARM_HUMIDITY_HST_MUL_10 / 10.f)
#define ALARM_PRESSURE_THR_LO   (CONFIG_APP_ALARM_PRESSURE_THR_LO)
#define ALARM_PRESSURE_THR_HI   (CONFIG_APP_ALARM_PRESSURE_THR_HI)
#define ALARM_PRESSURE_HST      (CONFIG_APP_ALARM_PRESSURE_HST)

bool app_alarm_is_active(void)
{
	static bool alarm_int_temp = false;

	if (isnan(g_app_sensor_data.temperature)) {
		alarm_int_temp = false;
	} else if (alarm_int_temp) {
		if (g_app_sensor_data.temperature > (ALARM_INT_TEMP_THR_LO + ALARM_INT_TEMP_HST) &&
		    g_app_sensor_data.temperature < (ALARM_INT_TEMP_THR_HI - ALARM_INT_TEMP_HST)) {
			LOG_INF("Deactivated alarm for internal temperature");

			alarm_int_temp = false;
		}
	} else {
		if (g_app_sensor_data.temperature < (ALARM_INT_TEMP_THR_LO - ALARM_INT_TEMP_HST) ||
		    g_app_sensor_data.temperature > (ALARM_INT_TEMP_THR_HI + ALARM_INT_TEMP_HST)) {
			LOG_INF("Activated alarm for internal temperature");

			alarm_int_temp = true;
		}
	}

	static bool alarm_ext_temp_1 = false;

	if (isnan(g_app_sensor_data.ext_temperature_1)) {
		alarm_ext_temp_1 = false;
	} else if (alarm_ext_temp_1) {
		if (g_app_sensor_data.ext_temperature_1 >
			    (ALARM_EXT_TEMP_1_THR_LO + ALARM_EXT_TEMP_1_HST) &&
		    g_app_sensor_data.ext_temperature_1 <
			    (ALARM_EXT_TEMP_1_THR_HI - ALARM_EXT_TEMP_1_HST)) {
			LOG_INF("Deactivated alarm for external temperature 1");

			alarm_ext_temp_1 = false;
		}
	} else {
		if (g_app_sensor_data.ext_temperature_1 <
			    (ALARM_EXT_TEMP_1_THR_LO - ALARM_EXT_TEMP_1_HST) ||
		    g_app_sensor_data.ext_temperature_1 >
			    (ALARM_EXT_TEMP_1_THR_HI + ALARM_EXT_TEMP_1_HST)) {
			LOG_INF("Activated alarm for external temperature 1");

			alarm_ext_temp_1 = true;
		}
	}

	static bool alarm_ext_temp_2 = false;

	if (isnan(g_app_sensor_data.ext_temperature_2)) {
		alarm_ext_temp_2 = false;
	} else if (alarm_ext_temp_2) {
		if (g_app_sensor_data.ext_temperature_2 >
			    (ALARM_EXT_TEMP_2_THR_LO + ALARM_EXT_TEMP_2_HST) &&
		    g_app_sensor_data.ext_temperature_2 <
			    (ALARM_EXT_TEMP_2_THR_HI - ALARM_EXT_TEMP_2_HST)) {
			LOG_INF("Deactivated alarm for external temperature 2");

			alarm_ext_temp_2 = false;
		}
	} else {
		if (g_app_sensor_data.ext_temperature_2 <
			    (ALARM_EXT_TEMP_2_THR_LO - ALARM_EXT_TEMP_2_HST) ||
		    g_app_sensor_data.ext_temperature_2 >
			    (ALARM_EXT_TEMP_2_THR_HI + ALARM_EXT_TEMP_2_HST)) {
			LOG_INF("Activated alarm for external temperature 2");

			alarm_ext_temp_2 = true;
		}
	}

	static bool alarm_humidity = false;

	if (isnan(g_app_sensor_data.humidity)) {
		alarm_humidity = false;
	} else if (alarm_humidity) {
		if (g_app_sensor_data.humidity > (ALARM_HUMIDITY_THR_LO + ALARM_HUMIDITY_HST) &&
		    g_app_sensor_data.humidity < (ALARM_HUMIDITY_THR_HI - ALARM_HUMIDITY_HST)) {
			LOG_INF("Deactivated alarm for humidity");

			alarm_humidity = false;
		}
	} else {
		if (g_app_sensor_data.humidity < (ALARM_HUMIDITY_THR_LO - ALARM_HUMIDITY_HST) ||
		    g_app_sensor_data.humidity > (ALARM_HUMIDITY_THR_HI + ALARM_HUMIDITY_HST)) {
			LOG_INF("Activated alarm for humidity");

			alarm_humidity = true;
		}
	}

	static bool alarm_pressure = false;

	if (isnan(g_app_sensor_data.pressure)) {
		alarm_pressure = false;
	} else if (alarm_pressure) {
		if (g_app_sensor_data.pressure / 100.f > (ALARM_PRESSURE_THR_LO + ALARM_PRESSURE_HST) &&
		    g_app_sensor_data.pressure / 100.f < (ALARM_PRESSURE_THR_HI - ALARM_PRESSURE_HST)) {
			LOG_INF("Deactivated alarm for pressure");

			alarm_pressure = false;
		}
	} else {
		if (g_app_sensor_data.pressure / 100.f < (ALARM_PRESSURE_THR_LO - ALARM_PRESSURE_HST) ||
		    g_app_sensor_data.pressure / 100.f > (ALARM_PRESSURE_THR_HI + ALARM_PRESSURE_HST)) {
			LOG_INF("Activated alarm for pressure");

			alarm_pressure = true;
		}
	}

	bool alarm = false;

#if defined(CONFIG_APP_ALARM_INT_TEMP)
	alarm = alarm_int_temp ? true : alarm;
#endif /* defined(CONFIG_APP_ALARM_INT_TEMP) */

#if defined(CONFIG_APP_ALARM_EXT_TEMP)
	alarm = alarm_ext_temp_1 ? true : alarm;
	alarm = alarm_ext_temp_2 ? true : alarm;
#endif /* defined(CONFIG_APP_ALARM_EXT_TEMP) */

#if defined(CONFIG_APP_ALARM_HUMIDITY)
	alarm = alarm_humidity ? true : alarm;
#endif /* defined(CONFIG_APP_ALARM_HUMIDITY) */

#if defined(CONFIG_APP_ALARM_PRESSURE)
	alarm = alarm_pressure ? true : alarm;
#endif /* defined(CONFIG_APP_ALARM_PRESSURE) */

	return alarm;
}
