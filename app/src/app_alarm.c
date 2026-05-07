/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_config.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_sensor.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_alarm, LOG_LEVEL_DBG);

#define APP_ALARM_BOTH_BOOL_RED_HOLD_MS 10000
#define APP_ALARM_SOURCE_COUNT          5

static bool m_alarm_active[APP_ALARM_SOURCE_COUNT];
static int64_t m_both_bool_expiry_ms[APP_ALARM_SOURCE_COUNT];
static app_alarm_event_cb m_event_cb;
static void *m_event_cb_user_data;

K_MUTEX_DEFINE(m_lock);

static int read_notify_bools(enum app_alarm_source source, bool *act, bool *deact)
{
	switch (source) {
	case APP_ALARM_SOURCE_HALL_LEFT:
		*act = g_app_config.hall_left_notify_act;
		*deact = g_app_config.hall_left_notify_deact;
		return 0;
	case APP_ALARM_SOURCE_HALL_RIGHT:
		*act = g_app_config.hall_right_notify_act;
		*deact = g_app_config.hall_right_notify_deact;
		return 0;
	case APP_ALARM_SOURCE_INPUT_A:
		*act = g_app_config.input_a_notify_act;
		*deact = g_app_config.input_a_notify_deact;
		return 0;
	case APP_ALARM_SOURCE_INPUT_B:
		*act = g_app_config.input_b_notify_act;
		*deact = g_app_config.input_b_notify_deact;
		return 0;
	case APP_ALARM_SOURCE_PIR_MOTION:
		*act = false;
		*deact = false;
		return 0;
	default:
		return -EINVAL;
	}
}

bool app_alarm_is_active(void)
{
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	static bool alarm_temperature = false;

	if (!g_app_config.alarm_temperature_enabled) {
		alarm_temperature = false;
	} else if (isnan(g_app_sensor_data.temperature)) {
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

	if (!g_app_config.alarm_humidity_enabled) {
		alarm_humidity = false;
	} else if (isnan(g_app_sensor_data.humidity)) {
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

	if (!g_app_config.alarm_pressure_enabled) {
		alarm_pressure = false;
	} else if (isnan(g_app_sensor_data.pressure)) {
		alarm_pressure = false;
	} else if (alarm_pressure) {
		if (g_app_sensor_data.pressure * 10.f >
			    (g_app_config.alarm_pressure_lo + g_app_config.alarm_pressure_hst) &&
		    g_app_sensor_data.pressure * 10.f <
			    (g_app_config.alarm_pressure_hi - g_app_config.alarm_pressure_hst)) {
			LOG_INF("Deactivated alarm for pressure");

			alarm_pressure = false;
		}
	} else {
		if (g_app_sensor_data.pressure * 10.f <
			    (g_app_config.alarm_pressure_lo - g_app_config.alarm_pressure_hst) ||
		    g_app_sensor_data.pressure * 10.f >
			    (g_app_config.alarm_pressure_hi + g_app_config.alarm_pressure_hst)) {
			LOG_INF("Activated alarm for pressure");

			alarm_pressure = true;
		}
	}

	static bool alarm_t1_temperature = false;

	if (!g_app_config.alarm_t1_temperature_enabled) {
		alarm_t1_temperature = false;
	} else if (isnan(g_app_sensor_data.t1_temperature)) {
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

	if (!g_app_config.alarm_t2_temperature_enabled) {
		alarm_t2_temperature = false;
	} else if (isnan(g_app_sensor_data.t2_temperature)) {
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

	k_mutex_unlock(&g_app_sensor_data_lock);

	return alarm;
}

void app_alarm_event(enum app_alarm_source source, bool active)
{
	bool notify_act = false;
	bool notify_deact = false;
	int ret;

	ret = read_notify_bools(source, &notify_act, &notify_deact);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("read_notify_bools", ret);
		return;
	}

	bool send = (active && notify_act) || (!active && notify_deact);

	k_mutex_lock(&m_lock, K_FOREVER);

	if (notify_act && notify_deact) {
		m_alarm_active[source] = true;
		m_both_bool_expiry_ms[source] = k_uptime_get() + APP_ALARM_BOTH_BOOL_RED_HOLD_MS;
	} else if (notify_act) {
		m_alarm_active[source] = active;
		m_both_bool_expiry_ms[source] = 0;
	} else if (notify_deact) {
		m_alarm_active[source] = !active;
		m_both_bool_expiry_ms[source] = 0;
	} else {
		m_alarm_active[source] = false;
		m_both_bool_expiry_ms[source] = 0;
	}

	app_alarm_event_cb cb = m_event_cb;
	void *user_data = m_event_cb_user_data;

	k_mutex_unlock(&m_lock);

	if (send && source != APP_ALARM_SOURCE_PIR_MOTION) {
#if defined(CONFIG_LORAWAN)
		app_lrw_send();
#endif /* defined(CONFIG_LORAWAN) */
	}

	if (cb) {
		cb(source, active, user_data);
	}
}

int app_alarm_set_event_callback(app_alarm_event_cb cb, void *user_data)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	m_event_cb = cb;
	m_event_cb_user_data = user_data;
	k_mutex_unlock(&m_lock);

	return 0;
}
