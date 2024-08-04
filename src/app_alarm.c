/*
 * Copyright (c) 2024 HARDWARIO a.s.
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

#define ALARM_TEMPERATURE_THR_LO 18.f
#define ALARM_TEMPERATURE_THR_HI 35.f
#define ALARM_TEMPERATURE_HST    0.5f

#define ALARM_EXT_TEMPERATURE_1_THR_LO 2.f
#define ALARM_EXT_TEMPERATURE_1_THR_HI 8.f
#define ALARM_EXT_TEMPERATURE_1_HST    0.5f

#define ALARM_EXT_TEMPERATURE_2_THR_LO 2.f
#define ALARM_EXT_TEMPERATURE_2_THR_HI 8.f
#define ALARM_EXT_TEMPERATURE_2_HST    0.5f

bool app_alarm_is_active(void)
{
	static bool alarm_temperature = false;

	if (isnan(g_app_sensor_data.temperature)) {
		alarm_temperature = false;
	} else if (alarm_temperature) {
		if (g_app_sensor_data.temperature >
			    (ALARM_TEMPERATURE_THR_LO + ALARM_TEMPERATURE_HST) &&
		    g_app_sensor_data.temperature <
			    (ALARM_TEMPERATURE_THR_HI - ALARM_TEMPERATURE_HST)) {
			LOG_INF("Deactivated alarm for temperature");

			alarm_temperature = false;
		}
	} else {
		if (g_app_sensor_data.temperature <
			    (ALARM_TEMPERATURE_THR_LO - ALARM_TEMPERATURE_HST) ||
		    g_app_sensor_data.temperature >
			    (ALARM_TEMPERATURE_THR_HI + ALARM_TEMPERATURE_HST)) {
			LOG_INF("Activated alarm for temperature");

			alarm_temperature = true;
		}
	}

	static bool alarm_ext_temperature_1 = false;

	if (isnan(g_app_sensor_data.ext_temperature_1)) {
		alarm_ext_temperature_1 = false;
	} else if (alarm_ext_temperature_1) {
		if (g_app_sensor_data.ext_temperature_1 >
			    (ALARM_EXT_TEMPERATURE_1_THR_LO + ALARM_EXT_TEMPERATURE_1_HST) &&
		    g_app_sensor_data.ext_temperature_1 <
			    (ALARM_EXT_TEMPERATURE_1_THR_HI - ALARM_EXT_TEMPERATURE_1_HST)) {
			LOG_INF("Deactivated alarm for external temperature 1");

			alarm_ext_temperature_1 = false;
		}
	} else {
		if (g_app_sensor_data.ext_temperature_1 <
			    (ALARM_EXT_TEMPERATURE_1_THR_LO - ALARM_EXT_TEMPERATURE_1_HST) ||
		    g_app_sensor_data.ext_temperature_1 >
			    (ALARM_EXT_TEMPERATURE_1_THR_HI + ALARM_EXT_TEMPERATURE_1_HST)) {
			LOG_INF("Activated alarm for external temperature 1");

			alarm_ext_temperature_1 = true;
		}
	}

	static bool alarm_ext_temperature_2 = false;

	if (isnan(g_app_sensor_data.ext_temperature_2)) {
		alarm_ext_temperature_2 = false;
	} else if (alarm_ext_temperature_2) {
		if (g_app_sensor_data.ext_temperature_2 >
			    (ALARM_EXT_TEMPERATURE_2_THR_LO + ALARM_EXT_TEMPERATURE_2_HST) &&
		    g_app_sensor_data.ext_temperature_2 <
			    (ALARM_EXT_TEMPERATURE_2_THR_HI - ALARM_EXT_TEMPERATURE_2_HST)) {
			LOG_INF("Deactivated alarm for external temperature 2");

			alarm_ext_temperature_2 = false;
		}
	} else {
		if (g_app_sensor_data.ext_temperature_2 <
			    (ALARM_EXT_TEMPERATURE_2_THR_LO - ALARM_EXT_TEMPERATURE_2_HST) ||
		    g_app_sensor_data.ext_temperature_2 >
			    (ALARM_EXT_TEMPERATURE_2_THR_HI + ALARM_EXT_TEMPERATURE_2_HST)) {
			LOG_INF("Activated alarm for external temperature 2");

			alarm_ext_temperature_2 = true;
		}
	}

	return alarm_temperature || alarm_ext_temperature_1 || alarm_ext_temperature_2;
}
