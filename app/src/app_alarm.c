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

#if defined(__has_include) && __has_include("app_clock.h")
#include "app_clock.h"
#define APP_ALARM_HAVE_CLOCK 1
#endif

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
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

/* Threshold side crossed (encoded in the fPort-3 record header). */
#define ALARM_SIDE_NA 0
#define ALARM_SIDE_LO 1
#define ALARM_SIDE_HI 2

#define ALARM_VALUE_SENTINEL ((int16_t)0x8000) /* discrete records: no threshold/value */

/* One collected alarm edge. threshold/value/hyst are already scaled to the
 * sensor's wire units (temp/hum ×100, pressure ×10); sentinel for discrete. */
struct alarm_event {
	uint8_t source; /* enum app_alarm_source */
	uint8_t side;   /* ALARM_SIDE_* */
	bool active;    /* true = activate, false = deactivate */
	int16_t threshold;
	int16_t value;
	int16_t hyst;
	uint16_t rel_s; /* seconds since the window base time */
};

#define ALARM_BATCH_MAX 8 /* records held per window; overflow counted in m_window_total */

static struct alarm_event m_batch[ALARM_BATCH_MAX];
static uint8_t m_batch_count;
static uint16_t m_window_total; /* all alarms in the window (may exceed m_batch_count) */
static uint32_t m_window_base_unix;
static int64_t m_window_start_ms;
static bool m_window_open;
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

#define ALARM_FRAME_MAX  64 /* fPort-3 frame buffer (matches app_lrw pending slot) */
#define ALARM_RECORD_LEN 9  /* hdr + rel_s + threshold + value + hyst */
#define ALARM_HEADER_LEN 5  /* total_count(1) + base_unix(4) */

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

/* Encode the collected batch into the fPort-3 frame and hand it to app_lrw.
 * Records are capped to the current DR budget; m_window_total still reports the
 * true count so the backend knows if some were dropped. Caller holds m_lock. */
static void alarm_batch_flush(void)
{
	if (m_batch_count == 0) {
		m_window_open = false;
		m_window_total = 0;
		return;
	}

	uint8_t buf[ALARM_FRAME_MAX];
	size_t pos = 0;
	size_t cap = ALARM_FRAME_MAX;
#if defined(CONFIG_LORAWAN)
	uint8_t dr = app_lrw_get_max_payload();
	if (dr > 0 && dr < cap) {
		cap = dr;
	}
#endif

	buf[pos++] = (uint8_t)MIN(m_window_total, 0xFF);
	sys_put_le32(m_window_base_unix, &buf[pos]);
	pos += 4;

	uint8_t written = 0;
	for (uint8_t i = 0; i < m_batch_count; i++) {
		if (pos + ALARM_RECORD_LEN > cap) {
			break; /* DR-bounded; total_count signals the rest */
		}
		struct alarm_event *e = &m_batch[i];
		buf[pos++] = (uint8_t)((e->source << 3) | (e->side << 1) | (e->active ? 1 : 0));
		sys_put_le16(e->rel_s, &buf[pos]);
		pos += 2;
		sys_put_le16((uint16_t)e->threshold, &buf[pos]);
		pos += 2;
		sys_put_le16((uint16_t)e->value, &buf[pos]);
		pos += 2;
		sys_put_le16((uint16_t)e->hyst, &buf[pos]);
		pos += 2;
		written++;
	}

#if defined(CONFIG_LORAWAN)
	(void)app_lrw_send_alarm(buf, pos);
#endif
	LOG_INF("Alarm batch: %u/%u records on fPort 3", written, m_window_total);

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
 * pass scaled threshold/value/hyst; discrete sources pass sentinels + side N/A.
 * With alarm_limit == 0 the record is sent immediately as a 1-record frame;
 * otherwise it joins the current collection window (opened on the first edge,
 * flushed by m_alarm_batch_work after alarm_limit seconds). */
static void alarm_collect(uint8_t source, bool active, uint8_t side, int16_t threshold,
			  int16_t value, int16_t hyst)
{
	int limit = g_app_config.alarm_limit;
	int64_t now = k_uptime_get();

	k_mutex_lock(&m_lock, K_FOREVER);

	if (limit <= 0) {
		m_window_base_unix = now_unix_or_uptime();
		m_window_total = 1;
		m_batch_count = 1;
		m_batch[0] = (struct alarm_event){source, side, active, threshold, value, hyst, 0};
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
		uint16_t rel = (uint16_t)MIN((now - m_window_start_ms) / 1000, 0xFFFF);
		m_batch[m_batch_count++] =
			(struct alarm_event){source, side, active, threshold, value, hyst, rel};
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
		*act = g_app_config.motion_sensitivity != APP_CONFIG_MOTION_SENSITIVITY_OFF;
		*deact = false;
		return 0;
	default:
		return -EINVAL;
	}
}

/* Per-source wire scaling for the fPort-3 detail (threshold sources only): temp
 * and humidity ×100, pressure already hPa×10 from the poll call. */
static int16_t alarm_scale(enum app_alarm_source source, float v)
{
	switch (source) {
	case APP_ALARM_SOURCE_TEMPERATURE:
	case APP_ALARM_SOURCE_T1_TEMPERATURE:
	case APP_ALARM_SOURCE_T2_TEMPERATURE:
	case APP_ALARM_SOURCE_HUMIDITY:
		return (int16_t)lroundf(v * 100.0f);
	case APP_ALARM_SOURCE_PRESSURE:
		return (int16_t)lroundf(v);
	default:
		return ALARM_VALUE_SENTINEL;
	}
}

/* Evaluate one threshold with hysteresis. Latches into m_alarm_active[source],
 * sets *should_send on every edge, and records the edge (source/side/threshold/
 * value/hysteresis) for the fPort-3 detail batch. Returns the latched state. */
static bool eval_threshold(enum app_alarm_source source, bool enabled, float value, float lo,
			   float hi, float hst, bool *should_send)
{
	bool *active = &m_alarm_active[source];

	if (!enabled || isnan(value)) {
		*active = false;
		return false;
	}

	if (*active) {
		if (value > lo + hst && value < hi - hst) {
			LOG_INF("Deactivated alarm for %s", app_alarm_source_name(source));
			*active = false;
			*should_send = true;
			alarm_collect(source, false, ALARM_SIDE_NA, ALARM_VALUE_SENTINEL,
				      alarm_scale(source, value), alarm_scale(source, hst));
		}
	} else if (value < lo - hst) {
		LOG_INF("Activated alarm for %s (lo)", app_alarm_source_name(source));
		*active = true;
		*should_send = true;
		alarm_collect(source, true, ALARM_SIDE_LO, alarm_scale(source, lo),
			      alarm_scale(source, value), alarm_scale(source, hst));
	} else if (value > hi + hst) {
		LOG_INF("Activated alarm for %s (hi)", app_alarm_source_name(source));
		*active = true;
		*should_send = true;
		alarm_collect(source, true, ALARM_SIDE_HI, alarm_scale(source, hi),
			      alarm_scale(source, value), alarm_scale(source, hst));
	}

	return *active;
}

bool app_alarm_poll(void)
{
	bool should_send = false;
	bool alarm = false;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	alarm |=
		eval_threshold(APP_ALARM_SOURCE_TEMPERATURE, g_app_config.alarm_temperature_enabled,
			       g_app_sensor_data.temperature, g_app_config.alarm_temperature_lo,
			       g_app_config.alarm_temperature_hi,
			       g_app_config.alarm_temperature_hst, &should_send);

	alarm |= eval_threshold(APP_ALARM_SOURCE_HUMIDITY, g_app_config.alarm_humidity_enabled,
				g_app_sensor_data.humidity, g_app_config.alarm_humidity_lo,
				g_app_config.alarm_humidity_hi, g_app_config.alarm_humidity_hst,
				&should_send);

	/* Pressure config thresholds are stored in hPa × 10, sensor reports hPa. */
	alarm |= eval_threshold(APP_ALARM_SOURCE_PRESSURE, g_app_config.alarm_pressure_enabled,
				g_app_sensor_data.pressure * 10.f, g_app_config.alarm_pressure_lo,
				g_app_config.alarm_pressure_hi, g_app_config.alarm_pressure_hst,
				&should_send);

	alarm |= eval_threshold(
		APP_ALARM_SOURCE_T1_TEMPERATURE, g_app_config.alarm_t1_temperature_enabled,
		g_app_sensor_data.t1_temperature, g_app_config.alarm_t1_temperature_lo,
		g_app_config.alarm_t1_temperature_hi, g_app_config.alarm_t1_temperature_hst,
		&should_send);

	alarm |= eval_threshold(
		APP_ALARM_SOURCE_T2_TEMPERATURE, g_app_config.alarm_t2_temperature_enabled,
		g_app_sensor_data.t2_temperature, g_app_config.alarm_t2_temperature_lo,
		g_app_config.alarm_t2_temperature_hi, g_app_config.alarm_t2_temperature_hst,
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
		/* Discrete sources have no threshold/value — sentinels, side N/A. */
		alarm_collect(source, active, ALARM_SIDE_NA, ALARM_VALUE_SENTINEL,
			      ALARM_VALUE_SENTINEL, ALARM_VALUE_SENTINEL);
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
