/*
 * Copyright (c) 2025-2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_alarm_rules.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_report.h"
#include "app_sensor.h"
#include "app_w1_slots.h"

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

/* No-data watchdog: a config-enabled analog sensor that reads NaN continuously
 * for this long is reported as stopped (a no_data AlarmEvent). Drivers already
 * retry transient I2C/1-Wire errors, so a NaN here is a confirmed read failure;
 * the window only rejects a single missed sample. The watchdog is independent
 * of the 16 alarm rule slots, so its events carry slot = APP_ALARM_NO_DATA_SLOT. */
#define APP_ALARM_NO_DATA_MS   5000
#define APP_ALARM_NO_DATA_SLOT 0xFF

/* Low-battery watchdog (#210): raise a fPort-3 alarm (source=battery,
 * quantity=voltage, type=low) when the supply drops below the configurable
 * `battery_level` threshold (config in mV, default 2400 — Li cells discharge
 * non-linearly so the warning level is left to the integrator) and clear it once
 * it recovers past the hysteresis band. Bench-measured min operating voltage is
 * ~1.3 V (the node wedges silently below that and only a power-cycle recovers
 * it), so the default 2.4 V warns the backend with margin to spare. Like the
 * no-data watchdog it is independent of the 16 rule slots (slot = 0xFE) and it
 * does drive the red LED (it is an alarm). */
#define APP_ALARM_BATTERY_HYST_V 0.3f /* fixed: recover above threshold + 0.3 V (anti-chatter) */
#define APP_ALARM_BATTERY_SLOT   0xFE

/* fPort-3 wire scaling per quantity; defined later, forward-declared for the
 * battery watchdog's alarm_collect_battery(). */
static int32_t alarm_scale(enum app_alarm_quantity q, float v);

/* Wire enum values (AlarmEvent.Type / .Edge). type says WHAT fired (#212). */
#define ALARM_TYPE_NONE    0
#define ALARM_TYPE_LOW     1
#define ALARM_TYPE_HIGH    2
#define ALARM_TYPE_TRIGGER 3
#define ALARM_TYPE_NO_DATA 4
#define ALARM_EDGE_ACT     0
#define ALARM_EDGE_DEACT   1

/* ---- per-rule runtime state, keyed 1:1 on the alarm slot index -----------
 * m_rt[slot] is the runtime latch for the rule in that slot. source/quantity are
 * cached so a slot that is cleared or re-pointed at a different (source,quantity)
 * resets its latch (see rt_sync). Two rules on one sensor live in different slots
 * and so latch independently. */

struct rstate {
	bool used;
	uint8_t source;
	uint8_t quantity;
	bool active;  /* alarm latched */
	uint8_t type; /* threshold: which bound latched (ALARM_TYPE_LOW/HIGH) for deactivate */
	uint8_t prev_state; /* STATE: last digital level seen */
	bool have_prev_state;
	uint32_t prev_count; /* COUNT: baseline counter at window start */
	bool have_prev_count;
	int64_t count_window_start; /* COUNT: start of the current interval_report window (#195) */
	int64_t oneshot_expiry;     /* STATE edge one-shot: auto-clear time (0 = none) */
};

static struct rstate m_rt[APP_ALARM_SLOT_COUNT];
/* -1 = "never sent" sentinel; k_uptime_get() legitimately returns 0 in the first
 * millisecond after boot, so 0 cannot mark "never sent" without a window where the
 * rate limit is silently skipped (#219). */
static int64_t m_last_alarm_send_ms = -1;
static app_alarm_event_cb m_event_cb;
static void *m_event_cb_user_data;

K_MUTEX_DEFINE(m_lock);

/* Return the rule in `slot`, syncing its runtime latch: an empty slot (or one
 * now pointing at a different source/quantity than its latch tracked) is reset.
 * Returns NULL for an empty slot. */
static bool rt_sync(uint8_t slot, struct app_alarm_rule *out)
{
	struct rstate *rt = &m_rt[slot];

	if (!app_alarm_rules_get(slot, out)) {
		if (rt->used) {
			*rt = (struct rstate){0};
		}
		return false;
	}
	if (!rt->used || rt->source != out->source || rt->quantity != out->quantity) {
		*rt = (struct rstate){
			.used = true, .source = out->source, .quantity = out->quantity};
	}
	return true;
}

/* ---- Alarm-detail batch on fPort 3 (#27) -------------------------------- */

#define ALARM_BATCH_MAX 8
#define ALARM_FRAME_MAX 64

static struct app_cmd_alarm_event m_batch[ALARM_BATCH_MAX];
static uint8_t m_batch_count;
static uint16_t m_window_total;
static uint32_t m_window_base_unix;
static bool m_window_base_synced; /* m_window_base_unix is absolute UTC, not uptime */
static int64_t m_window_start_ms;
static bool m_window_open;
static struct k_work_delayable m_alarm_batch_work;

static inline int64_t notif_hold_ms(void)
{
	return (int64_t)g_app_config.alarm_notif_time * 1000;
}

static void alarm_lrw_send(void)
{
#if defined(CONFIG_LORAWAN)
	int limit = g_app_config.alarm_limit;
	int64_t now = k_uptime_get();
	bool send;

	/* m_last_alarm_send_ms is read-modify-written from three contexts (main
	 * poll, PIR thread, system WQ). A 64-bit RMW is not atomic on Cortex-M4, so
	 * guard it under m_lock (#93.6); release before app_lrw_send() so the radio
	 * call never runs while holding the alarm lock. */
	k_mutex_lock(&m_lock, K_FOREVER);
	if (limit > 0 && m_last_alarm_send_ms != -1 &&
	    (now - m_last_alarm_send_ms) < (int64_t)limit * 1000) {
		send = false;
	} else {
		m_last_alarm_send_ms = now;
		send = true;
	}
	k_mutex_unlock(&m_lock);

	if (!send) {
		LOG_INF("Alarm uplink rate-limited (limit %d s)", limit);
		return;
	}
	app_report_trigger();
#endif
}

/* Current time in seconds. Returns absolute UTC when the RTC has synced (and sets
 * *synced), else uptime-seconds (*synced=false) so alarm times stay monotonic
 * before the clock is known. */
static uint32_t now_seconds(bool *synced)
{
#ifdef APP_ALARM_HAVE_CLOCK
	uint32_t u;
	if (app_clock_get_unix(&u) == 0) {
		*synced = true;
		return u;
	}
#endif
	*synced = false;
	return (uint32_t)(k_uptime_get() / 1000);
}

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

	/* L-4: if the window opened before the RTC synced but the clock is known
	 * now, re-anchor base_time to absolute UTC (rel_s offsets are unaffected).
	 * Mirrors the "fresh uptime" fixup in app_history_on_clock_sync(). */
	bool synced = m_window_base_synced;
#ifdef APP_ALARM_HAVE_CLOCK
	if (!synced) {
		uint32_t now_u;
		if (app_clock_get_unix(&now_u) == 0) {
			m_window_base_unix += now_u - (uint32_t)(k_uptime_get() / 1000);
			synced = true;
		}
	}
#endif

	uint8_t buf[ALARM_FRAME_MAX];
	size_t len = 0;
	uint8_t n = m_batch_count;
	int ret = -EMSGSIZE;

	while (n > 0) {
		ret = app_cmd_build_alarm_report(m_window_base_unix, m_window_total, synced,
						 m_batch, n, buf, cap, &len);
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

/* Record one alarm edge for the fPort-3 detail batch. Caller does NOT hold
 * m_lock (this takes it). */
/* Queue one built alarm event into the rate-limit window (or flush immediately
 * when alarm_limit <= 0). Shared by the rule path (alarm_collect) and the
 * no-data watchdog (alarm_collect_nodata). */
static void alarm_queue(struct app_cmd_alarm_event ev)
{
	int limit = g_app_config.alarm_limit;
	int64_t now = k_uptime_get();

	k_mutex_lock(&m_lock, K_FOREVER);

	if (limit <= 0) {
		m_window_base_unix = now_seconds(&m_window_base_synced);
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
		m_window_base_unix = now_seconds(&m_window_base_synced);
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

static void alarm_collect(uint8_t slot, uint8_t source, uint8_t quantity, bool active, uint8_t type,
			  bool has_value, int32_t value)
{
	struct app_cmd_alarm_event ev = {
		.slot = slot,
		.source = source,
		.quantity = quantity,
		.edge = active ? ALARM_EDGE_ACT : ALARM_EDGE_DEACT,
		.type = type,
		.has_value = has_value,
		.value = value,
		.rel_s = 0,
	};
	alarm_queue(ev);
}

/* No-data watchdog event: a config-enabled sensor stopped reporting (NaN for
 * >= APP_ALARM_NO_DATA_MS) or recovered. Not tied to a rule slot (slot = 0xFF). */
static void alarm_collect_nodata(uint8_t source, uint8_t quantity, bool active)
{
	struct app_cmd_alarm_event ev = {
		.slot = APP_ALARM_NO_DATA_SLOT,
		.source = source,
		.quantity = quantity,
		.edge = active ? ALARM_EDGE_ACT : ALARM_EDGE_DEACT,
		.type = ALARM_TYPE_NO_DATA,
		.has_value = false,
		.value = 0,
		.rel_s = 0,
	};
	alarm_queue(ev);
}

/* Low-battery watchdog event (#210): supply dropped below / recovered above the
 * threshold. type=low always (a falling supply); the deactivate edge marks the
 * recovery. Carries the current voltage (V×100) and slot = 0xFE. */
static void alarm_collect_battery(bool active, float voltage)
{
	struct app_cmd_alarm_event ev = {
		.slot = APP_ALARM_BATTERY_SLOT,
		.source = APP_ALARM_SRC_BATTERY,
		.quantity = APP_ALARM_Q_VOLTAGE,
		.edge = active ? ALARM_EDGE_ACT : ALARM_EDGE_DEACT,
		.type = ALARM_TYPE_LOW,
		.has_value = true,
		.value = alarm_scale(APP_ALARM_Q_VOLTAGE, voltage),
		.rel_s = 0,
	};
	alarm_queue(ev);
}

/* ---- value access (source, quantity) ------------------------------------ */

/* Wire scaling per quantity for the fPort-3 detail value. */
static int32_t alarm_scale(enum app_alarm_quantity q, float v)
{
	switch (q) {
	case APP_ALARM_Q_TEMPERATURE:
	case APP_ALARM_Q_HUMIDITY:
		return (int32_t)lroundf(v * 100.0f);
	case APP_ALARM_Q_PRESSURE:
		return (int32_t)lroundf(v * 10.0f); /* v already in hPa -> hPa×10 wire */
	case APP_ALARM_Q_MAGNETIC_FIELD:
		return (int32_t)lroundf(v * 1000.0f); /* mT -> µT */
	case APP_ALARM_Q_VOLTAGE:
		return (int32_t)lroundf(v * 100.0f); /* V -> V×100 */
	case APP_ALARM_Q_ILLUMINANCE:
	default:
		return (int32_t)lroundf(v);
	}
}

/* Read the analog value for a THRESHOLD rule. Returns false if the (source,
 * quantity) pair has no analog reading (caller treats as NaN/absent). Caller
 * holds g_app_sensor_data_lock. */
static bool read_threshold_value(uint8_t source, uint8_t quantity, float *out)
{
	const struct app_sensor_data *d = &g_app_sensor_data;

	if (source == APP_ALARM_SRC_ONBOARD) {
		switch (quantity) {
		case APP_ALARM_Q_TEMPERATURE:
			*out = d->temperature;
			return true;
		case APP_ALARM_Q_HUMIDITY:
			*out = d->humidity;
			return true;
		case APP_ALARM_Q_PRESSURE:
			*out = d->pressure * 10.0f; /* kPa -> hPa (config thresholds are hPa) */
			return true;
		default:
			return false;
		}
	}
	if (source >= APP_ALARM_SRC_SLOT1 && source <= APP_ALARM_SRC_SLOT4) {
		const struct app_w1_slot_reading *r = &d->w1[source - APP_ALARM_SRC_SLOT1];
		switch (quantity) {
		case APP_ALARM_Q_TEMPERATURE:
			*out = r->temperature;
			return true;
		case APP_ALARM_Q_HUMIDITY:
			*out = r->humidity;
			return true;
		case APP_ALARM_Q_ILLUMINANCE:
			*out = r->illuminance;
			return true;
		case APP_ALARM_Q_MAGNETIC_FIELD:
			*out = r->magnetic_field;
			return true;
		default:
			return false;
		}
	}
	/* Battery supply for the no-data watchdog (L-41): a failed/absent measurement
	 * leaves voltage NaN, so a dead battery monitor raises a no_data alarm like
	 * any other sensor instead of silently never firing. */
	if (source == APP_ALARM_SRC_BATTERY && quantity == APP_ALARM_Q_VOLTAGE) {
		*out = d->voltage;
		return true;
	}
	return false;
}

/* Current counter for a COUNT rule's source. Caller holds the data lock. */
static bool read_counter(uint8_t source, uint32_t *out)
{
	const struct app_sensor_data *d = &g_app_sensor_data;
	switch (source) {
	case APP_ALARM_SRC_HALL_LEFT:
		*out = d->hall_left_count;
		return true;
	case APP_ALARM_SRC_HALL_RIGHT:
		*out = d->hall_right_count;
		return true;
	case APP_ALARM_SRC_INPUT_A:
		*out = d->input_a_count;
		return true;
	case APP_ALARM_SRC_INPUT_B:
		*out = d->input_b_count;
		return true;
	case APP_ALARM_SRC_PIR:
		*out = d->motion_count;
		return true;
	case APP_ALARM_SRC_ACCEL:
		*out = d->accel_motion_count;
		return true;
	default:
		return false;
	}
}

/* Current digital level for a STATE rule polled in app_alarm_poll (slot tilt).
 * hall/input/pir/accel STATE arrives via app_alarm_event, not here. */
static bool read_poll_state(uint8_t source, uint8_t quantity, bool *out)
{
	const struct app_sensor_data *d = &g_app_sensor_data;
	if (quantity == APP_ALARM_Q_TILT && source >= APP_ALARM_SRC_SLOT1 &&
	    source <= APP_ALARM_SRC_SLOT4) {
		*out = d->w1[source - APP_ALARM_SRC_SLOT1].is_tilt_alert;
		return true;
	}
	return false;
}

/* ---- rule evaluation ---------------------------------------------------- */

/* Threshold rule with hysteresis (reuses the previous eval_threshold logic,
 * keyed to the rule's runtime state). Returns latched state. */
static bool eval_threshold(uint8_t slot, const struct app_alarm_rule *rule, struct rstate *rt,
			   float value, bool *should_send)
{
	if (!rule->enabled || isnan(value)) {
		if (rt->active) {
			rt->active = false;
			*should_send = true;
			alarm_collect(slot, rule->source, rule->quantity, false, rt->type, false,
				      0);
		}
		return false;
	}

	float lo = rule->lo, hi = rule->hi;
	/* Use the rule's hysteresis verbatim (0 = no dead-band, as configured). The
	 * negated comparison still captures NaN/negative (NaN >= 0 is false), so a
	 * garbage value can't widen the band, but we no longer force a minimum. */
	float hst = rule->hst;
	if (!(hst >= 0.0f)) {
		hst = 0.0f;
	}

	if (rt->active) {
		if (value > lo + hst && value < hi - hst) {
			rt->active = false;
			*should_send = true;
			alarm_collect(slot, rule->source, rule->quantity, false, rt->type, true,
				      alarm_scale(rule->quantity, value));
		}
	} else if (value < lo - hst) {
		rt->active = true;
		rt->type = ALARM_TYPE_LOW;
		*should_send = true;
		alarm_collect(slot, rule->source, rule->quantity, true, ALARM_TYPE_LOW, true,
			      alarm_scale(rule->quantity, value));
	} else if (value > hi + hst) {
		rt->active = true;
		rt->type = ALARM_TYPE_HIGH;
		*should_send = true;
		alarm_collect(slot, rule->source, rule->quantity, true, ALARM_TYPE_HIGH, true,
			      alarm_scale(rule->quantity, value));
	}
	return rt->active;
}

/* Momentary/pulse sources only ever assert (app_alarm_event(src, true)) and never
 * deassert, so a STATE rule's prev->cur edge can be observed at most once. Handle
 * them as a per-pulse one-shot in eval_state(). */
static inline bool source_is_momentary(uint8_t source)
{
	return source == APP_ALARM_SRC_ACCEL || source == APP_ALARM_SRC_PIR;
}

/* STATE rule: from/to over a digital level. from != to is an edge (one-shot:
 * activate, auto-clear after alarm_notif_time, no deactivate); from == to is a
 * level (active while cur == to). */
static void eval_state(uint8_t slot, const struct app_alarm_rule *rule, struct rstate *rt, bool cur,
		       bool *should_send)
{
	bool is_edge = (rule->from_state != rule->to_state);
	uint8_t cur_lvl = cur ? 1 : 0;

	if (!rule->enabled) {
		if (rt->active) {
			rt->active = false;
			rt->oneshot_expiry = 0;
			*should_send = true;
			alarm_collect(slot, rule->source, rule->quantity, false, ALARM_TYPE_TRIGGER,
				      true, 0);
		}
		rt->have_prev_state = true;
		rt->prev_state = cur_lvl;
		return;
	}

	if (source_is_momentary(rule->source)) {
		/* Pulse source: each asserted event is a discrete trigger. Fire a one-shot
		 * (auto-cleared after alarm_notif_time in app_alarm_poll), suppressed while a
		 * previous one is still latched so we emit at most one alarm per notif-time
		 * window regardless of event rate. from/to are not meaningful for a source
		 * that never reports a level, so edge (0->1) and level (1->1) behave alike. */
		/* Expire a lapsed one-shot here, in the event path, not only in the 3 s
		 * app_alarm_poll: otherwise an alarm_notif_time shorter than the poll cadence
		 * is silently clamped to it (a new event within ~3 s of the window elapsing
		 * stays suppressed), so notif_time < 3 s was ineffective (#267). */
		if (rt->active && rt->oneshot_expiry != 0 && k_uptime_get() >= rt->oneshot_expiry) {
			rt->active = false;
			rt->oneshot_expiry = 0;
		}
		if (cur && !rt->active) {
			rt->active = true;
			rt->oneshot_expiry = k_uptime_get() + notif_hold_ms();
			*should_send = true;
			alarm_collect(slot, rule->source, rule->quantity, true, ALARM_TYPE_TRIGGER,
				      true, cur_lvl);
		}
		rt->have_prev_state = true;
		rt->prev_state = cur_lvl;
		return;
	}

	if (is_edge) {
		/* Fire one-shot when the configured transition occurs. */
		if (rt->have_prev_state && rt->prev_state == rule->from_state &&
		    cur_lvl == rule->to_state) {
			rt->active = true;
			rt->oneshot_expiry = k_uptime_get() + notif_hold_ms();
			*should_send = true;
			alarm_collect(slot, rule->source, rule->quantity, true, ALARM_TYPE_TRIGGER,
				      true, cur_lvl);
		}
	} else {
		/* Level: active while cur == to. */
		bool want = (cur_lvl == rule->to_state);
		if (want && !rt->active) {
			rt->active = true;
			*should_send = true;
			alarm_collect(slot, rule->source, rule->quantity, true, ALARM_TYPE_TRIGGER,
				      true, cur_lvl);
		} else if (!want && rt->active) {
			rt->active = false;
			*should_send = true;
			alarm_collect(slot, rule->source, rule->quantity, false, ALARM_TYPE_TRIGGER,
				      true, cur_lvl);
		}
	}

	rt->have_prev_state = true;
	rt->prev_state = cur_lvl;
}

/* COUNT rate rule: alarm when the counter rose by >= hi over one interval_report
 * window. This is called on the 3 s LED poll, but the rate must be assessed over
 * interval_report ("per interval_report" contract, app_alarm_rules.h), not per
 * poll — otherwise with interval_sample < 3 s the counts split across polls and
 * the threshold under-counts (#195). So we accumulate across polls and evaluate
 * the delta only once a full interval_report window has elapsed (tumbling
 * window), re-baselining the counter and window each time. One-shot
 * (auto-clear). */
static void eval_count(uint8_t slot, const struct app_alarm_rule *rule, struct rstate *rt,
		       uint32_t cur, bool *should_send)
{
	int64_t now = k_uptime_get();

	if (!rt->have_prev_count) {
		rt->have_prev_count = true;
		rt->prev_count = cur;
		rt->count_window_start = now;
		return;
	}
	if (!rule->enabled) {
		rt->prev_count = cur;
		rt->count_window_start = now;
		return;
	}

	/* Hold until the interval_report window closes; counts keep accumulating
	 * in prev_count's delta meanwhile. */
	int64_t window_ms = (int64_t)g_app_config.interval_report * 1000;
	if (window_ms > 0 && (now - rt->count_window_start) < window_ms) {
		return;
	}

	/* M-8: a counter reset (counters-reset command / totalizer wipe) drops `cur`
	 * below prev_count. The uint32 subtraction below would then wrap to ~4.29e9
	 * and fire a bogus rate alarm on a routine admin action. A decreasing counter
	 * is never a legitimate rate — re-baseline the window and skip this pass. */
	if (cur < rt->prev_count) {
		rt->prev_count = cur;
		rt->count_window_start = now;
		return;
	}

	uint32_t delta = cur - rt->prev_count; /* wraps correctly on uint32 */
	if (rule->hi > 0 && delta >= (uint32_t)rule->hi) {
		rt->active = true;
		rt->oneshot_expiry = now + notif_hold_ms();
		*should_send = true;
		alarm_collect(slot, rule->source, rule->quantity, true, ALARM_TYPE_TRIGGER, true,
			      (int32_t)cur);
	}
	/* Re-baseline for the next window whether or not it fired. */
	rt->prev_count = cur;
	rt->count_window_start = now;
}

/* ---- no-data watchdog (config-driven, independent of rule slots) -------- */

/* Analog sensors monitored for "stopped reporting". Digital sources (hall /
 * input / PIR) are excluded — a disconnected line still reads a level, it never
 * goes NaN. One representative quantity per source (a dead sensor takes all of
 * its quantities with it). */
static const struct {
	uint8_t source;
	uint8_t quantity;
} m_nodata_tab[] = {
	{APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_TEMPERATURE},
	{APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_HUMIDITY},
	{APP_ALARM_SRC_ONBOARD, APP_ALARM_Q_PRESSURE},
	{APP_ALARM_SRC_SLOT1, APP_ALARM_Q_TEMPERATURE},
	{APP_ALARM_SRC_SLOT2, APP_ALARM_Q_TEMPERATURE},
	{APP_ALARM_SRC_SLOT3, APP_ALARM_Q_TEMPERATURE},
	{APP_ALARM_SRC_SLOT4, APP_ALARM_Q_TEMPERATURE},
	{APP_ALARM_SRC_BATTERY, APP_ALARM_Q_VOLTAGE}, /* L-41: battery monitor liveness */
};
#define NODATA_COUNT ARRAY_SIZE(m_nodata_tab)
static int64_t m_nodata_nan_since[NODATA_COUNT]; /* 0 = has data now */
static bool m_nodata_active[NODATA_COUNT];       /* no_data alarm latched */

static bool m_battery_low_active; /* low-battery watchdog latched (#210) */

/* Is this monitored sensor expected to report under the current config? */
static bool nodata_enabled(uint8_t source, uint8_t quantity)
{
	switch (source) {
	case APP_ALARM_SRC_ONBOARD:
		if (quantity == APP_ALARM_Q_PRESSURE) {
			return g_app_config.cap_barometer;
		}
		return true; /* onboard SHT4x temperature/humidity always present */
	case APP_ALARM_SRC_SLOT1:
	case APP_ALARM_SRC_SLOT2:
	case APP_ALARM_SRC_SLOT3:
	case APP_ALARM_SRC_SLOT4:
		/* H-5: gate on the *configured* (persisted) ROM, not the runtime type. A
		 * probe taught to this slot but absent from the bus has runtime type EMPTY;
		 * keying on that silently dropped it from monitoring. Keying on the
		 * configured ROM keeps it monitored so its absence raises a no_data alarm. */
		return g_app_config.cap_w1_sensors &&
		       app_w1_slot_is_configured(source - APP_ALARM_SRC_SLOT1);
	case APP_ALARM_SRC_BATTERY:
		return true; /* L-41: battery monitor always expected to report */
	default:
		return false;
	}
}

/* Evaluate the no-data watchdog over all monitored sensors. Caller holds
 * g_app_sensor_data_lock (read_threshold_value reads g_app_sensor_data). */
static void nodata_poll(int64_t now, bool *should_send)
{
	for (size_t i = 0; i < NODATA_COUNT; i++) {
		uint8_t src = m_nodata_tab[i].source;
		uint8_t q = m_nodata_tab[i].quantity;

		if (!nodata_enabled(src, q)) {
			/* Sensor disabled (cap off / slot unbound). If a no_data alarm was
			 * latched for it, emit the deactivate edge (M-7) before dropping the
			 * latch — otherwise the backend keeps a no_data alarm open forever on a
			 * sensor the operator deliberately turned off, needing a manual clear. */
			if (m_nodata_active[i]) {
				alarm_collect_nodata(src, q, false);
				*should_send = true;
			}
			m_nodata_nan_since[i] = 0;
			m_nodata_active[i] = false;
			continue;
		}

		float v = NAN;
		read_threshold_value(src, q, &v);

		if (!isnan(v)) {
			m_nodata_nan_since[i] = 0;
			if (m_nodata_active[i]) {
				m_nodata_active[i] = false;
				alarm_collect_nodata(src, q, false); /* recovered */
				*should_send = true;
			}
			continue;
		}

		/* NaN: arm / age the timer; fire once it has been NaN long enough. */
		if (m_nodata_nan_since[i] == 0) {
			m_nodata_nan_since[i] = now;
		}
		if (!m_nodata_active[i] && (now - m_nodata_nan_since[i]) >= APP_ALARM_NO_DATA_MS) {
			m_nodata_active[i] = true;
			alarm_collect_nodata(src, q, true);
			*should_send = true;
		}
	}
}

/* Low-battery watchdog (#210): independent of the rule slots and the no-data
 * watchdog. Reads the supply voltage from g_app_sensor_data (caller holds
 * g_app_sensor_data_lock), compares it against the configurable `battery_level`
 * threshold (mV) and fires/clears a low-battery alarm with hysteresis. A latched
 * alarm drives the red LED via app_alarm_poll (it is an alarm). */
static void battery_poll(bool *should_send)
{
	float v = g_app_sensor_data.voltage;
	float threshold = g_app_config.battery_level / 1000.0f; /* mV -> V */

	/* Skip until a plausible measurement exists (0/NaN before the first
	 * app_battery_measure, which would otherwise read as a false low). */
	if (!(v > 0.5f)) {
		return;
	}

	if (!m_battery_low_active) {
		if (v < threshold) {
			m_battery_low_active = true;
			alarm_collect_battery(true, v);
			*should_send = true;
		}
	} else if (v > threshold + APP_ALARM_BATTERY_HYST_V) {
		m_battery_low_active = false;
		alarm_collect_battery(false, v);
		*should_send = true;
	}
}

bool app_alarm_poll(void)
{
	bool should_send = false;
	bool alarm = false;
	int64_t now = k_uptime_get();

	/* m_rt[] is shared with app_alarm_event() (system WQ / PIR thread); hold
	 * m_lock across the whole evaluation + latch sweep so a concurrent event
	 * can't tear an m_rt slot (#185). Lock order is always
	 * g_app_sensor_data_lock -> m_lock; m_lock is recursive, so the nested
	 * alarm_collect() re-lock inside eval_* is fine. */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	k_mutex_lock(&m_lock, K_FOREVER);

	for (uint8_t slot = 0; slot < APP_ALARM_SLOT_COUNT; slot++) {
		struct app_alarm_rule rule_copy;
		if (!rt_sync(slot, &rule_copy)) {
			continue;
		}
		const struct app_alarm_rule *rule = &rule_copy;
		struct rstate *rt = &m_rt[slot];

		switch (app_alarm_quantity_kind((enum app_alarm_quantity)rule->quantity)) {
		case APP_ALARM_KIND_THRESHOLD: {
			float v = NAN;
			read_threshold_value(rule->source, rule->quantity, &v);
			eval_threshold(slot, rule, rt, v, &should_send);
			break;
		}
		case APP_ALARM_KIND_STATE: {
			/* Only slot tilt is polled here; hall/input/pir/accel come via
			 * app_alarm_event(). */
			bool st;
			if (read_poll_state(rule->source, rule->quantity, &st)) {
				eval_state(slot, rule, rt, st, &should_send);
			}
			break;
		}
		case APP_ALARM_KIND_RATE: {
			uint32_t c;
			if (read_counter(rule->source, &c)) {
				eval_count(slot, rule, rt, c, &should_send);
			}
			break;
		}
		}
	}

	/* No-data watchdog over every config-enabled analog sensor (independent of
	 * the rule slots above). Same lock — it reads g_app_sensor_data too, so it
	 * must run before the sensor-data lock is dropped (#205). */
	nodata_poll(now, &should_send);

	/* Low-battery watchdog (#210). Reads g_app_sensor_data.voltage, so it must
	 * also run before the sensor-data lock is dropped. */
	battery_poll(&should_send);

	/* A latched no-data watchdog event also counts as "active", so the main
	 * loop lights the red LED (blinks every BLINK_INTERVAL_SECONDS) until the
	 * sensor resumes. Read here under g_app_sensor_data_lock, like nodata_poll (#211). */
	for (size_t i = 0; i < NODATA_COUNT; i++) {
		if (m_nodata_active[i]) {
			alarm = true;
			break;
		}
	}

	/* A latched low-battery alarm also lights the red LED (#210) — it is an
	 * alarm. Read here under g_app_sensor_data_lock, like the no-data sweep. */
	if (m_battery_low_active) {
		alarm = true;
	}

	/* Sensor data no longer needed; keep m_lock for the latch sweep (#185). */
	k_mutex_unlock(&g_app_sensor_data_lock);

	/* Expire one-shot latches and collect "any active". */
	for (int i = 0; i < APP_ALARM_SLOT_COUNT; i++) {
		if (!m_rt[i].used) {
			continue;
		}
		if (m_rt[i].oneshot_expiry != 0 && now >= m_rt[i].oneshot_expiry) {
			m_rt[i].active = false;
			m_rt[i].oneshot_expiry = 0;
		}
		if (m_rt[i].active) {
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
	/* Discrete edge → every STATE rule on this source (several slots may carry
	 * the same source, e.g. an edge rule and a level rule). */
	app_alarm_event_cb cb;
	void *user_data;
	bool should_send = false;

	/* Evaluate every STATE rule on this source under m_lock so m_rt[] is not
	 * torn against app_alarm_poll() or another event on a different thread
	 * (#185). m_lock is recursive: the nested alarm_collect() re-lock is fine.
	 * cb is read under the same lock; should_send / cb are acted on after
	 * release so the radio enqueue and callback never run under m_lock. */
	k_mutex_lock(&m_lock, K_FOREVER);
	cb = m_event_cb;
	user_data = m_event_cb_user_data;

	for (uint8_t slot = 0; slot < APP_ALARM_SLOT_COUNT; slot++) {
		struct app_alarm_rule rule_copy;
		if (!rt_sync(slot, &rule_copy)) {
			continue;
		}
		const struct app_alarm_rule *rule = &rule_copy;
		if (rule->source != source || rule->quantity != APP_ALARM_Q_STATE) {
			continue;
		}
		eval_state(slot, rule, &m_rt[slot], active, &should_send);
	}
	k_mutex_unlock(&m_lock);

	if (should_send) {
		alarm_lrw_send();
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

bool app_alarm_is_active(enum app_alarm_source source, enum app_alarm_quantity quantity)
{
	bool a = false;
	k_mutex_lock(&m_lock, K_FOREVER);
	/* Any slot on this (source, quantity) latched active — several may exist. */
	for (int i = 0; i < APP_ALARM_SLOT_COUNT; i++) {
		if (m_rt[i].used && m_rt[i].source == source && m_rt[i].quantity == quantity &&
		    m_rt[i].active) {
			a = true;
			break;
		}
	}
	k_mutex_unlock(&m_lock);
	return a;
}

uint32_t app_alarm_status_flags(void)
{
	uint32_t flags = 0;

	/* Watchdog latches are written under g_app_sensor_data_lock in poll();
	 * take it first to keep the same lock order (data_lock -> m_lock) and avoid
	 * inversion. Read-only here: no evaluation, no one-shot expiry, no send. */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	for (size_t i = 0; i < NODATA_COUNT; i++) {
		if (m_nodata_active[i]) {
			flags |= APP_DEVICE_STATUS_ALARM_NO_DATA | APP_DEVICE_STATUS_ALARM_ANY;
			break;
		}
	}
	if (m_battery_low_active) {
		flags |= APP_DEVICE_STATUS_ALARM_LOW_BATT | APP_DEVICE_STATUS_ALARM_ANY;
	}
	k_mutex_unlock(&g_app_sensor_data_lock);

	k_mutex_lock(&m_lock, K_FOREVER);
	for (int i = 0; i < APP_ALARM_SLOT_COUNT; i++) {
		if (!m_rt[i].used || !m_rt[i].active) {
			continue;
		}
		flags |= APP_DEVICE_STATUS_ALARM_ANY;
		switch (app_alarm_quantity_kind((enum app_alarm_quantity)m_rt[i].quantity)) {
		case APP_ALARM_KIND_THRESHOLD:
			flags |= APP_DEVICE_STATUS_ALARM_THRESHOLD;
			break;
		case APP_ALARM_KIND_STATE:
			flags |= APP_DEVICE_STATUS_ALARM_STATE;
			break;
		case APP_ALARM_KIND_RATE:
			flags |= APP_DEVICE_STATUS_ALARM_RATE;
			break;
		}
	}
	k_mutex_unlock(&m_lock);

	return flags;
}

size_t app_alarm_active_snapshot(struct app_alarm_active *out, size_t max)
{
	if (!out || max == 0) {
		return 0;
	}

	size_t n = 0;

	/* No-data + low-battery watchdogs read g_app_sensor_data-backed latches;
	 * take that lock first, like app_alarm_status_flags() (keeps lock order
	 * data_lock -> m_lock and avoids holding both). */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	for (size_t i = 0; i < NODATA_COUNT && n < max; i++) {
		if (!m_nodata_active[i]) {
			continue;
		}
		out[n].source = m_nodata_tab[i].source;
		out[n].quantity = m_nodata_tab[i].quantity;
		out[n].type = ALARM_TYPE_NO_DATA;
		n++;
	}
	if (m_battery_low_active && n < max) {
		out[n].source = APP_ALARM_SRC_BATTERY;
		out[n].quantity = APP_ALARM_Q_VOLTAGE;
		out[n].type = ALARM_TYPE_LOW;
		n++;
	}
	k_mutex_unlock(&g_app_sensor_data_lock);

	/* Dynamic alarm-rule slots. type: threshold rules latch LOW/HIGH in
	 * rt->type; state/count rules report TRIGGER. */
	k_mutex_lock(&m_lock, K_FOREVER);
	for (int i = 0; i < APP_ALARM_SLOT_COUNT && n < max; i++) {
		if (!m_rt[i].used || !m_rt[i].active) {
			continue;
		}
		uint8_t type;
		switch (app_alarm_quantity_kind((enum app_alarm_quantity)m_rt[i].quantity)) {
		case APP_ALARM_KIND_THRESHOLD:
			type = m_rt[i].type;
			break;
		default:
			type = ALARM_TYPE_TRIGGER;
			break;
		}
		out[n].source = m_rt[i].source;
		out[n].quantity = m_rt[i].quantity;
		out[n].type = type;
		n++;
	}
	k_mutex_unlock(&m_lock);

	return n;
}

/* ---- shell -------------------------------------------------------------- */

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <string.h>

/* Per-slot latch state, for the shell list (distinct from the any-slot
 * app_alarm_is_active used by callers that key on source+quantity). */
static bool slot_active(uint8_t slot)
{
	bool a;
	k_mutex_lock(&m_lock, K_FOREVER);
	a = slot < APP_ALARM_SLOT_COUNT && m_rt[slot].used && m_rt[slot].active;
	k_mutex_unlock(&m_lock);
	return a;
}

/* Print one occupied slot's rule (caller has checked it is occupied). */
static void print_slot(const struct shell *sh, uint8_t slot, const struct app_alarm_rule *r)
{
	const char *src = app_alarm_source_name(r->source);
	const char *q = app_alarm_quantity_name(r->quantity);
	bool active = slot_active(slot);

	switch (app_alarm_quantity_kind(r->quantity)) {
	case APP_ALARM_KIND_THRESHOLD:
		shell_print(
			sh,
			"  [%u] %s %s  lo=%s%d.%02d hi=%s%d.%02d hst=%s%d.%02d  en=%d  active=%d",
			slot, src, q, APP_FP2(r->lo), APP_FP2(r->hi), APP_FP2(r->hst), r->enabled,
			active);
		break;
	case APP_ALARM_KIND_STATE:
		shell_print(sh, "  [%u] %s %s  %u->%u (%s)  en=%d  active=%d", slot, src, q,
			    r->from_state, r->to_state,
			    r->from_state == r->to_state ? "level" : "edge", r->enabled, active);
		break;
	case APP_ALARM_KIND_RATE:
		shell_print(sh, "  [%u] %s %s  rate>=%d/interval  en=%d  active=%d", slot, src, q,
			    (int)r->hi, r->enabled, active);
		break;
	}
}

static int cmd_alarm_list(const struct shell *sh, size_t argc, char **argv)
{
	/* Optional <index>: list just that one slot. */
	if (argc >= 2) {
		char *end;
		unsigned long slot = strtoul(argv[1], &end, 10);
		if (*end != '\0' || slot >= APP_ALARM_SLOT_COUNT) {
			shell_error(sh, "invalid <index> (0..%d)", APP_ALARM_SLOT_COUNT - 1);
			return -EINVAL;
		}
		struct app_alarm_rule r;
		if (!app_alarm_rules_get((uint8_t)slot, &r)) {
			shell_print(sh, "slot %lu empty", slot);
			return 0;
		}
		print_slot(sh, (uint8_t)slot, &r);
		return 0;
	}

	shell_print(sh, "%u rule(s):", app_alarm_rules_count());
	for (uint8_t slot = 0; slot < APP_ALARM_SLOT_COUNT; slot++) {
		struct app_alarm_rule r;
		if (app_alarm_rules_get(slot, &r)) {
			print_slot(sh, slot, &r);
		}
	}
	return 0;
}

/* Parse `<source> <quantity> <kind-args…>` starting at argv[base] into `r`
 * (enabled). Shared by `alarm set` (base=2, after the index) and `alarm new`
 * (base=1). Prints a usage/validity error and returns -EINVAL on bad input. */
static int parse_rule_spec(const struct shell *sh, size_t argc, char **argv, size_t base,
			   struct app_alarm_rule *r)
{
	int src = app_alarm_source_by_name(argv[base]);
	int qty = app_alarm_quantity_by_name(argv[base + 1]);
	if (src < 0 || qty < 0 ||
	    !app_alarm_rule_valid((enum app_alarm_source)src, (enum app_alarm_quantity)qty)) {
		shell_error(sh, "invalid <source> <quantity>");
		return -EINVAL;
	}

	*r = (struct app_alarm_rule){
		.source = (uint8_t)src, .quantity = (uint8_t)qty, .enabled = 1};

	size_t a = base + 2; /* first kind-arg */
	switch (app_alarm_quantity_kind((enum app_alarm_quantity)qty)) {
	case APP_ALARM_KIND_THRESHOLD:
		if (argc < a + 2) {
			shell_error(sh, "threshold args: <lo> <hi> [hst]");
			return -EINVAL;
		}
		r->lo = strtof(argv[a], NULL);
		r->hi = strtof(argv[a + 1], NULL);
		r->hst = (argc > a + 2) ? strtof(argv[a + 2], NULL) : 0.0f;
		break;
	case APP_ALARM_KIND_STATE:
		if (argc < a + 2) {
			shell_error(sh,
				    "state args: <from> <to> (0/1; from!=to=edge, from==to=level)");
			return -EINVAL;
		}
		r->from_state = (uint8_t)(strtoul(argv[a], NULL, 10) ? 1 : 0);
		r->to_state = (uint8_t)(strtoul(argv[a + 1], NULL, 10) ? 1 : 0);
		break;
	case APP_ALARM_KIND_RATE:
		if (argc < a + 1) {
			shell_error(sh, "count args: <N-per-interval>");
			return -EINVAL;
		}
		r->hi = (float)strtoul(argv[a], NULL, 10);
		break;
	}
	return 0;
}

/* Store `r` into `slot`, persist, and report. Shared by set / new. */
static int store_rule(const struct shell *sh, uint8_t slot, const struct app_alarm_rule *r)
{
	int ret = app_alarm_rules_set(slot, r);
	if (ret) {
		shell_error(sh, "set failed: %d", ret);
		return ret;
	}
	ret = app_alarm_rules_save();
	shell_print(sh, "rule set in slot %u%s", slot, ret ? " (NOT persisted!)" : "");
	app_lrw_arm_config_report(); /* #320: push the local change to the backend */
	return 0;
}

static int cmd_alarm_set(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	unsigned long slot = strtoul(argv[1], &end, 10);
	if (*end != '\0' || slot >= APP_ALARM_SLOT_COUNT) {
		shell_error(sh, "invalid <index> (0..%d)", APP_ALARM_SLOT_COUNT - 1);
		return -EINVAL;
	}

	struct app_alarm_rule r;
	int ret = parse_rule_spec(sh, argc, argv, 2, &r);
	if (ret) {
		return ret;
	}
	return store_rule(sh, (uint8_t)slot, &r);
}

static int cmd_alarm_new(const struct shell *sh, size_t argc, char **argv)
{
	struct app_alarm_rule r;
	int ret = parse_rule_spec(sh, argc, argv, 1, &r);
	if (ret) {
		return ret;
	}
	int slot = app_alarm_rules_first_free();
	if (slot < 0) {
		shell_error(sh, "no free slot (all %d in use)", APP_ALARM_SLOT_COUNT);
		return -ENOSPC;
	}
	return store_rule(sh, (uint8_t)slot, &r);
}

/* Force a sample + one evaluation pass now and report whether any alarm is
 * active. The main loop only calls app_alarm_poll() in the HEALTHY state, so on
 * a bench device (not joined) this is how a test exercises the rules. */
static int cmd_alarm_poll(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	app_sensor_sample();
	bool any = app_alarm_poll();
	shell_print(sh, "polled; any active: %s", any ? "yes" : "no");
	return 0;
}

static int cmd_alarm_clear(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1 || strcmp(argv[1], "all") == 0) {
		app_alarm_rules_clear_all();
		(void)app_alarm_rules_save();
		shell_print(sh, "all rules cleared");
		app_lrw_arm_config_report(); /* #320: push the local change to the backend */
		return 0;
	}
	char *end;
	unsigned long slot = strtoul(argv[1], &end, 10);
	if (*end != '\0' || slot >= APP_ALARM_SLOT_COUNT) {
		shell_error(sh, "usage: alarm clear <index>|all");
		return -EINVAL;
	}
	int ret = app_alarm_rules_clear((uint8_t)slot);
	if (ret) {
		shell_error(sh, "clear failed: %d%s", ret, ret == -ENOENT ? " (slot empty)" : "");
		return ret;
	}
	(void)app_alarm_rules_save();
	shell_print(sh, "slot %lu cleared", slot);
	app_lrw_arm_config_report(); /* #320: push the local change to the backend */
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_alarm,
	SHELL_CMD_ARG(list, NULL, "List alarm rules, or one slot. Usage: list [<index>]",
		      cmd_alarm_list, 1, 1),
	SHELL_CMD_ARG(set, NULL,
		      "Set a rule in a slot. Usage: set <index> <source> <quantity> <args>\n"
		      "  threshold: <lo> <hi> [hst]   state: <from> <to>   count: <N>",
		      cmd_alarm_set, 5, 2),
	SHELL_CMD_ARG(new, NULL,
		      "Set a rule in the first free slot. Usage: new <source> <quantity> <args>\n"
		      "  threshold: <lo> <hi> [hst]   state: <from> <to>   count: <N>",
		      cmd_alarm_new, 4, 2),
	SHELL_CMD_ARG(clear, NULL, "Clear a slot (or all). Usage: clear <index>|all",
		      cmd_alarm_clear, 1, 1),
	SHELL_CMD_ARG(poll, NULL, "Sample + evaluate rules now (bench test).", cmd_alarm_poll, 1,
		      0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(alarm, &sub_alarm, "Dynamic alarm rules.", NULL);
#endif /* defined(CONFIG_SHELL) */

static int app_alarm_init(void)
{
	k_work_init_delayable(&m_alarm_batch_work, alarm_batch_work_handler);
	return 0;
}

SYS_INIT(app_alarm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
