/*
 * Copyright (c) 2025-2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_alarm_rules.h"
#include "app_buzzer.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_log.h"
#include "app_radio.h"
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
#include <string.h>

LOG_MODULE_REGISTER(app_alarm, LOG_LEVEL_DBG);

/* No-data watchdog: a motherboard liveness channel (app_sensor_types.yaml) of an
 * enabled sensor, a 1-Wire part or a whole 1-Wire device that reads NaN
 * continuously for this long is reported as stopped (a no_data AlarmEvent).
 * Drivers already retry transient I2C/1-Wire errors, so a NaN here is a
 * confirmed read failure; the window only rejects a single missed sample. The watchdogs are
 * independent of the 16 alarm rules, so their events carry rule = APP_ALARM_WATCHDOG_RULE. */
#define APP_ALARM_NO_DATA_MS    5000
#define APP_ALARM_WATCHDOG_RULE 0xFF

/* Low-battery watchdog (#210): raise an alarm (slot 0, channel battery-voltage,
 * type=low) when the supply drops below the configurable
 * `battery_level` threshold (config in mV, default 2400 — Li cells discharge
 * non-linearly so the warning level is left to the integrator) and clear it once
 * it recovers past the hysteresis band. Bench-measured min operating voltage is
 * ~1.3 V (the node wedges silently below that and only a power-cycle recovers
 * it), so the default 2.4 V warns the backend with margin to spare. Like the
 * no-data watchdog it is independent of the 16 rules (rule = 0xFE) and it
 * does drive the red LED (it is an alarm). */
#define APP_ALARM_BATTERY_HYST_V 0.3f /* fixed: recover above threshold + 0.3 V (anti-chatter) */
#define APP_ALARM_BATTERY_RULE   0xFE

/* Wire enum values (AlarmEvent.Type / .Edge). type says WHAT fired (#212). */
#define ALARM_TYPE_NONE            0
#define ALARM_TYPE_LOW             1
#define ALARM_TYPE_HIGH            2
#define ALARM_TYPE_TRIGGER         3
#define ALARM_TYPE_NO_DATA         4
#define ALARM_TYPE_SENSOR_MISMATCH 5
#define ALARM_EDGE_ACT             0
#define ALARM_EDGE_DEACT           1

/* ---- per-rule runtime state, keyed 1:1 on the rule index ----------------
 * m_rt[idx] is the runtime latch for rule idx. The rule is snapshotted
 * (last_rule) so a rule that is cleared or edited resets its latch and the
 * deactivate edge still names the old target (see rt_sync). Two rules on one
 * channel latch independently. */

struct rstate {
	bool used;
	bool active;  /* alarm latched */
	uint8_t type; /* threshold: which bound latched (ALARM_TYPE_LOW/HIGH) for deactivate */
	uint8_t prev_state; /* STATE: last digital level seen */
	bool have_prev_state;
	uint32_t prev_count; /* COUNT: baseline counter at window start */
	bool have_prev_count;
	int64_t count_window_start; /* COUNT: start of the current interval_report window (#195) */
	int64_t oneshot_expiry;     /* STATE edge / momentary / COUNT one-shot: auto-clear time (0 =
				       none) */
	int64_t confirm_deadline; /* dwell-before-activation deadline (#348, 0 = none). While armed,
				   * `type` doubles as the pending THRESHOLD side (LOW/HIGH) being
				   * confirmed, since it is otherwise unused until the rule latches.
				   */
	struct app_alarm_rule last_rule; /* full rule snapshot as of the last rt_sync() reset, so an
					  * edit that only changes e.g. lo/hi/dwell/enabled/
					  * from_state/to_state is still detected and resets the
					  * latch below; also the target of its deactivate edge. */
};

static struct rstate m_rt[APP_ALARM_RULE_COUNT];
/* -1 = "never sent" sentinel; k_uptime_get() legitimately returns 0 in the first
 * millisecond after boot, so 0 cannot mark "never sent" without a window where the
 * rate limit is silently skipped (#219). */
static int64_t m_last_alarm_send_ms = -1;
static app_alarm_event_cb m_event_cb;
static void *m_event_cb_user_data;

/* #397: per-alarm active bitmask as of the last app_alarm_poll() + monotonic
 * activation sequence (bumped once per poll that latched a NEW alarm). Both
 * written by app_alarm_poll() under m_lock and exposed read-only via
 * app_alarm_active_mask()/app_alarm_activation_seq(). */
static uint32_t m_active_mask;
static uint32_t m_activation_seq;

K_MUTEX_DEFINE(m_lock);

static void alarm_collect(uint8_t idx, const struct app_alarm_rule *rule, bool active, uint8_t type,
			  bool has_value, int32_t value);

/* Return rule `idx`, syncing its runtime latch: an empty rule (or one that
 * changed at all since the latch was last reset — any field, e.g. a live edit
 * of lo/hi/dwell/enabled/from_state/to_state on the SAME channel) is reset.
 * Without this, an already-active/pending latch would carry over under the
 * edited rule's new parameters. Returns false for an empty rule.
 *
 * Resetting an ACTIVE latch must also emit the alarm-batch deactivate edge here:
 * this reset runs before the eval_* dispatch, so eval_threshold()/eval_state()'s
 * own !rule->enabled deactivate branches can never see the pre-reset latch for
 * the disable/clear/edit transition — without this, a backend pairing
 * activate/deactivate edges is left with a dangling activate. Runs under m_lock
 * (recursive), so the nested alarm_collect() re-lock is fine. */
static bool rt_sync(uint8_t idx, struct app_alarm_rule *out, bool *should_send)
{
	struct rstate *rt = &m_rt[idx];

	if (!app_alarm_rules_get(idx, out)) {
		if (rt->used) {
			if (rt->active) {
				alarm_collect(idx, &rt->last_rule, false, rt->type, false, 0);
				*should_send = true;
			}
			*rt = (struct rstate){0};
		}
		return false;
	}
	if (!rt->used || memcmp(&rt->last_rule, out, sizeof(*out)) != 0) {
		if (rt->active) {
			alarm_collect(idx, &rt->last_rule, false, rt->type, false, 0);
			*should_send = true;
		}
		*rt = (struct rstate){.used = true, .last_rule = *out};
	}
	return true;
}

/* rt_sync() + the inert check (#430): a stale rule, or a motherboard rule
 * whose capability is off, is evaluated as disabled, so a latched alarm emits
 * its deactivate edge and nothing new fires. */
static bool rt_sync_armed(uint8_t idx, struct app_alarm_rule *out, bool *should_send)
{
	if (!rt_sync(idx, out, should_send)) {
		return false;
	}
	if (!app_alarm_rule_armed(out)) {
		out->enabled = 0;
	}
	return true;
}

/* ---- Alarm-detail batch (#27) ------------------------------------------- */

#define ALARM_BATCH_MAX 8
#define ALARM_FRAME_MAX 64

static struct app_cmd_alarm_event m_batch[ALARM_BATCH_MAX];
static uint8_t m_batch_count;
static uint16_t m_window_total;
static uint32_t m_window_base_unix;
static bool m_window_base_synced; /* m_window_base_unix is absolute UTC, not uptime */
static int64_t m_window_start_ms;
static bool m_window_open;
/* The batch waits for the radio: link down, the boot/join announce is still
 * going out (Info -> settings-info -> data, 2026-09-27), or its pages do not
 * fit the free alarm slots (#462). m_lock. */
static bool m_batch_held;
static struct k_work_delayable m_alarm_batch_work;

/* Per-rule dwell/hold duration in ms (#348): `dwell` is a plain duration in
 * seconds, unified across every kind that uses a timer (THRESHOLD dwell-before-
 * activate, STATE edge confirm+hold, STATE level dwell-before-activate,
 * momentary-source hold/re-arm, RATE hold/re-arm). Negative/NaN clamps to 0
 * (immediate — never widens a window from garbage data). */
static inline int64_t rule_hold_ms(const struct app_alarm_rule *rule)
{
	float s = rule->dwell;
	if (!(s >= 0.0f)) {
		s = 0.0f;
	}
	return (int64_t)(s * 1000.0f);
}

/* Send the alarm state now, rate-limited by alarm_limit. Goes through
 * app_report/app_radio, so it is the same on every radio. */
static void alarm_send(void)
{
	int limit = g_app_config.alarm_limit;
	int64_t now = k_uptime_get();
	bool send;

	/* m_last_alarm_send_ms is read-modify-written from three contexts (main
	 * poll, PIR thread, system WQ). A 64-bit RMW is not atomic on Cortex-M4, so
	 * guard it under m_lock (#93.6); release before app_report_trigger() so the
	 * radio call never runs while holding the alarm lock. */
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
}

#if defined(CONFIG_APP_BUZZER)
/* #397: melody replay interval per alarm_buzzer_mode while any alarm stays
 * active. 0 = play once per new activation. RESERVED_6/7 fall back to NORMAL
 * until a future firmware assigns them their own behavior. */
static uint16_t alarm_buzzer_repeat_s(enum app_config_alarm_buzzer_mode mode)
{
	switch (mode) {
	case APP_CONFIG_ALARM_BUZZER_MODE_ONCE:
		return 0;
	case APP_CONFIG_ALARM_BUZZER_MODE_SLOW:
		return 120;
	case APP_CONFIG_ALARM_BUZZER_MODE_FAST:
		return 10;
	case APP_CONFIG_ALARM_BUZZER_MODE_CONTINUOUS:
		return 1;
	case APP_CONFIG_ALARM_BUZZER_MODE_NORMAL:
	default:
		return 30;
	}
}

/* #397: drive the local buzzer as an audible alarm indicator off the central
 * activation-edge detection in app_alarm_poll() (new_bits / all_cleared come
 * from the per-alarm active bitmask — covering every alarm type, unlike
 * app_alarm_event()'s per-source callback, which only sees raw GPIO edges).
 * Every non-off mode replays the melody whenever a NEW bit appears — even
 * while other alarms are already active — which also restarts the mode's
 * repeat cycle; the modes differ only in the replay interval
 * (alarm_buzzer_repeat_s above). Once the last bit clears, stop immediately.
 * new_bits is non-zero only in the one poll where an alarm actually latched,
 * so enabling cap_buzzer mid-alarm never fires a spurious start. */
static void alarm_buzzer_sync(uint32_t new_bits, bool all_cleared)
{
	if (!g_app_config.cap_buzzer) {
		return;
	}

	if (all_cleared) {
		/* Sent regardless of mode: covers a mode switched to off (or a
		 * repeat cycle from a previous mode) while the alarm was active. */
		app_buzzer_play_repeating(APP_BUZZER_KIND_STOP, 0);
		return;
	}

	if (new_bits == 0 || g_app_config.alarm_buzzer_mode == APP_CONFIG_ALARM_BUZZER_MODE_OFF) {
		return;
	}

	/* No special arbitration against a concurrent remote buzzer_play: the
	 * melody engine's single-slot queue already wakes a getter (idle or
	 * mid repeat-wait) immediately on any new request ("newest replaces
	 * not-yet-started"), so this naturally preempts an unrelated melody. A
	 * later deactivation's STOP can also silence an unrelated queued remote
	 * melody — symmetric trade-off, not worth extra source-tracking. */
	app_buzzer_play_repeating(APP_BUZZER_KIND_ALARM,
				  alarm_buzzer_repeat_s(g_app_config.alarm_buzzer_mode));
}
#else
/* #395: buzzer not built into this FW — alarm indication over the buzzer is a
 * no-op (the alarm engine itself, uplinks and the active mask are unaffected). */
static void alarm_buzzer_sync(uint32_t new_bits, bool all_cleared)
{
	ARG_UNUSED(new_bits);
	ARG_UNUSED(all_cleared);
}
#endif /* defined(CONFIG_APP_BUZZER) */

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

	/* Boot/join order (Hynek, 2026-09-27): Info -> settings-info -> data, and
	 * an alarm is data. While the link is down or the announce is still going
	 * out the batch waits with its window held open, so later edges join it;
	 * app_radio flushes it once the announce is queued (app_alarm_flush_held()),
	 * and the timer covers the announce's fallback deadline. */
	int32_t hold_ms = app_radio_data_hold_ms();

	if (hold_ms != 0) {
		if (!m_batch_held) {
			LOG_INF("Alarm batch held: %s",
				hold_ms < 0 ? "link down" : "boot/join announce first");
		}
		m_batch_held = true;
		m_window_open = true;
		if (hold_ms > 0) {
			k_work_reschedule(&m_alarm_batch_work, K_MSEC(hold_ms));
		}
		return;
	}
	bool was_held = m_batch_held;

	m_batch_held = false;

	size_t cap = ALARM_FRAME_MAX;
	/* 0 = budget unknown right now: encode against the buffer and let the
	 * radio defer (#409 3a). */
	uint8_t dr = app_radio_get_max_payload();

	if (dr > 0 && dr < cap) {
		cap = dr;
	}

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

	/* #409 3b / #425: split the batch across as many AlarmReport frames as the
	 * budget needs (each self-contained: same base_time/total/time_synced) and
	 * number them page_index/page_count like every other paged answer. Pass 1
	 * lays the pages out greedily with worst-case (one-byte) page numbers, so the
	 * real numbers in pass 2 never make a page grow. */
#define ALARM_PAGE_BOUND 127
	uint8_t buf[ALARM_FRAME_MAX];
	uint8_t page_n[ALARM_BATCH_MAX];
	uint8_t pages = 0;
	uint8_t laid = 0;

	while (laid < m_batch_count) {
		size_t len = 0;
		uint8_t n = m_batch_count - laid;
		int ret = -EMSGSIZE;

		while (n > 0) {
			ret = app_cmd_build_alarm_report(m_window_base_unix, m_window_total, synced,
							 &m_batch[laid], n, ALARM_PAGE_BOUND,
							 ALARM_PAGE_BOUND, buf, cap, &len);
			if (ret == 0) {
				break;
			}
			n--;
		}
		if (ret != 0) {
			/* Not even one event fits (the 11 B budget tier): no alarm
			 * detail for the rest. The alarm state still reaches the server
			 * through the alarm bits in every telemetry frame. */
			LOG_WRN("Alarm detail skipped: %u event(s) do not fit %u B; state is "
				"in telemetry system_flags",
				m_batch_count - laid, (unsigned)cap);
			break;
		}
		page_n[pages++] = n;
		laid += n;
	}

	/* #462: a burst must not overflow the radio's alarm queue (4 frames). While
	 * queued frames still drain, the batch waits held and later edges join it
	 * (fewer, fuller frames); app_radio flushes it when it takes the next alarm
	 * frame. An empty queue takes the batch whatever its size: there is no
	 * frame left to release it. */
	uint32_t room = app_radio_tx_alarm_free();

	if (pages > room && room < APP_RADIO_TX_QUEUE_DEPTH) {
		if (!was_held) {
			LOG_INF("Alarm batch held: %u page(s), %u alarm slot(s) free", pages,
				(unsigned)room);
		}
		m_batch_held = true;
		m_window_open = true;
		return;
	}

	for (uint8_t p = 0, first = 0; p < pages; first += page_n[p], p++) {
		size_t len = 0;
		int ret = app_cmd_build_alarm_report(m_window_base_unix, m_window_total, synced,
						     &m_batch[first], page_n[p], p, pages, buf, cap,
						     &len);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_cmd_build_alarm_report", ret);
			break;
		}
		(void)app_radio_send_alarm(buf, len);
		LOG_INF("Alarm batch page %u/%u: events %u..%u of %u (%u B)", p + 1, pages,
			first + 1, first + page_n[p], m_window_total, (unsigned)len);
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

void app_alarm_flush_held(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);

	bool held = m_batch_held;

	k_mutex_unlock(&m_lock);
	if (held) {
		k_work_reschedule(&m_alarm_batch_work, K_NO_WAIT);
	}
}

bool app_alarm_flush_pending(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);

	bool waiting = m_batch_count > 0 && app_radio_data_hold_ms() == 0;

	k_mutex_unlock(&m_lock);
	if (waiting) {
		k_work_reschedule(&m_alarm_batch_work, K_NO_WAIT);
	}
	return waiting;
}

/* Record one alarm edge for the alarm-detail batch. Caller does NOT hold
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
		/* Normally a batch of one; a batch held for the radio (see
		 * alarm_batch_flush()) collects the edges until it can go. */
		if (m_batch_count == 0) {
			m_window_start_ms = now;
			m_window_base_unix = now_seconds(&m_window_base_synced);
			m_window_total = 0;
		}
		m_window_total++;
		if (m_batch_count < ALARM_BATCH_MAX) {
			ev.rel_s = (uint32_t)MIN((now - m_window_start_ms) / 1000, 0xFFFF);
			m_batch[m_batch_count++] = ev;
		}
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

/* Wire value of a reading on channel `c`: the physical value × its wire scale
 * (app_sensor_types.yaml, #430). */
static int32_t alarm_scale(const struct app_sensor_channel *c, float v)
{
	return (int32_t)lroundf(v * (c != NULL ? c->wire_scale : 1.0f));
}

static int32_t rule_scale(const struct app_alarm_rule *rule, float v)
{
	return alarm_scale(app_alarm_rule_channel(rule), v);
}

static void alarm_collect(uint8_t idx, const struct app_alarm_rule *rule, bool active, uint8_t type,
			  bool has_value, int32_t value)
{
	struct app_cmd_alarm_event ev = {
		.rule = idx,
		.slot = rule->slot,
		.channel = rule->channel,
		.edge = active ? ALARM_EDGE_ACT : ALARM_EDGE_DEACT,
		.type = type,
		.sensor_type = rule->sensor_type,
		.has_value = has_value,
		.value = value,
		.rel_s = 0,
	};
	alarm_queue(ev);
}

/* No-data watchdog event: a liveness channel, a 1-Wire part (its first
 * channel) or a whole 1-Wire device (APP_SENSOR_CH_DEVICE) stopped reporting
 * (NaN for >= APP_ALARM_NO_DATA_MS) or recovered. Not tied to a rule (rule = 0xFF). */
static void alarm_collect_nodata(uint8_t slot, uint8_t channel, uint8_t sensor_type, bool active)
{
	struct app_cmd_alarm_event ev = {
		.rule = APP_ALARM_WATCHDOG_RULE,
		.slot = slot,
		.channel = channel,
		.edge = active ? ALARM_EDGE_ACT : ALARM_EDGE_DEACT,
		.type = ALARM_TYPE_NO_DATA,
		.sensor_type = sensor_type,
		.has_value = false,
		.value = 0,
		.rel_s = 0,
	};
	alarm_queue(ev);
}

/* Sensor-mismatch watchdog event (#430): a 1-Wire slot holds a device of
 * another type than its sensorN_type. sensor_type = the expected type, value =
 * the detected type; no channel (0). Not tied to a rule (0xFF). */
static void alarm_collect_mismatch(uint8_t w1_slot, uint8_t expected, uint8_t detected, bool active)
{
	struct app_cmd_alarm_event ev = {
		.rule = APP_ALARM_WATCHDOG_RULE,
		.slot = w1_slot + 1,
		.channel = 0,
		.edge = active ? ALARM_EDGE_ACT : ALARM_EDGE_DEACT,
		.type = ALARM_TYPE_SENSOR_MISMATCH,
		.sensor_type = expected,
		.has_value = true,
		.value = detected,
		.rel_s = 0,
	};
	alarm_queue(ev);
}

/* Low-battery watchdog event (#210): supply dropped below / recovered above the
 * threshold. type=low always (a falling supply); the deactivate edge marks the
 * recovery. Carries the current voltage at the battery-voltage channel's wire
 * scale (mV) and rule = 0xFE. */
static void alarm_collect_battery(bool active, float voltage)
{
	struct app_cmd_alarm_event ev = {
		.rule = APP_ALARM_BATTERY_RULE,
		.slot = APP_ALARM_SLOT_MB,
		.channel = APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE,
		.edge = active ? ALARM_EDGE_ACT : ALARM_EDGE_DEACT,
		.type = ALARM_TYPE_LOW,
		.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
		.has_value = true,
		.value = alarm_scale(
			app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
					       APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE),
			voltage),
		.rel_s = 0,
	};
	alarm_queue(ev);
}

/* ---- value access (slot, channel) --------------------------------------- */

/* Float reading of channel `ch` in `slot`, NaN when absent or when a 1-Wire
 * slot currently holds another type than `sensor_type`. Caller holds
 * g_app_sensor_data_lock. */
static float read_value(uint8_t slot, uint8_t ch, uint8_t sensor_type)
{
	const struct app_sensor_data *d = &g_app_sensor_data;

	if (slot == APP_ALARM_SLOT_MB) {
		return ch < APP_SENSOR_CH_MOTHERBOARD_COUNT ? d->mb.v[ch].f : NAN;
	}
	if (slot > APP_ALARM_SLOT_MAX || ch >= APP_SENSOR_W1_CH_MAX ||
	    d->w1[slot - 1].type != sensor_type) {
		return NAN;
	}
	return d->w1[slot - 1].v[ch].f;
}

/* Current counter of a RATE rule (a motherboard counter channel). Caller holds
 * the data lock. */
static bool read_counter(const struct app_alarm_rule *rule, const struct app_sensor_channel *c,
			 uint32_t *out)
{
	if (rule->slot != APP_ALARM_SLOT_MB || !(c->flags & APP_SENSOR_F_COUNTER)) {
		return false;
	}
	*out = g_app_sensor_data.mb.v[rule->channel].u;
	return true;
}

/* Current digital level for a STATE rule, re-sampled every app_alarm_poll (#348).
 * hall/input additionally arrive event-driven via app_alarm_event() for immediate
 * reaction (fall-through cancel, momentary fire), but a confirm/dwell armed by
 * eval_state() only gets *resolved* once its deadline elapses — which, without
 * this poll-time re-evaluation, would never happen while the level simply holds
 * steady with no further edge to re-invoke eval_state(). Feeding the unchanged
 * current level back in here every ~3s is what lets that "now >= deadline" check
 * actually run. Momentary channels (PIR / accelerometer motion) fire immediately,
 * no confirm phase, so they have no deadline to resolve this way — their only
 * timer is oneshot_expiry, already swept centrally in app_alarm_poll().
 *
 * Hall/input read their OWN live data (app_hall_get_data()/app_input_get_data()),
 * not g_app_sensor_data: that cache is only refreshed by app_sensor_sample(),
 * which runs on the interval_sample timer (started only if interval_sample != 0,
 * app_sensor.c) or a manual sample/`alarm poll` — with interval_sample == 0 (a
 * valid, common config) the hall/input state channels would stay frozen at
 * their boot-time value between app_alarm_poll() calls, silently defeating this
 * exact re-evaluation. app_hall.c/app_input.c run their own independent 100 ms
 * GPIO timer regardless of interval_sample, so their getters are always fresh.
 * Any other state channel (1-Wire tilt) reads the sensor cache. */
static bool read_poll_state(const struct app_alarm_rule *rule, const struct app_sensor_channel *c,
			    bool *out)
{
	if (c->flags & APP_SENSOR_F_MOMENTARY) {
		return false;
	}
	if (rule->slot == APP_ALARM_SLOT_MB) {
		switch (rule->channel) {
		case APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE:
		case APP_SENSOR_CH_MOTHERBOARD_HALL_RIGHT_STATE: {
			struct app_hall_data hd;
			if (app_hall_get_data(&hd)) {
				return false;
			}
			*out = (rule->channel == APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE)
				       ? hd.left_is_active
				       : hd.right_is_active;
			return true;
		}
		case APP_SENSOR_CH_MOTHERBOARD_INPUT_A_STATE:
		case APP_SENSOR_CH_MOTHERBOARD_INPUT_B_STATE: {
			struct app_input_data id;
			if (app_input_get_data(&id)) {
				return false;
			}
			*out = (rule->channel == APP_SENSOR_CH_MOTHERBOARD_INPUT_A_STATE)
				       ? id.input_a_is_active
				       : id.input_b_is_active;
			return true;
		}
		default:
			break;
		}
	}

	float v = read_value(rule->slot, rule->channel, rule->sensor_type);

	if (isnan(v)) {
		return false;
	}
	*out = v == 1.0f;
	return true;
}

/* ---- rule evaluation ---------------------------------------------------- */

/* Threshold rule (#348): lo/hi are a plain band, no hysteresis. `dwell`
 * (seconds) is a dwell that must elapse, with the value continuously outside [lo, hi]
 * the whole time, before the rule activates; 0 = immediate. Deactivation is
 * always immediate on returning to [lo, hi] — dwell only guards against a
 * spurious/transient excursion causing a false activation, and there is no
 * matching reason to delay clearing an alarm once the value has recovered.
 * Returns latched state. */
static bool eval_threshold(uint8_t idx, const struct app_alarm_rule *rule, struct rstate *rt,
			   float value, bool *should_send)
{
	if (!rule->enabled || isnan(value)) {
		if (rt->active) {
			rt->active = false;
			*should_send = true;
			alarm_collect(idx, rule, false, rt->type, false, 0);
		}
		rt->confirm_deadline = 0;
		return false;
	}

	float lo = rule->lo, hi = rule->hi;

	if (rt->active) {
		if (value >= lo && value <= hi) {
			rt->active = false;
			rt->confirm_deadline = 0;
			*should_send = true;
			alarm_collect(idx, rule, false, rt->type, true, rule_scale(rule, value));
		}
		return rt->active;
	}

	uint8_t want_type = ALARM_TYPE_NONE;
	if (value < lo) {
		want_type = ALARM_TYPE_LOW;
	} else if (value > hi) {
		want_type = ALARM_TYPE_HIGH;
	}

	if (want_type == ALARM_TYPE_NONE) {
		/* Back in bounds: any pending dwell was on a false alarm — drop it. */
		rt->confirm_deadline = 0;
		return false;
	}

	int64_t hold_ms = rule_hold_ms(rule);
	if (hold_ms <= 0) {
		rt->active = true;
		rt->type = want_type;
		*should_send = true;
		alarm_collect(idx, rule, true, want_type, true, rule_scale(rule, value));
		return true;
	}

	/* `type` doubles as the pending side while a dwell is armed (struct rstate
	 * comment) — a change of side (e.g. dwelling HIGH, briefly back in bounds,
	 * now LOW) restarts the window rather than reusing a stale deadline. */
	if (rt->confirm_deadline == 0 || rt->type != want_type) {
		rt->type = want_type;
		rt->confirm_deadline = k_uptime_get() + hold_ms;
		return false;
	}

	if (k_uptime_get() >= rt->confirm_deadline) {
		rt->confirm_deadline = 0;
		rt->active = true;
		*should_send = true;
		alarm_collect(idx, rule, true, want_type, true, rule_scale(rule, value));
		return true;
	}

	return false; /* still dwelling */
}

/* Momentary/pulse channels (APP_SENSOR_F_MOMENTARY) only ever assert
 * (app_alarm_event(ch, true)) and never deassert, so a STATE rule's prev->cur
 * edge can be observed at most once. Handle them as a per-pulse one-shot in
 * eval_state(). */
static inline bool rule_is_momentary(const struct app_alarm_rule *rule)
{
	const struct app_sensor_channel *c = app_alarm_rule_channel(rule);

	return c != NULL && (c->flags & APP_SENSOR_F_MOMENTARY);
}

/* STATE rule: from/to over a digital level (#348). from != to is an edge: the
 * raw from->to transition arms a confirm dwell (`dwell` seconds, continuously
 * at to_state) before it fires, then the SAME duration holds the alarm active
 * (auto-clear, no deactivate event) before it can re-arm — one field driving
 * both the pre-fire debounce and the post-fire hold. from == to is a level:
 * active while cur == to, gated by the same confirm dwell before activating;
 * deactivation is immediate once cur != to (see eval_threshold: no reason to
 * delay clearing). `dwell` == 0 keeps the pre-#348 immediate behavior throughout. */
static void eval_state(uint8_t idx, const struct app_alarm_rule *rule, struct rstate *rt, bool cur,
		       bool *should_send)
{
	bool is_edge = (rule->from_state != rule->to_state);
	uint8_t cur_lvl = cur ? 1 : 0;
	int64_t hold_ms = rule_hold_ms(rule);

	if (!rule->enabled) {
		if (rt->active) {
			rt->active = false;
			rt->oneshot_expiry = 0;
			*should_send = true;
			alarm_collect(idx, rule, false, ALARM_TYPE_TRIGGER, true, 0);
		}
		rt->confirm_deadline = 0;
		rt->have_prev_state = true;
		rt->prev_state = cur_lvl;
		return;
	}

	if (rule_is_momentary(rule)) {
		/* Pulse channel: each asserted event is a discrete, already-debounced
		 * trigger (there is nothing to dwell-confirm) — fire immediately, then
		 * hold for `dwell` seconds (re-arm window), suppressed while a previous
		 * one is still latched so we emit at most one alarm per hold window
		 * regardless of event rate. from/to are not meaningful for a source
		 * that never reports a level, so edge (0->1) and level (1->1) behave
		 * alike. */
		/* Expire a lapsed one-shot here, in the event path, not only in the 3 s
		 * app_alarm_poll: otherwise a hold shorter than the poll cadence is
		 * silently clamped to it (a new event within ~3 s of the window
		 * elapsing stays suppressed), so hold < 3 s was ineffective (#267). */
		if (rt->active && rt->oneshot_expiry != 0 && k_uptime_get() >= rt->oneshot_expiry) {
			rt->active = false;
			rt->oneshot_expiry = 0;
		}
		if (cur && !rt->active) {
			rt->active = true;
			rt->oneshot_expiry = k_uptime_get() + hold_ms;
			*should_send = true;
			alarm_collect(idx, rule, true, ALARM_TYPE_TRIGGER, true, cur_lvl);
		}
		rt->have_prev_state = true;
		rt->prev_state = cur_lvl;
		return;
	}

	if (is_edge) {
		bool raw_transition = rt->have_prev_state && rt->prev_state == rule->from_state &&
				      cur_lvl == rule->to_state;

		if (raw_transition && rt->confirm_deadline == 0) {
			if (hold_ms <= 0) {
				rt->active = true;
				rt->oneshot_expiry = k_uptime_get();
				*should_send = true;
				alarm_collect(idx, rule, true, ALARM_TYPE_TRIGGER, true, cur_lvl);
			} else {
				rt->confirm_deadline = k_uptime_get() + hold_ms;
			}
		} else if (rt->confirm_deadline != 0 && cur_lvl != rule->to_state) {
			/* Reverted before the dwell elapsed: not a real transition. */
			rt->confirm_deadline = 0;
		} else if (rt->confirm_deadline != 0 && k_uptime_get() >= rt->confirm_deadline) {
			rt->confirm_deadline = 0;
			rt->active = true;
			rt->oneshot_expiry = k_uptime_get() + hold_ms;
			*should_send = true;
			alarm_collect(idx, rule, true, ALARM_TYPE_TRIGGER, true, cur_lvl);
		}
	} else {
		/* Level: active while cur == to, gated by the same confirm dwell. */
		bool want = (cur_lvl == rule->to_state);

		if (!want) {
			rt->confirm_deadline = 0;
			if (rt->active) {
				rt->active = false;
				*should_send = true;
				alarm_collect(idx, rule, false, ALARM_TYPE_TRIGGER, true, cur_lvl);
			}
		} else if (!rt->active) {
			if (hold_ms <= 0) {
				rt->active = true;
				*should_send = true;
				alarm_collect(idx, rule, true, ALARM_TYPE_TRIGGER, true, cur_lvl);
			} else if (rt->confirm_deadline == 0) {
				rt->confirm_deadline = k_uptime_get() + hold_ms;
			} else if (k_uptime_get() >= rt->confirm_deadline) {
				rt->confirm_deadline = 0;
				rt->active = true;
				*should_send = true;
				alarm_collect(idx, rule, true, ALARM_TYPE_TRIGGER, true, cur_lvl);
			}
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
 * window), re-baselining the counter and window each time. One-shot: fires and
 * then holds for `dwell` seconds (#348, same hold/re-arm role as a momentary
 * source) before it can fire again — 0 re-arms immediately. */
static void eval_count(uint8_t idx, const struct app_alarm_rule *rule, struct rstate *rt,
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

	/* Expire a lapsed one-shot before the firing check, mirroring the momentary/
	 * STATE pattern (eval_state()): otherwise a sustained over-limit condition
	 * across multiple consecutive windows would re-fire every window instead of
	 * once-then-hold. */
	if (rt->active && rt->oneshot_expiry != 0 && now >= rt->oneshot_expiry) {
		rt->active = false;
		rt->oneshot_expiry = 0;
	}

	uint32_t delta = cur - rt->prev_count; /* wraps correctly on uint32 */
	if (rule->hi > 0 && delta >= (uint32_t)rule->hi && !rt->active) {
		rt->active = true;
		rt->oneshot_expiry = now + rule_hold_ms(rule);
		*should_send = true;
		alarm_collect(idx, rule, true, ALARM_TYPE_TRIGGER, true, (int32_t)cur);
	}
	/* Re-baseline for the next window whether or not it fired. */
	rt->prev_count = cur;
	rt->count_window_start = now;
}

/* ---- no-data watchdog (config-driven, independent of the rules) --------- */

/* Motherboard: every `liveness` channel (app_sensor_types.yaml, #430) of an
 * enabled sensor is watched for "stopped reporting" — SHT4x temperature /
 * humidity, pressure and the battery monitor (L-41). Digital channels are not
 * liveness channels — a disconnected line still reads a level, it never goes
 * NaN. Latch k is the k-th liveness channel.
 *
 * 1-Wire slot: latch 0 watches the whole device — no channel at all means it
 * stopped answering (unplugged, cable cut) and raises ONE no_data alarm on
 * channel APP_SENSOR_CH_DEVICE instead of one per channel. Latch 1 + p watches
 * part (chip) p of the slot's type while the device still answers; its alarm
 * names the part's first channel. A part is only watched once it has reported
 * since the slot was armed, so a chip that is not fitted on this probe
 * (e.g. TMP112) never alarms. */
#define NODATA_SLOTS (APP_ALARM_SLOT_MAX + 1)

struct nodata_latch {
	uint32_t nan_since; /* k_uptime_get_32() of the first NaN */
	bool nan;           /* NaN since nan_since */
	bool active;        /* no_data alarm latched */
	bool seen;          /* 1-Wire part: reported since the slot was armed */
	uint8_t channel;    /* channel it reports, for the deactivate edge */
};

/* Flat: the motherboard latches first, then APP_ALARM_NODATA_W1_MAX per slot. */
static struct nodata_latch m_nodata[APP_ALARM_NODATA_MAX];
static uint8_t m_nodata_type[NODATA_SLOTS]; /* type the latches of a slot were armed for */

static struct nodata_latch *nodata_latches(uint8_t slot, int *n)
{
	if (slot == APP_ALARM_SLOT_MB) {
		*n = APP_ALARM_NODATA_MB_MAX;
		return &m_nodata[0];
	}
	*n = APP_ALARM_NODATA_W1_MAX;
	return &m_nodata[APP_ALARM_NODATA_MB_MAX + (slot - 1) * APP_ALARM_NODATA_W1_MAX];
}

static bool m_battery_low_active; /* low-battery watchdog latched (#210) */

/* Sensor-mismatch watchdog (#430), one latch per 1-Wire slot. The expected and
 * detected types are cached so the deactivate edge names what cleared. */
#define MISMATCH_COUNT APP_W1_SLOT_COUNT
static bool m_mismatch_active[MISMATCH_COUNT];
static uint8_t m_mismatch_expected[MISMATCH_COUNT];
static uint8_t m_mismatch_detected[MISMATCH_COUNT];

/* Is 1-Wire slot `s` in mismatch under the current config? */
static bool mismatch_now(int s)
{
#if defined(CONFIG_W1)
	return g_app_config.cap_w1_sensors &&
	       app_w1_slot_get_state(s) == APP_W1_SLOT_STATE_MISMATCH;
#else
	ARG_UNUSED(s);
	return false;
#endif /* defined(CONFIG_W1) */
}

/* Type whose channels are watched in `slot` now, or NONE. */
static uint8_t nodata_slot_type(uint8_t slot)
{
	if (slot == APP_ALARM_SLOT_MB) {
		return APP_SENSOR_TYPE_MOTHERBOARD;
	}
#if defined(CONFIG_W1)
	int s = slot - 1;

	/* H-5: gate on the *configured* (persisted) ROM, not the runtime type. A
	 * probe taught to this slot but absent from the bus has runtime type EMPTY;
	 * keying on that silently dropped it from monitoring. Keying on the
	 * configured ROM keeps it monitored so its absence raises a no_data alarm.
	 * A mismatched slot reports TYPE_SENSOR_MISMATCH instead (#430): one
	 * alarm, not two. */
	if (!g_app_config.cap_w1_sensors || !app_w1_slot_is_configured(s) || m_mismatch_active[s]) {
		return APP_SENSOR_TYPE_NONE;
	}

	uint8_t type = app_w1_slot_get_expected_type(s);

	/* A slot taught before sensorN_type existed: the bound device's type. */
	return type != APP_SENSOR_TYPE_NONE ? type : g_app_sensor_data.w1[s].type;
#else
	return APP_SENSOR_TYPE_NONE;
#endif /* defined(CONFIG_W1) */
}

/* Is liveness channel `c` of an enabled sensor expected to report now? */
static bool nodata_channel_enabled(const struct app_sensor_channel *c)
{
	return c->cap_off == APP_SENSOR_NO_CAP ||
	       *(const bool *)((const char *)&g_app_config + c->cap_off);
}

/* Drop latch `l` of `slot`, emitting the deactivate edge of a latched alarm
 * (M-7) — otherwise the backend keeps a no_data alarm open forever on a sensor
 * the operator deliberately turned off, needing a manual clear. */
static void nodata_reset(uint8_t slot, uint8_t type, struct nodata_latch *l, bool *should_send)
{
	if (l->active) {
		alarm_collect_nodata(slot, l->channel, type, false);
		*should_send = true;
	}
	l->nan = false;
	l->active = false;
	l->seen = false;
}

/* Advance latch `l` of `slot` by one sample; `ok` = its input reported. Fires
 * once the input has been missing for APP_ALARM_NO_DATA_MS. */
static void nodata_step(uint8_t slot, uint8_t type, struct nodata_latch *l, bool ok, uint32_t now,
			bool *should_send)
{
	if (ok) {
		l->nan = false;
		if (l->active) {
			l->active = false;
			alarm_collect_nodata(slot, l->channel, type, false); /* recovered */
			*should_send = true;
		}
		return;
	}

	/* Missing: arm / age the timer; fire once it has been missing long enough. */
	if (!l->nan) {
		l->nan = true;
		l->nan_since = now;
	}
	if (!l->active && (now - l->nan_since) >= APP_ALARM_NO_DATA_MS) {
		l->active = true;
		alarm_collect_nodata(slot, l->channel, type, true);
		*should_send = true;
	}
}

/* Sensor-mismatch watchdog (#430): an ACTIVATE edge when a slot goes into
 * mismatch (rebind at boot or after a scan / teach), a DEACTIVATE edge when it
 * leaves it (the right type is back, the slot is re-taught or cleared, or
 * cap_w1_sensors is turned off). Runs before nodata_poll(), which skips a
 * mismatched slot. Caller holds g_app_sensor_data_lock (the latches share it
 * with the no-data watchdog). */
static void mismatch_poll(bool *should_send)
{
	for (int s = 0; s < MISMATCH_COUNT; s++) {
		bool mm = mismatch_now(s);

		if (mm && !m_mismatch_active[s]) {
#if defined(CONFIG_W1)
			m_mismatch_expected[s] = app_w1_slot_get_expected_type(s);
			m_mismatch_detected[s] = app_w1_slot_get_detected_type(s);
#endif /* defined(CONFIG_W1) */
			m_mismatch_active[s] = true;
			alarm_collect_mismatch(s, m_mismatch_expected[s], m_mismatch_detected[s],
					       true);
			*should_send = true;
		} else if (!mm && m_mismatch_active[s]) {
			m_mismatch_active[s] = false;
			alarm_collect_mismatch(s, m_mismatch_expected[s], m_mismatch_detected[s],
					       false);
			*should_send = true;
		}
	}
}

/* Motherboard: latch k = the k-th liveness channel. */
static void nodata_poll_mb(const struct app_sensor_type *t, struct nodata_latch *latch, int n,
			   uint32_t now, bool *should_send)
{
	int k = 0;

	for (uint8_t ch = 0; ch < t->channel_count; ch++) {
		const struct app_sensor_channel *c = &t->channels[ch];

		if (!(c->flags & APP_SENSOR_F_LIVENESS) || (c->flags & APP_SENSOR_F_RETIRED)) {
			continue;
		}
		if (k >= n) {
			break; /* tests/alarm_eval guards the registry against this */
		}

		struct nodata_latch *l = &latch[k++];

		l->channel = ch;
		if (!nodata_channel_enabled(c)) {
			nodata_reset(APP_ALARM_SLOT_MB, t->id, l, should_send);
			continue;
		}
		nodata_step(APP_ALARM_SLOT_MB, t->id, l,
			    !isnan(read_value(APP_ALARM_SLOT_MB, ch, t->id)), now, should_send);
	}
}

/* 1-Wire slot: latch 0 = the whole device, latch 1 + p = part p. */
static void nodata_poll_w1(uint8_t slot, const struct app_sensor_type *t,
			   struct nodata_latch *latch, int n, uint32_t now, bool *should_send)
{
	uint32_t reported = 0; /* bit ch = channel ch has a value */

	for (uint8_t ch = 0; ch < t->channel_count; ch++) {
		if (!isnan(read_value(slot, ch, t->id))) {
			reported |= BIT(ch);
		}
	}

	latch[0].channel = APP_SENSOR_CH_DEVICE;
	nodata_step(slot, t->id, &latch[0], reported != 0, now, should_send);

	for (uint8_t p = 0; p < t->part_count && 1 + p < n; p++) {
		struct nodata_latch *l = &latch[1 + p];
		uint32_t mask = 0;

		for (uint8_t ch = 0; ch < t->channel_count; ch++) {
			const struct app_sensor_channel *c = &t->channels[ch];

			if (c->part == p && !(c->flags & APP_SENSOR_F_RETIRED)) {
				if (mask == 0) {
					l->channel = ch; /* the part's first channel */
				}
				mask |= BIT(ch);
			}
		}
		if (reported == 0) {
			/* The device alarm covers every part: hold the part latch as is
			 * (no second alarm, no false recovery) until it answers again. */
			l->nan = false;
			continue;
		}
		if (reported & mask) {
			l->seen = true;
		}
		if (l->seen) {
			nodata_step(slot, t->id, l, (reported & mask) != 0, now, should_send);
		}
	}
}

/* Evaluate the no-data watchdog over every liveness channel. Caller holds
 * g_app_sensor_data_lock (read_value reads g_app_sensor_data). */
static void nodata_poll(bool *should_send)
{
	uint32_t now = k_uptime_get_32();

	for (uint8_t slot = 0; slot < NODATA_SLOTS; slot++) {
		uint8_t type = nodata_slot_type(slot);
		int n;
		struct nodata_latch *latch = nodata_latches(slot, &n);

		/* A changed (or dropped) slot type re-arms every latch of the slot;
		 * latched alarms first emit their deactivate edge under the old type. */
		if (type != m_nodata_type[slot]) {
			for (int k = 0; k < n; k++) {
				nodata_reset(slot, m_nodata_type[slot], &latch[k], should_send);
			}
			m_nodata_type[slot] = type;
		}

		const struct app_sensor_type *t = app_sensor_type_get(type);

		if (t == NULL) {
			continue;
		}
		if (slot == APP_ALARM_SLOT_MB) {
			nodata_poll_mb(t, latch, n, now, should_send);
		} else {
			nodata_poll_w1(slot, t, latch, n, now, should_send);
		}
	}
}

static bool nodata_slot_active(uint8_t slot)
{
	int n;
	const struct nodata_latch *latch = nodata_latches(slot, &n);

	for (int k = 0; k < n; k++) {
		if (latch[k].active) {
			return true;
		}
	}
	return false;
}

/* Low-battery watchdog (#210): independent of the rules and the no-data
 * watchdog. Evaluates the motherboard battery-voltage channel (ch 20, a
 * watchdog-only channel, #430) from g_app_sensor_data (caller holds
 * g_app_sensor_data_lock) against the configurable `battery_level` threshold
 * (mV) and fires/clears a low-battery alarm with hysteresis. A latched alarm
 * drives the red LED via app_alarm_poll (it is an alarm). */
static void battery_poll(bool *should_send)
{
	float v = read_value(APP_ALARM_SLOT_MB, APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE,
			     APP_SENSOR_TYPE_MOTHERBOARD);
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

/* #397: per-alarm bit layout of the active mask handed to alarm_buzzer_sync()
 * — rules first, then one no-data bit per slot (any of its liveness channels),
 * then low battery, then one mismatch bit per 1-Wire slot. */
#define ALARM_MASK_NODATA_SHIFT   APP_ALARM_RULE_COUNT
#define ALARM_MASK_BATTERY_SHIFT  (APP_ALARM_RULE_COUNT + NODATA_SLOTS)
#define ALARM_MASK_MISMATCH_SHIFT (ALARM_MASK_BATTERY_SHIFT + 1)
BUILD_ASSERT(ALARM_MASK_MISMATCH_SHIFT + MISMATCH_COUNT <= 32,
	     "alarm active mask no longer fits in uint32_t");

/* Evaluate one armed-or-inert rule against its channel. Caller holds
 * g_app_sensor_data_lock and m_lock. */
static void eval_rule(uint8_t idx, const struct app_alarm_rule *rule, bool *should_send)
{
	const struct app_sensor_channel *c = app_alarm_rule_channel(rule);
	struct rstate *rt = &m_rt[idx];

	if (c == NULL) {
		return;
	}
	switch (c->kind) {
	case APP_SENSOR_KIND_THRESHOLD:
		eval_threshold(idx, rule, rt,
			       read_value(rule->slot, rule->channel, rule->sensor_type),
			       should_send);
		break;
	case APP_SENSOR_KIND_STATE: {
		/* Non-momentary state channels are re-sampled here every poll (#348,
		 * see read_poll_state()) so an armed confirm/dwell deadline gets
		 * resolved even without a fresh edge; momentary ones are event-only.
		 * An inert rule whose level can no longer be read (a stale 1-Wire
		 * rule) still runs eval_state() with the last level so a latched
		 * level alarm deactivates; a momentary one-shot just expires. */
		bool st = rt->prev_state != 0;
		bool drop = !rule->enabled && rt->active && !(c->flags & APP_SENSOR_F_MOMENTARY);

		if (read_poll_state(rule, c, &st) || drop) {
			eval_state(idx, rule, rt, st, should_send);
		}
		break;
	}
	case APP_SENSOR_KIND_RATE: {
		uint32_t cnt;
		if (read_counter(rule, c, &cnt)) {
			eval_count(idx, rule, rt, cnt, should_send);
		}
		break;
	}
	default:
		break;
	}
}

bool app_alarm_poll(void)
{
	bool should_send = false;
	uint32_t active_mask = 0;
	int64_t now = k_uptime_get();

	/* m_rt[] is shared with app_alarm_event() (system WQ / PIR thread); hold
	 * m_lock across the whole evaluation + latch sweep so a concurrent event
	 * can't tear an m_rt entry (#185). Lock order is always
	 * g_app_sensor_data_lock -> m_lock; m_lock is recursive, so the nested
	 * alarm_collect() re-lock inside eval_* is fine. */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	k_mutex_lock(&m_lock, K_FOREVER);

	for (uint8_t idx = 0; idx < APP_ALARM_RULE_COUNT; idx++) {
		struct app_alarm_rule rule;

		if (rt_sync_armed(idx, &rule, &should_send)) {
			eval_rule(idx, &rule, &should_send);
		}
	}

	/* No-data watchdog over every liveness channel (independent of the rules
	 * above). Same lock — it reads g_app_sensor_data too, so it must run before
	 * the sensor-data lock is dropped (#205). The mismatch watchdog runs first:
	 * a mismatched slot suppresses its no-data alarm. */
	mismatch_poll(&should_send);
	nodata_poll(&should_send);

	/* Low-battery watchdog (#210). Reads the battery-voltage channel, so it must
	 * also run before the sensor-data lock is dropped. */
	battery_poll(&should_send);

	/* A latched watchdog alarm also counts as "active", so the main loop lights
	 * the red LED (blinks every BLINK_INTERVAL_SECONDS) until it clears. Read
	 * here under g_app_sensor_data_lock, like the polls (#211). Collected into
	 * the mask (#397) so a NEW watchdog alarm is distinguishable from one
	 * already sounding. */
	for (uint8_t slot = 0; slot < NODATA_SLOTS; slot++) {
		if (nodata_slot_active(slot)) {
			active_mask |= BIT(ALARM_MASK_NODATA_SHIFT + slot);
		}
	}
	if (m_battery_low_active) {
		active_mask |= BIT(ALARM_MASK_BATTERY_SHIFT);
	}
	for (int s = 0; s < MISMATCH_COUNT; s++) {
		if (m_mismatch_active[s]) {
			active_mask |= BIT(ALARM_MASK_MISMATCH_SHIFT + s);
		}
	}

	/* Sensor data no longer needed; keep m_lock for the latch sweep (#185). */
	k_mutex_unlock(&g_app_sensor_data_lock);

	/* Expire one-shot latches and collect "any active". */
	for (int i = 0; i < APP_ALARM_RULE_COUNT; i++) {
		if (!m_rt[i].used) {
			continue;
		}
		if (m_rt[i].oneshot_expiry != 0 && now >= m_rt[i].oneshot_expiry) {
			m_rt[i].active = false;
			m_rt[i].oneshot_expiry = 0;
		}
		if (m_rt[i].active) {
			active_mask |= BIT(i);
		}
	}

	/* #397: central activation-edge detection over the finished mask, under
	 * the same m_lock the readers (app_alarm_active_mask/activation_seq)
	 * take — a new bit bumps the sequence exactly once per poll. */
	uint32_t new_bits = active_mask & ~m_active_mask;
	bool all_cleared = active_mask == 0 && m_active_mask != 0;

	m_active_mask = active_mask;
	if (new_bits != 0) {
		m_activation_seq++;
	}
	k_mutex_unlock(&m_lock);

	if (should_send) {
		alarm_send();
	}

	alarm_buzzer_sync(new_bits, all_cleared);

	return active_mask != 0;
}

uint32_t app_alarm_active_mask(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	uint32_t mask = m_active_mask;
	k_mutex_unlock(&m_lock);
	return mask;
}

uint32_t app_alarm_activation_seq(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	uint32_t seq = m_activation_seq;
	k_mutex_unlock(&m_lock);
	return seq;
}

void app_alarm_event(uint8_t channel, bool active)
{
	/* Discrete edge → every STATE rule on this motherboard channel (several
	 * rules may target it, e.g. an edge rule and a level rule). */
	app_alarm_event_cb cb;
	void *user_data;
	bool should_send = false;

	/* Evaluate under m_lock so m_rt[] is not torn against app_alarm_poll() or
	 * another event on a different thread (#185). m_lock is recursive: the
	 * nested alarm_collect() re-lock is fine. cb is read under the same lock;
	 * should_send / cb are acted on after release so the radio enqueue and
	 * callback never run under m_lock. */
	k_mutex_lock(&m_lock, K_FOREVER);
	cb = m_event_cb;
	user_data = m_event_cb_user_data;

	for (uint8_t idx = 0; idx < APP_ALARM_RULE_COUNT; idx++) {
		struct app_alarm_rule rule;

		if (!rt_sync_armed(idx, &rule, &should_send)) {
			continue;
		}

		const struct app_sensor_channel *c = app_alarm_rule_channel(&rule);

		if (rule.slot != APP_ALARM_SLOT_MB || rule.channel != channel || c == NULL ||
		    c->kind != APP_SENSOR_KIND_STATE) {
			continue;
		}
		eval_state(idx, &rule, &m_rt[idx], active, &should_send);
	}
	k_mutex_unlock(&m_lock);

	if (should_send) {
		alarm_send();
	}

	if (cb) {
		cb(channel, active, user_data);
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

/* Kind of the rule latched in m_rt[i] (APP_SENSOR_KIND_NONE if unknown). */
static uint8_t rt_kind(int i)
{
	const struct app_sensor_channel *c = app_alarm_rule_channel(&m_rt[i].last_rule);

	return c != NULL ? c->kind : APP_SENSOR_KIND_NONE;
}

uint32_t app_alarm_status_flags(void)
{
	uint32_t flags = 0;

	/* Watchdog latches are written under g_app_sensor_data_lock in poll();
	 * take it first to keep the same lock order (data_lock -> m_lock) and avoid
	 * inversion. Read-only here: no evaluation, no one-shot expiry, no send. */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	for (uint8_t slot = 0; slot < NODATA_SLOTS; slot++) {
		if (nodata_slot_active(slot)) {
			flags |= APP_DEVICE_STATUS_ALARM_NO_DATA | APP_DEVICE_STATUS_ALARM_ANY;
			break;
		}
	}
	if (m_battery_low_active) {
		flags |= APP_DEVICE_STATUS_ALARM_LOW_BATT | APP_DEVICE_STATUS_ALARM_ANY;
	}
	for (int s = 0; s < MISMATCH_COUNT; s++) {
		if (m_mismatch_active[s]) {
			flags |= APP_DEVICE_STATUS_ALARM_SENSOR_MISMATCH |
				 APP_DEVICE_STATUS_ALARM_ANY;
			break;
		}
	}
	k_mutex_unlock(&g_app_sensor_data_lock);

	k_mutex_lock(&m_lock, K_FOREVER);
	for (int i = 0; i < APP_ALARM_RULE_COUNT; i++) {
		if (!m_rt[i].used || !m_rt[i].active) {
			continue;
		}
		flags |= APP_DEVICE_STATUS_ALARM_ANY;
		switch (rt_kind(i)) {
		case APP_SENSOR_KIND_THRESHOLD:
			flags |= APP_DEVICE_STATUS_ALARM_THRESHOLD;
			break;
		case APP_SENSOR_KIND_STATE:
			flags |= APP_DEVICE_STATUS_ALARM_STATE;
			break;
		case APP_SENSOR_KIND_RATE:
			flags |= APP_DEVICE_STATUS_ALARM_RATE;
			break;
		default:
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

	/* Watchdogs read g_app_sensor_data-backed latches; take that lock first,
	 * like app_alarm_status_flags() (keeps lock order data_lock -> m_lock and
	 * avoids holding both). */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	for (uint8_t slot = 0; slot < NODATA_SLOTS; slot++) {
		int cnt;
		const struct nodata_latch *latch = nodata_latches(slot, &cnt);

		for (int k = 0; k < cnt && n < max; k++) {
			if (!latch[k].active) {
				continue;
			}
			out[n++] = (struct app_alarm_active){
				.slot = slot,
				.channel = latch[k].channel,
				.sensor_type = m_nodata_type[slot],
				.type = ALARM_TYPE_NO_DATA,
			};
		}
	}
	if (m_battery_low_active && n < max) {
		out[n++] = (struct app_alarm_active){
			.slot = APP_ALARM_SLOT_MB,
			.channel = APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE,
			.sensor_type = APP_SENSOR_TYPE_MOTHERBOARD,
			.type = ALARM_TYPE_LOW,
		};
	}
	for (int s = 0; s < MISMATCH_COUNT && n < max; s++) {
		if (!m_mismatch_active[s]) {
			continue;
		}
		out[n++] = (struct app_alarm_active){
			.slot = s + 1,
			.channel = 0,
			.sensor_type = m_mismatch_expected[s],
			.type = ALARM_TYPE_SENSOR_MISMATCH,
		};
	}
	k_mutex_unlock(&g_app_sensor_data_lock);

	/* Rules. type: threshold rules latch LOW/HIGH in rt->type; state/count
	 * rules report TRIGGER. */
	k_mutex_lock(&m_lock, K_FOREVER);
	for (int i = 0; i < APP_ALARM_RULE_COUNT && n < max; i++) {
		if (!m_rt[i].used || !m_rt[i].active) {
			continue;
		}

		const struct app_alarm_rule *r = &m_rt[i].last_rule;

		out[n++] = (struct app_alarm_active){
			.slot = r->slot,
			.channel = r->channel,
			.sensor_type = r->sensor_type,
			.type = rt_kind(i) == APP_SENSOR_KIND_THRESHOLD ? m_rt[i].type
									: ALARM_TYPE_TRIGGER,
		};
	}
	k_mutex_unlock(&m_lock);

	return n;
}

/* ---- shell -------------------------------------------------------------- */

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#include <stdlib.h>

static const char *const m_kind_names[] = {
	[APP_SENSOR_KIND_THRESHOLD] = "threshold",
	[APP_SENSOR_KIND_STATE] = "state",
	[APP_SENSOR_KIND_RATE] = "rate",
	[APP_SENSOR_KIND_NONE] = "none",
};

/* Per-rule latch state, for the shell list. */
static bool rule_active(uint8_t idx)
{
	bool a;
	k_mutex_lock(&m_lock, K_FOREVER);
	a = idx < APP_ALARM_RULE_COUNT && m_rt[idx].used && m_rt[idx].active;
	k_mutex_unlock(&m_lock);
	return a;
}

/* Print one occupied rule (caller has checked it is occupied). */
static void print_rule(const struct shell *sh, uint8_t idx, const struct app_alarm_rule *r)
{
	const struct app_sensor_channel *c = app_alarm_rule_channel(r);
	const struct app_sensor_type *t = app_sensor_type_get(r->sensor_type);
	const char *slot = app_alarm_slot_name(r->slot);
	const char *ch = c != NULL ? c->name : "?";
	/* A rule written for another type than the slot has now is inert (#430). */
	const char *stale = app_alarm_rule_stale(r) ? "  STALE" : "";
	bool active = rule_active(idx);

	if (c == NULL) {
		return;
	}
	switch (c->kind) {
	case APP_SENSOR_KIND_THRESHOLD:
		shell_print(sh,
			    "  [%u] %s %s (%s)  lo=%s%d.%02d hi=%s%d.%02d dwell=%s%d.%02d  en=%d "
			    "active=%d%s",
			    idx, slot, ch, t->name, APP_FP2(r->lo), APP_FP2(r->hi),
			    APP_FP2(r->dwell), r->enabled, active, stale);
		break;
	case APP_SENSOR_KIND_STATE:
		shell_print(sh, "  [%u] %s %s (%s)  %u->%u (%s) dwell=%s%d.%02d  en=%d active=%d%s",
			    idx, slot, ch, t->name, r->from_state, r->to_state,
			    r->from_state == r->to_state ? "level" : "edge", APP_FP2(r->dwell),
			    r->enabled, active, stale);
		break;
	case APP_SENSOR_KIND_RATE:
		shell_print(sh,
			    "  [%u] %s %s (%s)  rate>=%d/interval dwell=%s%d.%02d  en=%d "
			    "active=%d%s",
			    idx, slot, ch, t->name, (int)r->hi, APP_FP2(r->dwell), r->enabled,
			    active, stale);
		break;
	default:
		break;
	}
}

static int cmd_alarm_list(const struct shell *sh, size_t argc, char **argv)
{
	/* Optional <rule>: list just that one rule. */
	if (argc >= 2) {
		char *end;
		unsigned long idx = strtoul(argv[1], &end, 10);
		if (*end != '\0' || idx >= APP_ALARM_RULE_COUNT) {
			shell_error(sh, "invalid <rule> (0..%d)", APP_ALARM_RULE_COUNT - 1);
			return -EINVAL;
		}
		struct app_alarm_rule r;
		if (!app_alarm_rules_get((uint8_t)idx, &r)) {
			shell_print(sh, "rule %lu empty", idx);
			return 0;
		}
		print_rule(sh, (uint8_t)idx, &r);
		return 0;
	}

	shell_print(sh, "%u rule(s):", app_alarm_rules_count());
	for (uint8_t idx = 0; idx < APP_ALARM_RULE_COUNT; idx++) {
		struct app_alarm_rule r;
		if (app_alarm_rules_get(idx, &r)) {
			print_rule(sh, idx, &r);
		}
	}
	return 0;
}

/* Staged type of `slot` (shell writes go to the staging config, so a type set
 * with `config sensorN-type` before `settings save` counts). */
static uint8_t staged_slot_type(uint8_t slot)
{
	const struct app_config *c = app_config();
	const uint8_t types[] = {APP_SENSOR_TYPE_MOTHERBOARD, c->sensor1_type, c->sensor2_type,
				 c->sensor3_type, c->sensor4_type};

	return slot < ARRAY_SIZE(types) ? types[slot] : APP_SENSOR_TYPE_NONE;
}

/* Parse `<slot> <channel> <key> <value> …` starting at argv[base] into `r`
 * (enabled). Keys: lo, hi, dwell (threshold), from, to, dwell (state), hi,
 * dwell (rate). The sensor type is the slot's current (staged) type. Shared by
 * `alarm set` (base=2, after the rule index) and `alarm new` (base=1). Prints a
 * usage/validity error and returns -EINVAL on bad input. */
static int parse_rule_spec(const struct shell *sh, size_t argc, char **argv, size_t base,
			   struct app_alarm_rule *r)
{
	char *end;
	int slot = app_alarm_slot_by_name(argv[base]);

	if (slot < 0) {
		unsigned long v = strtoul(argv[base], &end, 10);

		slot = (*end == '\0' && v <= APP_ALARM_SLOT_MAX) ? (int)v : -1;
	}
	if (slot < 0) {
		shell_error(sh, "invalid <slot> (mb, s1..s4)");
		return -EINVAL;
	}

	uint8_t type = staged_slot_type((uint8_t)slot);

	if (type == APP_SENSOR_TYPE_NONE) {
		shell_error(sh, "slot %s has no type: set sensor%d-type first", argv[base], slot);
		return -EINVAL;
	}

	int ch = app_sensor_channel_by_name(type, argv[base + 1]);

	if (ch < 0) {
		unsigned long v = strtoul(argv[base + 1], &end, 10);

		ch = (*end == '\0' && v <= UINT8_MAX) ? (int)v : -1;
	}
	if (ch < 0 || !app_alarm_rule_valid((uint8_t)slot, (uint8_t)ch, type)) {
		shell_error(sh, "invalid <channel> for %s (see 'sensor types %s')",
			    app_sensor_type_get(type)->name, app_sensor_type_get(type)->name);
		return -EINVAL;
	}

	*r = (struct app_alarm_rule){
		.slot = (uint8_t)slot, .channel = (uint8_t)ch, .sensor_type = type, .enabled = 1};

	const struct app_sensor_channel *c = app_alarm_rule_channel(r);
	bool have_lo = false, have_hi = false, have_from = false, have_to = false;

	for (size_t a = base + 2; a < argc; a += 2) {
		const char *key = argv[a];

		if (a + 1 >= argc) {
			shell_error(sh, "missing value for '%s'", key);
			return -EINVAL;
		}

		float v = strtof(argv[a + 1], &end);

		if (*end != '\0') {
			shell_error(sh, "invalid value for '%s': %s", key, argv[a + 1]);
			return -EINVAL;
		}
		if (strcmp(key, "lo") == 0 && c->kind == APP_SENSOR_KIND_THRESHOLD) {
			r->lo = v;
			have_lo = true;
		} else if (strcmp(key, "hi") == 0 && c->kind != APP_SENSOR_KIND_STATE) {
			r->hi = v;
			have_hi = true;
		} else if (strcmp(key, "from") == 0 && c->kind == APP_SENSOR_KIND_STATE) {
			r->from_state = v != 0.0f ? 1 : 0;
			have_from = true;
		} else if (strcmp(key, "to") == 0 && c->kind == APP_SENSOR_KIND_STATE) {
			r->to_state = v != 0.0f ? 1 : 0;
			have_to = true;
		} else if (strcmp(key, "dwell") == 0) {
			r->dwell = v;
		} else {
			shell_error(sh, "'%s' does not apply to a %s channel", key,
				    m_kind_names[c->kind]);
			return -EINVAL;
		}
	}

	switch (c->kind) {
	case APP_SENSOR_KIND_THRESHOLD:
		if (!have_lo || !have_hi) {
			shell_error(sh,
				    "threshold args: lo <lo> hi <hi> [dwell <s>]  (dwell = "
				    "seconds outside the band before activating, 0 = immediate)");
			return -EINVAL;
		}
		break;
	case APP_SENSOR_KIND_STATE:
		if (!have_from || !have_to) {
			shell_error(sh, "state args: from <0|1> to <0|1> [dwell <s>]  "
					"(from!=to=edge, from==to=level; dwell = confirm+hold "
					"seconds, 0 = immediate)");
			return -EINVAL;
		}
		break;
	default:
		if (!have_hi) {
			shell_error(sh, "rate args: hi <N-per-interval> [dwell <s>]");
			return -EINVAL;
		}
		break;
	}
	return 0;
}

/* Store `r` at `idx`, persist, and report. Shared by set / new. */
static int store_rule(const struct shell *sh, uint8_t idx, const struct app_alarm_rule *r)
{
	int ret = app_alarm_rules_set(idx, r);
	if (ret) {
		shell_error(sh,
			    "set failed: %d (band hi > lo, dwell 0..3600, PIR/accel motion "
			    "edge only)",
			    ret);
		return ret;
	}
	ret = app_alarm_rules_save();
	shell_print(sh, "rule %u set%s", idx, ret ? " (NOT persisted!)" : "");
	return 0;
}

static int cmd_alarm_set(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	unsigned long idx = strtoul(argv[1], &end, 10);
	if (*end != '\0' || idx >= APP_ALARM_RULE_COUNT) {
		shell_error(sh, "invalid <rule> (0..%d)", APP_ALARM_RULE_COUNT - 1);
		return -EINVAL;
	}

	struct app_alarm_rule r;
	int ret = parse_rule_spec(sh, argc, argv, 2, &r);
	if (ret) {
		return ret;
	}
	return store_rule(sh, (uint8_t)idx, &r);
}

static int cmd_alarm_new(const struct shell *sh, size_t argc, char **argv)
{
	struct app_alarm_rule r;
	int ret = parse_rule_spec(sh, argc, argv, 1, &r);
	if (ret) {
		return ret;
	}
	int idx = app_alarm_rules_first_free();
	if (idx < 0) {
		shell_error(sh, "no free rule (all %d in use)", APP_ALARM_RULE_COUNT);
		return -ENOSPC;
	}
	return store_rule(sh, (uint8_t)idx, &r);
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
		return 0;
	}
	char *end;
	unsigned long idx = strtoul(argv[1], &end, 10);
	if (*end != '\0' || idx >= APP_ALARM_RULE_COUNT) {
		shell_error(sh, "usage: alarm clear <rule>|all");
		return -EINVAL;
	}
	int ret = app_alarm_rules_clear((uint8_t)idx);
	if (ret) {
		shell_error(sh, "clear failed: %d%s", ret, ret == -ENOENT ? " (rule empty)" : "");
		return ret;
	}
	(void)app_alarm_rules_save();
	shell_print(sh, "rule %lu cleared", idx);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_alarm,
	SHELL_CMD_ARG(list, NULL, "List alarm rules, or one rule. Usage: list [<rule>]",
		      cmd_alarm_list, 1, 1),
	SHELL_CMD_ARG(set, NULL,
		      "Set rule <rule>. Usage: set <rule> <slot> <channel> <key> <value>...\n"
		      "  slot: mb | s1..s4; channel: name or number (see 'sensor types')\n"
		      "  threshold: lo <lo> hi <hi> [dwell <s>]\n"
		      "  state: from <0|1> to <0|1> [dwell <s>]   rate: hi <N> [dwell <s>]\n"
		      "  e.g. set 0 mb temperature lo 2 hi 8 dwell 60",
		      cmd_alarm_set, 6, 4),
	SHELL_CMD_ARG(new, NULL,
		      "Set a rule in the first free index. Usage: new <slot> <channel> <key> "
		      "<value>...\n"
		      "  keys as in 'alarm set'. dwell: threshold = seconds outside [lo,hi]\n"
		      "  before activating; state edge = confirm dwell before firing AND hold\n"
		      "  before re-arm; state level = dwell before activating (deactivate is\n"
		      "  immediate); pir/accel motion (momentary) = hold before re-arm, no\n"
		      "  confirm; rate = hold before re-arm. 0 = immediate/no hold throughout.",
		      cmd_alarm_new, 5, 4),
	SHELL_CMD_ARG(clear, NULL, "Clear a rule (or all). Usage: clear <rule>|all",
		      cmd_alarm_clear, 1, 1),
	SHELL_CMD_ARG(poll, NULL, "Sample + evaluate rules now (bench test).", cmd_alarm_poll, 1,
		      0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(alarm, &sub_alarm, "Dynamic alarm rules.", NULL);

/* `sensor types [<type>]`: the registry channel tables (#430), the channel
 * names / numbers `alarm set` takes. */
static void print_type(const struct shell *sh, const struct app_sensor_type *t)
{
	shell_print(sh, "type %u %s:", t->id, t->name);
	for (uint8_t ch = 0; ch < t->channel_count; ch++) {
		const struct app_sensor_channel *c = &t->channels[ch];

		if (c->flags & APP_SENSOR_F_RETIRED) {
			continue;
		}
		shell_print(sh, "  %2u %-18s %-9s%s%s", ch, c->name, m_kind_names[c->kind],
			    (c->flags & APP_SENSOR_F_MOMENTARY) ? " momentary" : "",
			    (c->flags & APP_SENSOR_F_WATCHDOG_ONLY) ? " watchdog-only" : "");
	}
}

static int cmd_sensor_types(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2) {
		const struct app_sensor_type *t = app_sensor_type_by_name(argv[1]);

		if (t == NULL) {
			t = app_sensor_type_get((uint8_t)strtoul(argv[1], NULL, 10));
		}
		if (t == NULL) {
			shell_error(sh, "unknown type: %s", argv[1]);
			return -EINVAL;
		}
		print_type(sh, t);
		return 0;
	}
	for (uint8_t id = 1; id != 0; id++) {
		const struct app_sensor_type *t = app_sensor_type_get(id);

		if (t == NULL) {
			break;
		}
		print_type(sh, t);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sensor,
			       SHELL_CMD_ARG(types, NULL,
					     "List sensor types and their channels. Usage: types "
					     "[<type>]",
					     cmd_sensor_types, 1, 1),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sensor, &sub_sensor, "Sensor type registry.", NULL);
#endif /* defined(CONFIG_SHELL) */

static int app_alarm_init(void)
{
	k_work_init_delayable(&m_alarm_batch_work, alarm_batch_work_handler);
	return 0;
}

SYS_INIT(app_alarm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
