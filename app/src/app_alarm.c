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

/* Wire enum values (AlarmEvent.Side / .Edge). */
#define ALARM_SIDE_NONE  0
#define ALARM_SIDE_LO    1
#define ALARM_SIDE_HI    2
#define ALARM_EDGE_ACT   0
#define ALARM_EDGE_DEACT 1

/* ---- per-rule runtime state (keyed by source+quantity, not rule index, so it
 * survives the rule list being reordered on clear) -------------------------- */

struct rstate {
	bool used;
	uint8_t source;
	uint8_t quantity;
	bool active;        /* alarm latched */
	uint8_t side;       /* threshold: which bound latched */
	uint8_t prev_state; /* STATE: last digital level seen */
	bool have_prev_state;
	uint32_t prev_count; /* COUNT: baseline counter */
	bool have_prev_count;
	int64_t oneshot_expiry; /* STATE edge one-shot: auto-clear time (0 = none) */
};

static struct rstate m_rt[APP_ALARM_RULE_MAX];
static int64_t m_last_alarm_send_ms;
static app_alarm_event_cb m_event_cb;
static void *m_event_cb_user_data;

K_MUTEX_DEFINE(m_lock);

static struct rstate *rt_for(uint8_t source, uint8_t quantity)
{
	struct rstate *free = NULL;
	for (int i = 0; i < APP_ALARM_RULE_MAX; i++) {
		if (m_rt[i].used && m_rt[i].source == source && m_rt[i].quantity == quantity) {
			return &m_rt[i];
		}
		if (!m_rt[i].used && free == NULL) {
			free = &m_rt[i];
		}
	}
	if (free) {
		*free = (struct rstate){.used = true, .source = source, .quantity = quantity};
	}
	return free;
}

/* ---- Alarm-detail batch on fPort 3 (#27) -------------------------------- */

#define ALARM_BATCH_MAX 8
#define ALARM_FRAME_MAX 64

static struct app_cmd_alarm_event m_batch[ALARM_BATCH_MAX];
static uint8_t m_batch_count;
static uint16_t m_window_total;
static uint32_t m_window_base_unix;
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

	if (limit > 0 && m_last_alarm_send_ms != 0 &&
	    (now - m_last_alarm_send_ms) < (int64_t)limit * 1000) {
		LOG_INF("Alarm uplink rate-limited (limit %d s)", limit);
		return;
	}
	m_last_alarm_send_ms = now;
	app_lrw_send();
#endif
}

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

/* Record one alarm edge for the fPort-3 detail batch. Caller does NOT hold
 * m_lock (this takes it). */
static void alarm_collect(uint8_t source, uint8_t quantity, bool active, uint8_t side,
			  bool has_value, int32_t value)
{
	int limit = g_app_config.alarm_limit;
	int64_t now = k_uptime_get();
	struct app_cmd_alarm_event ev = {
		.source = source,
		.quantity = quantity,
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
static bool eval_threshold(const struct app_alarm_rule *rule, struct rstate *rt, float value,
			   bool *should_send)
{
	if (!rule->enabled || isnan(value)) {
		if (rt->active) {
			rt->active = false;
			*should_send = true;
			alarm_collect(rule->source, rule->quantity, false, rt->side, false, 0);
		}
		return false;
	}

	float lo = rule->lo, hi = rule->hi, hst = rule->hst;

	if (rt->active) {
		if (value > lo + hst && value < hi - hst) {
			rt->active = false;
			*should_send = true;
			alarm_collect(rule->source, rule->quantity, false, rt->side, true,
				      alarm_scale(rule->quantity, value));
		}
	} else if (value < lo - hst) {
		rt->active = true;
		rt->side = ALARM_SIDE_LO;
		*should_send = true;
		alarm_collect(rule->source, rule->quantity, true, ALARM_SIDE_LO, true,
			      alarm_scale(rule->quantity, value));
	} else if (value > hi + hst) {
		rt->active = true;
		rt->side = ALARM_SIDE_HI;
		*should_send = true;
		alarm_collect(rule->source, rule->quantity, true, ALARM_SIDE_HI, true,
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
static void eval_state(const struct app_alarm_rule *rule, struct rstate *rt, bool cur,
		       bool *should_send)
{
	bool is_edge = (rule->from_state != rule->to_state);
	uint8_t cur_lvl = cur ? 1 : 0;

	if (!rule->enabled) {
		if (rt->active) {
			rt->active = false;
			rt->oneshot_expiry = 0;
			*should_send = true;
			alarm_collect(rule->source, rule->quantity, false, ALARM_SIDE_NONE, true,
				      0);
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
		if (cur && !rt->active) {
			rt->active = true;
			rt->oneshot_expiry = k_uptime_get() + notif_hold_ms();
			*should_send = true;
			alarm_collect(rule->source, rule->quantity, true, ALARM_SIDE_NONE, true,
				      cur_lvl);
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
			alarm_collect(rule->source, rule->quantity, true, ALARM_SIDE_NONE, true,
				      cur_lvl);
		}
	} else {
		/* Level: active while cur == to. */
		bool want = (cur_lvl == rule->to_state);
		if (want && !rt->active) {
			rt->active = true;
			*should_send = true;
			alarm_collect(rule->source, rule->quantity, true, ALARM_SIDE_NONE, true,
				      cur_lvl);
		} else if (!want && rt->active) {
			rt->active = false;
			*should_send = true;
			alarm_collect(rule->source, rule->quantity, false, ALARM_SIDE_NONE, true,
				      cur_lvl);
		}
	}

	rt->have_prev_state = true;
	rt->prev_state = cur_lvl;
}

/* COUNT rate rule: alarm when the counter rose by >= hi since the last fire.
 * The underlying counters in g_app_sensor_data only refresh per sample/report,
 * so this is effectively a per-report rate. One-shot (auto-clear). */
static void eval_count(const struct app_alarm_rule *rule, struct rstate *rt, uint32_t cur,
		       bool *should_send)
{
	if (!rt->have_prev_count) {
		rt->have_prev_count = true;
		rt->prev_count = cur;
		return;
	}
	if (!rule->enabled) {
		rt->prev_count = cur;
		return;
	}
	uint32_t delta = cur - rt->prev_count; /* wraps correctly on uint32 */
	if (rule->hi > 0 && delta >= (uint32_t)rule->hi) {
		rt->active = true;
		rt->oneshot_expiry = k_uptime_get() + notif_hold_ms();
		*should_send = true;
		alarm_collect(rule->source, rule->quantity, true, ALARM_SIDE_NONE, true,
			      (int32_t)cur);
		rt->prev_count = cur;
	}
}

bool app_alarm_poll(void)
{
	bool should_send = false;
	bool alarm = false;
	int64_t now = k_uptime_get();

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	for (uint8_t i = 0; i < app_alarm_rules_count(); i++) {
		const struct app_alarm_rule *rule = app_alarm_rules_get(i);
		if (rule == NULL) {
			break;
		}
		struct rstate *rt = rt_for(rule->source, rule->quantity);
		if (rt == NULL) {
			continue;
		}

		switch (app_alarm_quantity_kind((enum app_alarm_quantity)rule->quantity)) {
		case APP_ALARM_KIND_THRESHOLD: {
			float v = NAN;
			read_threshold_value(rule->source, rule->quantity, &v);
			eval_threshold(rule, rt, v, &should_send);
			break;
		}
		case APP_ALARM_KIND_STATE: {
			/* Only slot tilt is polled here; hall/input/pir/accel come via
			 * app_alarm_event(). */
			bool st;
			if (read_poll_state(rule->source, rule->quantity, &st)) {
				eval_state(rule, rt, st, &should_send);
			}
			break;
		}
		case APP_ALARM_KIND_RATE: {
			uint32_t c;
			if (read_counter(rule->source, &c)) {
				eval_count(rule, rt, c, &should_send);
			}
			break;
		}
		}
	}

	k_mutex_unlock(&g_app_sensor_data_lock);

	/* Expire one-shot latches and collect "any active". */
	k_mutex_lock(&m_lock, K_FOREVER);
	for (int i = 0; i < APP_ALARM_RULE_MAX; i++) {
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
	/* Discrete edge → its STATE rule (if any). */
	const struct app_alarm_rule *rule = app_alarm_rules_find(source, APP_ALARM_Q_STATE);
	app_alarm_event_cb cb;
	void *user_data;

	k_mutex_lock(&m_lock, K_FOREVER);
	cb = m_event_cb;
	user_data = m_event_cb_user_data;
	k_mutex_unlock(&m_lock);

	if (rule != NULL) {
		struct rstate *rt = rt_for(source, APP_ALARM_Q_STATE);
		if (rt != NULL) {
			bool should_send = false;
			eval_state(rule, rt, active, &should_send);
			if (should_send) {
				alarm_lrw_send();
			}
		}
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
	for (int i = 0; i < APP_ALARM_RULE_MAX; i++) {
		if (m_rt[i].used && m_rt[i].source == source && m_rt[i].quantity == quantity) {
			a = m_rt[i].active;
			break;
		}
	}
	k_mutex_unlock(&m_lock);
	return a;
}

/* ---- shell -------------------------------------------------------------- */

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <string.h>

static int cmd_alarm_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint8_t n = app_alarm_rules_count();
	shell_print(sh, "%u rule(s):", n);
	for (uint8_t i = 0; i < n; i++) {
		const struct app_alarm_rule *r = app_alarm_rules_get(i);
		const char *src = app_alarm_source_name(r->source);
		const char *q = app_alarm_quantity_name(r->quantity);
		bool active = app_alarm_is_active(r->source, r->quantity);
		switch (app_alarm_quantity_kind(r->quantity)) {
		case APP_ALARM_KIND_THRESHOLD:
			shell_print(sh, "  %s %s  lo=%.2f hi=%.2f hst=%.2f  en=%d  active=%d", src,
				    q, (double)r->lo, (double)r->hi, (double)r->hst, r->enabled,
				    active);
			break;
		case APP_ALARM_KIND_STATE:
			shell_print(sh, "  %s %s  %u->%u (%s)  en=%d  active=%d", src, q,
				    r->from_state, r->to_state,
				    r->from_state == r->to_state ? "level" : "edge", r->enabled,
				    active);
			break;
		case APP_ALARM_KIND_RATE:
			shell_print(sh, "  %s %s  rate>=%d/interval  en=%d  active=%d", src, q,
				    (int)r->hi, r->enabled, active);
			break;
		}
	}
	return 0;
}

static int cmd_alarm_set(const struct shell *sh, size_t argc, char **argv)
{
	int src = app_alarm_source_by_name(argv[1]);
	int qty = app_alarm_quantity_by_name(argv[2]);
	if (src < 0 || qty < 0 ||
	    !app_alarm_rule_valid((enum app_alarm_source)src, (enum app_alarm_quantity)qty)) {
		shell_error(sh, "invalid <source> <quantity>");
		return -EINVAL;
	}

	struct app_alarm_rule r = {.source = (uint8_t)src, .quantity = (uint8_t)qty, .enabled = 1};

	switch (app_alarm_quantity_kind((enum app_alarm_quantity)qty)) {
	case APP_ALARM_KIND_THRESHOLD:
		if (argc < 5) {
			shell_error(sh, "usage: alarm set <source> <quantity> <lo> <hi> [hst]");
			return -EINVAL;
		}
		r.lo = strtof(argv[3], NULL);
		r.hi = strtof(argv[4], NULL);
		r.hst = (argc >= 6) ? strtof(argv[5], NULL) : 0.0f;
		break;
	case APP_ALARM_KIND_STATE:
		if (argc < 5) {
			shell_error(sh, "usage: alarm set <source> <quantity> <from> <to> "
					"(0/1; from!=to=edge, from==to=level)");
			return -EINVAL;
		}
		r.from_state = (uint8_t)(strtoul(argv[3], NULL, 10) ? 1 : 0);
		r.to_state = (uint8_t)(strtoul(argv[4], NULL, 10) ? 1 : 0);
		break;
	case APP_ALARM_KIND_RATE:
		if (argc < 4) {
			shell_error(sh, "usage: alarm set <source> count <N-per-interval>");
			return -EINVAL;
		}
		r.hi = (float)strtoul(argv[3], NULL, 10);
		break;
	}

	int ret = app_alarm_rules_set(&r);
	if (ret) {
		shell_error(sh, "set failed: %d%s", ret,
			    ret == -ENOSPC ? " (rule table full)" : "");
		return ret;
	}
	ret = app_alarm_rules_save();
	shell_print(sh, "rule set%s", ret ? " (NOT persisted!)" : "");
	return 0;
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
	if (argc == 1) {
		app_alarm_rules_clear_all();
		(void)app_alarm_rules_save();
		shell_print(sh, "all rules cleared");
		return 0;
	}
	int src = app_alarm_source_by_name(argv[1]);
	int qty = (argc >= 3) ? app_alarm_quantity_by_name(argv[2]) : -1;
	if (src < 0 || qty < 0) {
		shell_error(sh, "usage: alarm clear [<source> <quantity>]");
		return -EINVAL;
	}
	int ret = app_alarm_rules_clear((enum app_alarm_source)src, (enum app_alarm_quantity)qty);
	if (ret) {
		shell_error(sh, "clear failed: %d", ret);
		return ret;
	}
	(void)app_alarm_rules_save();
	shell_print(sh, "rule cleared");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_alarm, SHELL_CMD_ARG(list, NULL, "List alarm rules.", cmd_alarm_list, 1, 0),
	SHELL_CMD_ARG(set, NULL,
		      "Set a rule. Usage: set <source> <quantity> <args>\n"
		      "  threshold: <lo> <hi> [hst]   state: <from> <to>   count: <N>",
		      cmd_alarm_set, 4, 2),
	SHELL_CMD_ARG(clear, NULL, "Clear a rule (or all). Usage: clear [<source> <quantity>]",
		      cmd_alarm_clear, 1, 2),
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
