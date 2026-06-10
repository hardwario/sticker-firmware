/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_sensor.h"

#if defined(__has_include) && __has_include("app_clock.h")
#include "app_clock.h"
#define APP_ALARM_HAVE_CLOCK 1
#endif

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_alarm, LOG_LEVEL_DBG);

static bool m_alarm_active[APP_ALARM_SOURCE_COUNT];
static int64_t m_both_bool_expiry_ms[APP_ALARM_SOURCE_COUNT];
static int64_t m_last_alarm_send_ms;
static app_alarm_event_cb m_event_cb;
static void *m_event_cb_user_data;

K_MUTEX_DEFINE(m_lock);

/* ---- Alarm-detail batch on fPort 3 (#27) -------------------------------- */

/* Threshold side crossed (AlarmEvent.Side wire values). */
#define ALARM_SIDE_NONE 0
#define ALARM_SIDE_LO   1
#define ALARM_SIDE_HI   2

/* Transition direction (AlarmEvent.Edge wire values). */
#define ALARM_EDGE_ACT   0
#define ALARM_EDGE_DEACT 1

#define ALARM_BATCH_MAX 8 /* records held per window; overflow counted in m_window_total */

/* Collected edges, ready to hand to app_cmd_build_alarm_report() verbatim. */
static struct app_cmd_alarm_event m_batch[ALARM_BATCH_MAX];
static uint8_t m_batch_count;
static uint16_t m_window_total; /* all alarms in the window (may exceed m_batch_count) */
static uint32_t m_window_base_unix;
static int64_t m_window_start_ms;
static bool m_window_open;
/* Side latched when a threshold activates, reused so the deactivate edge keeps
 * the same hi/lo (the deactivate condition itself doesn't know which bound). */
static uint8_t m_alarm_side[APP_ALARM_SOURCE_COUNT];
static struct k_work_delayable m_alarm_batch_work;

static const char *const m_source_names[APP_ALARM_SOURCE_COUNT] = {
	[APP_ALARM_SOURCE_HALL_LEFT] = "hall-left",
	[APP_ALARM_SOURCE_HALL_RIGHT] = "hall-right",
	[APP_ALARM_SOURCE_PIR_MOTION] = "pir",
	[APP_ALARM_SOURCE_INPUT_A] = "input-a",
	[APP_ALARM_SOURCE_INPUT_B] = "input-b",
	[APP_ALARM_SOURCE_TEMPERATURE] = "temperature",
	[APP_ALARM_SOURCE_HUMIDITY] = "humidity",
	[APP_ALARM_SOURCE_PRESSURE] = "pressure",
	[APP_ALARM_SOURCE_T1_TEMPERATURE] = "t1-temperature",
	[APP_ALARM_SOURCE_T2_TEMPERATURE] = "t2-temperature",
	[APP_ALARM_SOURCE_ACCEL_MOTION] = "accel-motion",
};

const char *app_alarm_source_name(enum app_alarm_source source)
{
	if (source < 0 || source >= APP_ALARM_SOURCE_COUNT) {
		return "?";
	}
	return m_source_names[source];
}

/* Red-LED hold for both-mode and pulse sources, in ms (config alarm_notif_time). */
static inline int64_t notif_hold_ms(void)
{
	return (int64_t)g_app_config.alarm_notif_time * 1000;
}

/* Send an alarm uplink, rate-limited by the global alarm_limit (seconds). The
 * first alarm goes out immediately; further alarms within the window are
 * suppressed (the source latch, LED and counters still update elsewhere — only
 * the LoRaWAN message is throttled). alarm_limit == 0 disables the limit. */
static void alarm_lrw_send(void)
{
#if defined(CONFIG_LORAWAN)
	int limit = g_app_config.alarm_limit;
	int64_t now = k_uptime_get();

	if (limit > 0 && m_last_alarm_send_ms != 0 &&
	    (now - m_last_alarm_send_ms) < (int64_t)limit * 1000) {
		LOG_INF("Alarm uplink rate-limited (limit %d s)", limit);
		return;
	}

	m_last_alarm_send_ms = now;
	app_lrw_send();
#endif /* defined(CONFIG_LORAWAN) */
}

#define ALARM_FRAME_MAX 64 /* fPort-3 frame buffer (matches app_lrw pending slot) */

static uint32_t now_unix_or_uptime(void)
{
#ifdef APP_ALARM_HAVE_CLOCK
	uint32_t u;
	if (app_clock_get_unix(&u) == 0) {
		return u;
	}
#endif
	return (uint32_t)(k_uptime_get() / 1000);
}

/* Encode the collected batch into an AlarmReport (fPort 3) and hand it to
 * app_lrw. Events are trimmed to the current DR budget by retrying the encode
 * with fewer events; m_window_total still reports the true count so the backend
 * knows some were dropped (decoder: truncated = events < total). Caller holds
 * m_lock. */
static void alarm_batch_flush(void)
{
	if (m_batch_count == 0) {
		m_window_open = false;
		m_window_total = 0;
		return;
	}

	size_t cap = ALARM_FRAME_MAX;
#if defined(CONFIG_LORAWAN)
	uint8_t dr = app_lrw_get_max_payload();
	if (dr > 0 && dr < cap) {
		cap = dr;
	}
#endif

	uint8_t buf[ALARM_FRAME_MAX];
	size_t len = 0;
	uint8_t n = m_batch_count;
	int ret = -EMSGSIZE;

	/* Drop the newest events first until the report fits the DR budget. */
	while (n > 0) {
		ret = app_cmd_build_alarm_report(m_window_base_unix, m_window_total, m_batch, n,
						 buf, cap, &len);
		if (ret == 0) {
			break;
		}
		n--;
	}

	if (ret == 0) {
#if defined(CONFIG_LORAWAN)
		(void)app_lrw_send_alarm(buf, len);
#endif
		LOG_INF("Alarm batch: %u/%u events on fPort 3 (%u B)", n, m_window_total,
			(unsigned)len);
	} else {
		LOG_ERR_CALL_FAILED_INT("app_cmd_build_alarm_report", ret);
	}

	m_batch_count = 0;
	m_window_total = 0;
	m_window_open = false;
}

static void alarm_batch_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&m_lock, K_FOREVER);
	alarm_batch_flush();
	k_mutex_unlock(&m_lock);
}

/* Record one alarm edge for the fPort-3 detail batch (#27). Threshold sources
 * pass has_value=true with the scaled current value; discrete sources pass
 * has_value=false + side none. With alarm_limit == 0 the event is sent
 * immediately as a 1-event report; otherwise it joins the current collection
 * window (opened on the first edge, flushed by m_alarm_batch_work after
 * alarm_limit seconds). */
static void alarm_collect(uint8_t source, bool active, uint8_t side, bool has_value, int32_t value)
{
	int limit = g_app_config.alarm_limit;
	int64_t now = k_uptime_get();
	struct app_cmd_alarm_event ev = {
		.source = source,
		.edge = active ? ALARM_EDGE_ACT : ALARM_EDGE_DEACT,
		.side = side,
		.has_value = has_value,
		.value = value,
		.rel_s = 0,
	};

	k_mutex_lock(&m_lock, K_FOREVER);

	if (limit <= 0) {
		m_window_base_unix = now_unix_or_uptime();
		m_window_total = 1;
		m_batch_count = 1;
		m_batch[0] = ev;
		alarm_batch_flush();
		k_mutex_unlock(&m_lock);
		return;
	}

	if (!m_window_open) {
		m_window_open = true;
		m_window_start_ms = now;
		m_window_base_unix = now_unix_or_uptime();
		m_window_total = 0;
		m_batch_count = 0;
		k_work_schedule(&m_alarm_batch_work, K_SECONDS(limit));
	}

	m_window_total++;
	if (m_batch_count < ALARM_BATCH_MAX) {
		ev.rel_s = (uint32_t)MIN((now - m_window_start_ms) / 1000, 0xFFFF);
		m_batch[m_batch_count++] = ev;
	}

	k_mutex_unlock(&m_lock);
}

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
		*act = g_app_config.pir_notify_act && g_app_config.cap_pir_detector;
		*deact = false;
		return 0;
	case APP_ALARM_SOURCE_ACCEL_MOTION:
		/* Accelerometer any-motion is an activation-only pulse, gated by the
		 * motion_sensitivity config (OFF = disabled). */
		*act = g_app_config.accel_motion_sensitivity != APP_CONFIG_MOTION_SENSITIVITY_OFF;
		*deact = false;
		return 0;
	default:
		return -EINVAL;
	}
}

/* Per-source wire scaling for the fPort-3 detail value (threshold sources only).
 * Wire units: temp and humidity ×100, pressure hPa×10. `v` for pressure arrives
 * already in hPa (the poll call passes kPa×10), so ×10 again yields hPa×10 to
 * match the proto/decoder contract. */
static int32_t alarm_scale(enum app_alarm_source source, float v)
{
	switch (source) {
	case APP_ALARM_SOURCE_TEMPERATURE:
	case APP_ALARM_SOURCE_T1_TEMPERATURE:
	case APP_ALARM_SOURCE_T2_TEMPERATURE:
	case APP_ALARM_SOURCE_HUMIDITY:
		return (int32_t)lroundf(v * 100.0f);
	case APP_ALARM_SOURCE_PRESSURE:
		return (int32_t)lroundf(v * 10.0f);
	default:
		return 0;
	}
}

/* Evaluate one threshold with hysteresis. Latches into m_alarm_active[source],
 * sets *should_send on every edge, and records the edge (source/edge/side +
 * scaled current value) for the fPort-3 detail batch. The deactivate edge
 * carries the side latched at activation (m_alarm_side). Returns the latched
 * state. */
static bool eval_threshold(enum app_alarm_source source, bool enabled, float value, float lo,
			   float hi, float hst, bool *should_send)
{
	bool *active = &m_alarm_active[source];

	if (!enabled || isnan(value)) {
		if (*active) {
			/* The alarm was latched active but it can no longer be
			 * evaluated (sensor lost -> NaN, or the source was disabled).
			 * Emit a deactivate edge so a backend tracking fPort-3 does not
			 * stay "active" forever. No valid reading, so the edge carries
			 * no value. */
			LOG_INF("Force-deactivated alarm for %s (%s)",
				app_alarm_source_name(source),
				isnan(value) ? "sensor lost" : "disabled");
			*active = false;
			*should_send = true;
			alarm_collect(source, false, m_alarm_side[source], false, 0);
		}
		return false;
	}

	if (*active) {
		if (value > lo + hst && value < hi - hst) {
			LOG_INF("Deactivated alarm for %s", app_alarm_source_name(source));
			*active = false;
			*should_send = true;
			alarm_collect(source, false, m_alarm_side[source], true,
				      alarm_scale(source, value));
		}
	} else if (value < lo - hst) {
		LOG_INF("Activated alarm for %s (lo)", app_alarm_source_name(source));
		*active = true;
		*should_send = true;
		m_alarm_side[source] = ALARM_SIDE_LO;
		alarm_collect(source, true, ALARM_SIDE_LO, true, alarm_scale(source, value));
	} else if (value > hi + hst) {
		LOG_INF("Activated alarm for %s (hi)", app_alarm_source_name(source));
		*active = true;
		*should_send = true;
		m_alarm_side[source] = ALARM_SIDE_HI;
		alarm_collect(source, true, ALARM_SIDE_HI, true, alarm_scale(source, value));
	}

	return *active;
}

bool app_alarm_poll(void)
{
	bool should_send = false;
	bool alarm = false;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	alarm |=
		eval_threshold(APP_ALARM_SOURCE_TEMPERATURE, g_app_config.temperature_alarm_enabled,
			       g_app_sensor_data.temperature, g_app_config.temperature_alarm_lo,
			       g_app_config.temperature_alarm_hi,
			       g_app_config.temperature_alarm_hst, &should_send);

	alarm |= eval_threshold(APP_ALARM_SOURCE_HUMIDITY, g_app_config.humidity_alarm_enabled,
				g_app_sensor_data.humidity, g_app_config.humidity_alarm_lo,
				g_app_config.humidity_alarm_hi, g_app_config.humidity_alarm_hst,
				&should_send);

	/* Sensor reports kPa; ×10 converts to hPa to match the hPa-valued config
	 * thresholds. (alarm_scale then ×10 again for the hPa×10 wire value.) */
	alarm |= eval_threshold(APP_ALARM_SOURCE_PRESSURE, g_app_config.pressure_alarm_enabled,
				g_app_sensor_data.pressure * 10.f, g_app_config.pressure_alarm_lo,
				g_app_config.pressure_alarm_hi, g_app_config.pressure_alarm_hst,
				&should_send);

	alarm |= eval_threshold(APP_ALARM_SOURCE_T1_TEMPERATURE, g_app_config.t1_alarm_enabled,
				g_app_sensor_data.t1_temperature, g_app_config.t1_alarm_lo,
				g_app_config.t1_alarm_hi, g_app_config.t1_alarm_hst, &should_send);

	alarm |= eval_threshold(APP_ALARM_SOURCE_T2_TEMPERATURE, g_app_config.t2_alarm_enabled,
				g_app_sensor_data.t2_temperature, g_app_config.t2_alarm_lo,
				g_app_config.t2_alarm_hi, g_app_config.t2_alarm_hst, &should_send);

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
		alarm_lrw_send();
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

	/* Both bools set, or a pulse source (PIR / accelerometer motion are
	 * activation-only): the latch has no clearing edge, so hold it for
	 * alarm_notif_time and let app_alarm_poll() auto-clear it. Otherwise the
	 * latch tracks the configured edge. */
	if ((notify_act && notify_deact) ||
	    ((source == APP_ALARM_SOURCE_PIR_MOTION || source == APP_ALARM_SOURCE_ACCEL_MOTION) &&
	     notify_act)) {
		m_alarm_active[source] = true;
		m_both_bool_expiry_ms[source] = k_uptime_get() + notif_hold_ms();
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

	if (send) {
		alarm_lrw_send();
		/* Discrete sources have no value — side none, value absent. */
		alarm_collect(source, active, ALARM_SIDE_NONE, false, 0);
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

static int app_alarm_init(void)
{
	k_work_init_delayable(&m_alarm_batch_work, alarm_batch_work_handler);
	return 0;
}

SYS_INIT(app_alarm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
