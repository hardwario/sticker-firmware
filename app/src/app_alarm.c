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

/* Evaluate a single threshold with hysteresis. Updates *active to reflect the
 * latched state and sets *should_send = true on every Activated/Deactivated
 * edge. Returns the latched state for the caller to OR into the aggregate. */
static bool eval_threshold(bool *active, bool enabled, float value, float lo, float hi, float hst,
			   const char *name, bool *should_send)
{
	if (!enabled || isnan(value)) {
		*active = false;
		return false;
	}

	if (*active) {
		if (value > lo + hst && value < hi - hst) {
			LOG_INF("Deactivated alarm for %s", name);
			*active = false;
			*should_send = true;
		}
	} else {
		if (value < lo - hst || value > hi + hst) {
			LOG_INF("Activated alarm for %s", name);
			*active = true;
			*should_send = true;
		}
	}

	return *active;
}

bool app_alarm_poll(void)
{
	bool should_send = false;
	bool alarm = false;

	static bool alarm_temperature;
	static bool alarm_humidity;
	static bool alarm_pressure;
	static bool alarm_t1_temperature;
	static bool alarm_t2_temperature;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	alarm |= eval_threshold(&alarm_temperature, g_app_config.alarm_temperature_enabled,
				g_app_sensor_data.temperature, g_app_config.alarm_temperature_lo,
				g_app_config.alarm_temperature_hi,
				g_app_config.alarm_temperature_hst, "internal temperature",
				&should_send);

	alarm |= eval_threshold(&alarm_humidity, g_app_config.alarm_humidity_enabled,
				g_app_sensor_data.humidity, g_app_config.alarm_humidity_lo,
				g_app_config.alarm_humidity_hi, g_app_config.alarm_humidity_hst,
				"humidity", &should_send);

	/* Pressure config thresholds are stored in hPa × 10, sensor reports hPa. */
	alarm |= eval_threshold(&alarm_pressure, g_app_config.alarm_pressure_enabled,
				g_app_sensor_data.pressure * 10.f, g_app_config.alarm_pressure_lo,
				g_app_config.alarm_pressure_hi, g_app_config.alarm_pressure_hst,
				"pressure", &should_send);

	alarm |= eval_threshold(&alarm_t1_temperature, g_app_config.alarm_t1_temperature_enabled,
				g_app_sensor_data.t1_temperature,
				g_app_config.alarm_t1_temperature_lo,
				g_app_config.alarm_t1_temperature_hi,
				g_app_config.alarm_t1_temperature_hst, "external temperature 1",
				&should_send);

	alarm |= eval_threshold(&alarm_t2_temperature, g_app_config.alarm_t2_temperature_enabled,
				g_app_sensor_data.t2_temperature,
				g_app_config.alarm_t2_temperature_lo,
				g_app_config.alarm_t2_temperature_hi,
				g_app_config.alarm_t2_temperature_hst, "external temperature 2",
				&should_send);

	k_mutex_unlock(&g_app_sensor_data_lock);

	int64_t now = k_uptime_get();

	k_mutex_lock(&m_lock, K_FOREVER);
	for (int s = 0; s < APP_ALARM_SOURCE_COUNT; s++) {
		/* Expire the both-bool 10 s hold: app_alarm_event() can only latch
		 * m_alarm_active[s] true (no edge knows which state is "alarm"
		 * when both bools are set), so the timer is the only mechanism
		 * that clears it back to false. */
		if (m_both_bool_expiry_ms[s] != 0 && now >= m_both_bool_expiry_ms[s]) {
			m_alarm_active[s] = false;
			m_both_bool_expiry_ms[s] = 0;
		}
		if (m_alarm_active[s]) {
			alarm = true;
		}
	}
	k_mutex_unlock(&m_lock);

	if (should_send) {
#if defined(CONFIG_LORAWAN)
		app_lrw_send();
#endif /* defined(CONFIG_LORAWAN) */
	}

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

	/* Both bools set: every edge (act or deact) extends the red-LED hold 10 s further. */
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
