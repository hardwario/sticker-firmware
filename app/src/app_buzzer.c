/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_buzzer.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>

LOG_MODULE_REGISTER(app_buzzer, LOG_LEVEL_INF);

/* HW variant: a CMI-9605IC-0380T self-driven magnetic buzzer is wired directly
 * across the two GPIO pins normally reserved for the PIR sensor
 * (pir-si-gpios/pir-dl-gpios) instead of a real PIR (#338). Mutually exclusive
 * with app_pyq1648 — caller must only init this when cap_pir_detector is off. */
static const struct gpio_dt_spec m_a_spec = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pir_si_gpios);
static const struct gpio_dt_spec m_b_spec = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pir_dl_gpios);

#define REQUEST_MAX_AGE_MS   2000
#define REQUEST_MIN_DELAY_MS 500

/* The debug build (RTT log + shell, CONFIG_PM=n) runs close to its RAM budget
 * even without the buzzer, so this thread's stack is kept modest — well above
 * the 256 B the equivalent bench-test thread (gpio writes + k_sleep +
 * occasional LOG_ERR, no msgq/switch) ran fine at, for margin around the extra
 * k_msgq_get()/switch this version adds. */
#define BUZZER_THREAD_STACK_SIZE 512
#define BUZZER_THREAD_PRIORITY   K_PRIO_PREEMPT(5)

/* Sentinel `kind` for a bounded on(ms) request (app_buzzer_on) — distinct from
 * the #338 wire melody ids below (0-15, of which 1-3 are assigned), so it can
 * share the same struct buzzer_request/enqueue_request path without colliding
 * with a real melody id. */
#define BUZZER_KIND_ON 0xffu

/* Internal on/off timing DSL, modeled on app_led's {SET, DELAY, END} shape (minus
 * the channel field — there's only one buzzer). Deliberately not exposed via
 * app_buzzer.h: callers outside this file may only trigger one of the fixed
 * melodies (by numeric id, see MELODY_TABLE below) or a bounded app_buzzer_on(),
 * never a custom sequence. */
enum buzzer_cmd_type {
	BUZZER_CMD_END = -1,
	BUZZER_CMD_SET = 0,
	BUZZER_CMD_DELAY = 1,
};

struct buzzer_cmd {
	enum buzzer_cmd_type type;
	union {
		bool on;
		int duration;
	};
};

/* Fixed severity-scaled melodies (analogous to the LED severity scheme, #278):
 * info = single beep, warning = double beep, alarm = fast repeated beep. These
 * live in flash (`static const`), not RAM — only a 1-byte selector travels
 * through the queue below. */
static const struct buzzer_cmd PATTERN_INFO[] = {
	{.type = BUZZER_CMD_SET, .on = true},
	{.type = BUZZER_CMD_DELAY, .duration = 80},
	{.type = BUZZER_CMD_END},
};

static const struct buzzer_cmd PATTERN_WARNING[] = {
	{.type = BUZZER_CMD_SET, .on = true},
	{.type = BUZZER_CMD_DELAY, .duration = 80},
	{.type = BUZZER_CMD_SET, .on = false},
	{.type = BUZZER_CMD_DELAY, .duration = 150},
	{.type = BUZZER_CMD_SET, .on = true},
	{.type = BUZZER_CMD_DELAY, .duration = 80},
	{.type = BUZZER_CMD_END},
};

static const struct buzzer_cmd PATTERN_ALARM[] = {
	{.type = BUZZER_CMD_SET, .on = true},
	{.type = BUZZER_CMD_DELAY, .duration = 100},
	{.type = BUZZER_CMD_SET, .on = false},
	{.type = BUZZER_CMD_DELAY, .duration = 80},
	{.type = BUZZER_CMD_END},
};

struct buzzer_melody {
	const struct buzzer_cmd *commands;
	int repetitions;
};

/* #338 wire melody ids: 0 = no-op ("play nothing" — also proto3's default for
 * an omitted field, so silence-by-default is never an error); 1-3 assigned
 * below; 4-15 reserved. This table is the ONLY place that needs to change to
 * add a new melody — app_config.proto/app_cmd.c pass the raw id straight
 * through and never validate it; app_buzzer_play() here is what decides
 * whether an id exists. */
static const struct buzzer_melody MELODY_TABLE[] = {
	[1] = {.commands = PATTERN_INFO, .repetitions = 1},
	[2] = {.commands = PATTERN_WARNING, .repetitions = 1},
	[3] = {.commands = PATTERN_ALARM, .repetitions = 5},
};

/* Queue message: a melody id (see MELODY_TABLE), or (kind == BUZZER_KIND_ON) a
 * caller-supplied bounded on-duration — never a full command array. A 32-bit
 * timestamp (wraps after ~49 days) is plenty for the REQUEST_MAX_AGE_MS check.
 * repeat_s (0-999, #338): 0 = play once; otherwise the thread replays this same
 * request that many seconds after each playback finishes, indefinitely, until
 * app_buzzer_set(false) aborts it or a new request supersedes it. */
struct buzzer_request {
	uint32_t timestamp;
	uint8_t kind;
	uint32_t on_ms;
	uint16_t repeat_s;
};

K_MSGQ_DEFINE(m_buzzer_msgq, sizeof(struct buzzer_request), 1, 4);
static K_THREAD_STACK_DEFINE(m_buzzer_thread_stack, BUZZER_THREAD_STACK_SIZE);

static k_tid_t m_buzzer_thread_id;

/* Set by app_buzzer_set(false) so an in-flight pattern on the playback thread
 * bails out immediately instead of sleeping out its remaining commands —
 * paired with k_wakeup() to cut a pattern short mid-k_sleep(), not just
 * between commands. Consumed (atomic_clear) ONLY by the thread loop — see
 * thread_entry for why execute_request must not clear it. */
static atomic_t m_abort;

static void set(bool on)
{
	int ret;

	ret = gpio_pin_set_dt(&m_a_spec, on ? 1 : 0);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_set_dt", ret);
	}

	ret = gpio_pin_set_dt(&m_b_spec, 0);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_set_dt", ret);
	}
}

int app_buzzer_set(bool on)
{
	if (!m_buzzer_thread_id) {
		return -EAGAIN;
	}

	if (!on) {
		atomic_set(&m_abort, 1);
		k_wakeup(m_buzzer_thread_id);
	}

	set(on);

	return 0;
}

static void execute_pattern(const struct buzzer_cmd *commands, int repetitions)
{
	int length = 0;
	for (int i = 0; commands[i].type != BUZZER_CMD_END; i++) {
		length = i + 1;
	}

	for (int rep = 0; rep < repetitions; rep++) {
		for (int i = 0; i < length; i++) {
			if (atomic_get(&m_abort)) {
				set(false);
				return;
			}

			const struct buzzer_cmd *cmd = &commands[i];

			/* Skip last delay on last repetition */
			if (rep == repetitions - 1 && i == length - 1 &&
			    cmd->type == BUZZER_CMD_DELAY) {
				break;
			}

			switch (cmd->type) {
			case BUZZER_CMD_SET:
				set(cmd->on);
				break;
			case BUZZER_CMD_DELAY:
				if (cmd->duration > 0) {
					k_sleep(K_MSEC(cmd->duration));
				}
				break;
			default:
				LOG_ERR("Invalid command type: %d", cmd->type);
				break;
			}
		}
	}

	set(false);
}

static void execute_request(const struct buzzer_request *request)
{
	if (request->kind == BUZZER_KIND_ON) {
		const struct buzzer_cmd commands[] = {
			{.type = BUZZER_CMD_SET, .on = true},
			{.type = BUZZER_CMD_DELAY, .duration = (int)request->on_ms},
			{.type = BUZZER_CMD_END},
		};
		execute_pattern(commands, 1);
		return;
	}

	/* kind 0 ("nothing") or an unmapped id never reaches here — app_buzzer_play()
	 * rejects those before enqueueing; this is defensive, not load-bearing. */
	if (request->kind >= ARRAY_SIZE(MELODY_TABLE) || !MELODY_TABLE[request->kind].commands) {
		return;
	}

	const struct buzzer_melody *melody = &MELODY_TABLE[request->kind];
	execute_pattern(melody->commands, melody->repetitions);
}

static void thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct buzzer_request active = {0};
	bool repeating = false;

	for (;;) {
		struct buzzer_request incoming;
		k_timeout_t wait = repeating ? K_SECONDS(active.repeat_s) : K_FOREVER;
		int ret = k_msgq_get(&m_buzzer_msgq, &incoming, wait);

		/* Consume a pending abort (app_buzzer_set(false)): it cancels the
		 * repeat cycle, but a freshly fetched request supersedes it (the two
		 * can only race within one queue hop — newest action wins). The flag
		 * MUST be consumed here with atomic_clear, never merely read and
		 * never cleared in execute_request: k_wakeup() only interrupts
		 * k_sleep(), not a msgq pend, so an off landing while this thread
		 * is blocked above leaves the flag set — if it were not consumed on
		 * the way through this loop, it would poison every later request. */
		if (atomic_clear(&m_abort)) {
			repeating = false;
			if (ret != 0) {
				continue;
			}
		}

		if (ret == 0) {
			if (k_uptime_get_32() - incoming.timestamp > REQUEST_MAX_AGE_MS) {
				LOG_WRN("Discarding stale buzzer request");
				continue;
			}
			active = incoming;
		} else if (!repeating) {
			/* Defensive: a non-timeout error with no repeat pending. */
			continue;
		}
		/* else: the repeat_s wait elapsed naturally — replay `active` as-is. */

		execute_request(&active);
		repeating = !atomic_get(&m_abort) && active.repeat_s > 0;

		k_sleep(K_MSEC(REQUEST_MIN_DELAY_MS));
	}
}

static int enqueue_request(uint8_t kind, uint32_t on_ms, uint16_t repeat_s)
{
	if (!m_buzzer_thread_id) {
		return -EAGAIN;
	}

	struct buzzer_request request = {
		.timestamp = k_uptime_get_32(), .kind = kind, .on_ms = on_ms, .repeat_s = repeat_s};

	int ret = k_msgq_put(&m_buzzer_msgq, &request, K_NO_WAIT);
	if (ret == -ENOMSG) {
		/* Queue is full (depth 1) — the newest request replaces a
		 * not-yet-started one rather than being dropped. */
		struct buzzer_request discard;
		k_msgq_get(&m_buzzer_msgq, &discard, K_NO_WAIT);
		ret = k_msgq_put(&m_buzzer_msgq, &request, K_NO_WAIT);
	}
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("k_msgq_put", ret);
		return ret;
	}

	/* No explicit wakeup needed: k_msgq_put() already wakes a thread blocked
	 * in k_msgq_get() regardless of its timeout — whether it's idle
	 * (K_FOREVER) or waiting out a repeat interval (K_SECONDS(repeat_s)), the
	 * new request is picked up immediately either way. */

	return 0;
}

int app_buzzer_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&m_a_spec)) {
		LOG_ERR("GPIO port is not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&m_b_spec)) {
		LOG_ERR("GPIO port is not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&m_a_spec, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&m_b_spec, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	static struct k_thread thread;
	m_buzzer_thread_id = k_thread_create(
		&thread, m_buzzer_thread_stack, K_THREAD_STACK_SIZEOF(m_buzzer_thread_stack),
		thread_entry, NULL, NULL, NULL, BUZZER_THREAD_PRIORITY, 0, K_NO_WAIT);

	k_thread_name_set(m_buzzer_thread_id, "buzzer");

	return 0;
}

int app_buzzer_on(uint32_t duration_ms)
{
	if (duration_ms == 0) {
		return -EINVAL;
	}

	return enqueue_request(BUZZER_KIND_ON, duration_ms, 0);
}

int app_buzzer_play_repeating(uint32_t kind, uint16_t repeat_s)
{
	if (!m_buzzer_thread_id) {
		return -EAGAIN;
	}

	/* Wire ids are a nibble (0-15); anything wider collapses to 0. */
	if (kind >= 16) {
		kind = 0;
	}

	if (kind == 0) {
		/* 0 = "silence": the remote stop. Kill any queued request plus the
		 * in-flight playback and pending repeat cycle — release builds have
		 * no shell, so this is the only way to stop a repeating melody over
		 * NFC/LRW. repeat_s is ignored here. */
		k_msgq_purge(&m_buzzer_msgq);
		return app_buzzer_set(false);
	}

	if (kind >= ARRAY_SIZE(MELODY_TABLE) || !MELODY_TABLE[kind].commands) {
		return -ENOENT;
	}

	return enqueue_request((uint8_t)kind, 0, repeat_s);
}

int app_buzzer_play(uint32_t kind)
{
	return app_buzzer_play_repeating(kind, 0);
}
