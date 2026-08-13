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

/* Internal on/off timing DSL, modeled on app_led's {SET, DELAY, END} shape (minus
 * the channel field — there's only one buzzer). Deliberately not exposed via
 * app_buzzer.h: callers outside this file may only trigger one of the fixed
 * app_buzzer_play_*() melodies or a bounded app_buzzer_on(), never a custom
 * sequence. */
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

enum buzzer_request_kind {
	BUZZER_REQUEST_ON = 0,
	BUZZER_REQUEST_INFO = 1,
	BUZZER_REQUEST_WARNING = 2,
	BUZZER_REQUEST_ALARM = 3,
};

struct buzzer_pattern_table_entry {
	const struct buzzer_cmd *commands;
	int repetitions;
};

static const struct buzzer_pattern_table_entry PATTERN_TABLE[] = {
	[BUZZER_REQUEST_INFO] = {.commands = PATTERN_INFO, .repetitions = 1},
	[BUZZER_REQUEST_WARNING] = {.commands = PATTERN_WARNING, .repetitions = 1},
	[BUZZER_REQUEST_ALARM] = {.commands = PATTERN_ALARM, .repetitions = 5},
};

/* Queue message: a fixed-melody selector, or (kind == BUZZER_REQUEST_ON) a
 * caller-supplied bounded on-duration — never a full command array. A 32-bit
 * timestamp (wraps after ~49 days) is plenty for the REQUEST_MAX_AGE_MS check. */
struct buzzer_request {
	uint32_t timestamp;
	enum buzzer_request_kind kind;
	uint32_t on_ms;
};

K_MSGQ_DEFINE(m_buzzer_msgq, sizeof(struct buzzer_request), 1, 4);
static K_THREAD_STACK_DEFINE(m_buzzer_thread_stack, BUZZER_THREAD_STACK_SIZE);

static k_tid_t m_buzzer_thread_id;

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
	if (request->kind == BUZZER_REQUEST_ON) {
		const struct buzzer_cmd commands[] = {
			{.type = BUZZER_CMD_SET, .on = true},
			{.type = BUZZER_CMD_DELAY, .duration = (int)request->on_ms},
			{.type = BUZZER_CMD_END},
		};
		execute_pattern(commands, 1);
		return;
	}

	const struct buzzer_pattern_table_entry *entry = &PATTERN_TABLE[request->kind];
	execute_pattern(entry->commands, entry->repetitions);
}

static void thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct buzzer_request request;
		k_msgq_get(&m_buzzer_msgq, &request, K_FOREVER);

		if (k_uptime_get_32() - request.timestamp <= REQUEST_MAX_AGE_MS) {
			execute_request(&request);
		} else {
			LOG_WRN("Discarding stale buzzer request");
		}

		k_sleep(K_MSEC(REQUEST_MIN_DELAY_MS));
	}
}

static int enqueue_request(enum buzzer_request_kind kind, uint32_t on_ms)
{
	if (!m_buzzer_thread_id) {
		return -EAGAIN;
	}

	struct buzzer_request request = {
		.timestamp = k_uptime_get_32(), .kind = kind, .on_ms = on_ms};

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

	return enqueue_request(BUZZER_REQUEST_ON, duration_ms);
}

int app_buzzer_play_info(void)
{
	return enqueue_request(BUZZER_REQUEST_INFO, 0);
}

int app_buzzer_play_warning(void)
{
	return enqueue_request(BUZZER_REQUEST_WARNING, 0);
}

int app_buzzer_play_alarm(void)
{
	return enqueue_request(BUZZER_REQUEST_ALARM, 0);
}
