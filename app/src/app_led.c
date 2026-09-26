/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_led.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_led, LOG_LEVEL_DBG);

#define BLINK_QUEUE_SIZE     3
#define PLAY_QUEUE_SIZE      1
#define REQUEST_MAX_AGE_MS   2000
#define REQUEST_MIN_DELAY_MS 500

#define LED_THREAD_STACK_SIZE 2048
#define LED_THREAD_PRIORITY   K_PRIO_PREEMPT(5)

/* Red + green are HW PWM (TIM2/TIM16) so they can be dimmed; yellow is plain
 * GPIO (PA4 has no timer channel). "On" drives the PWM at LED_DIM_PERCENT duty
 * instead of full brightness, cutting LED current to ~1/5 while staying clearly
 * visible. */
#define LED_DIM_PERCENT 20
static const struct pwm_dt_spec m_led_r = PWM_DT_SPEC_GET(DT_NODELABEL(led_r_pwm));
static const struct pwm_dt_spec m_led_g = PWM_DT_SPEC_GET(DT_NODELABEL(led_g_pwm));
static const struct gpio_dt_spec m_led_y = GPIO_DT_SPEC_GET(DT_NODELABEL(led_y), gpios);

struct blink_request {
	int64_t timestamp;
	struct app_led_blink_req blink;
};

struct play_request {
	int64_t timestamp;
	struct app_led_play_req play;
};

K_MSGQ_DEFINE(m_blink_msgq, sizeof(struct blink_request), BLINK_QUEUE_SIZE, 4);
K_MSGQ_DEFINE(m_play_msgq, sizeof(struct play_request), PLAY_QUEUE_SIZE, 4);
static K_SEM_DEFINE(m_led_sem, 0, K_SEM_MAX_LIMIT);
static K_THREAD_STACK_DEFINE(m_led_thread_stack, LED_THREAD_STACK_SIZE);

static k_tid_t m_led_thread_id;

/* Drive a PWM LED (red/green) to a raw duty percent (0..100). The pulse is the
 * on-time; polarity-inverted in DT for the active-low LED, so pulse=period is
 * full brightness and pulse=0 is off. */
static void set_pwm_pct(const struct pwm_dt_spec *led, int pct)
{
	if (pct < 0) {
		pct = 0;
	} else if (pct > 100) {
		pct = 100;
	}

	uint32_t pulse = (uint32_t)((uint64_t)led->period * pct / 100);
	int ret = pwm_set_dt(led, led->period, pulse);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("pwm_set_dt", ret);
	}
}

static void set(enum app_led_channel channel, int state)
{
	int ret;

	switch (channel) {
	case APP_LED_CHANNEL_R:
	case APP_LED_CHANNEL_G: {
		/* "on" = LED_DIM_PERCENT duty (dimmed), "off" = 0. */
		const struct pwm_dt_spec *led =
			(channel == APP_LED_CHANNEL_R) ? &m_led_r : &m_led_g;

		set_pwm_pct(led, state ? LED_DIM_PERCENT : 0);
		break;
	}
	case APP_LED_CHANNEL_Y:
		ret = gpio_pin_set_dt(&m_led_y, state);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_set_dt", ret);
		}
		break;
	default:
		LOG_ERR("Invalid channel: %d", channel);
		return;
	}
}

void app_led_set(enum app_led_channel channel, int state)
{
	set(channel, state);
}

/* Fade granularity: one PWM update every ~5 ms is smooth to the eye without
 * flooding the bus. A 200 ms heartbeat is then ~40 steps. */
#define LED_FADE_STEP_MS 5

int app_led_fade(enum app_led_channel channel, int from_pct, int to_pct, int duration_ms)
{
	const struct pwm_dt_spec *led;

	switch (channel) {
	case APP_LED_CHANNEL_R:
		led = &m_led_r;
		break;
	case APP_LED_CHANNEL_G:
		led = &m_led_g;
		break;
	default:
		/* Yellow is plain GPIO (no timer channel) — cannot be dimmed. */
		return -EINVAL;
	}

	if (duration_ms < 0) {
		duration_ms = 0;
	}

	int steps = duration_ms / LED_FADE_STEP_MS;
	if (steps < 1) {
		steps = 1;
	}

	for (int i = 0; i <= steps; i++) {
		int pct = from_pct + (to_pct - from_pct) * i / steps;
		set_pwm_pct(led, pct);
		if (i < steps) {
			k_sleep(K_MSEC(duration_ms / steps));
		}
	}

	return 0;
}

int app_led_heartbeat(enum app_led_channel channel)
{
	/* One beat: quick rise to full, slightly slower fall to off — 200 ms total,
	 * which reads as a heartbeat pulse. */
	int ret = app_led_fade(channel, 0, 100, 80);
	if (ret) {
		return ret;
	}

	return app_led_fade(channel, 100, 0, 120);
}

/* --- Periodic idle-state indicator (lrw-mode off) -------------------------- */

/* Default: green PWM heartbeat — the "OK" pulse. Runtime-tunable via shell to
 * compare power against a plain GPIO blink / different colours / dark. */
static enum app_led_idle_mode m_idle_mode = APP_LED_IDLE_PWM;
static enum app_led_channel m_idle_channel = APP_LED_CHANNEL_G;
static int m_idle_on_ms = 5;

void app_led_idle_config(enum app_led_idle_mode mode, enum app_led_channel channel, int on_ms)
{
	/* The PWM heartbeat needs a dimmable channel; yellow is GPIO-only. */
	if (mode == APP_LED_IDLE_PWM && channel == APP_LED_CHANNEL_Y) {
		channel = APP_LED_CHANNEL_G;
	}

	m_idle_mode = mode;
	m_idle_channel = channel;
	if (on_ms > 0) {
		m_idle_on_ms = on_ms;
	}
}

void app_led_idle_get(enum app_led_idle_mode *mode, enum app_led_channel *channel, int *on_ms)
{
	if (mode) {
		*mode = m_idle_mode;
	}
	if (channel) {
		*channel = m_idle_channel;
	}
	if (on_ms) {
		*on_ms = m_idle_on_ms;
	}
}

void app_led_idle_pulse(void)
{
	switch (m_idle_mode) {
	case APP_LED_IDLE_GPIO: {
		struct app_led_blink_req req = {.color = m_idle_channel,
						.duration = m_idle_on_ms,
						.space = 0,
						.repetitions = 1};
		app_led_blink(&req);
		break;
	}
	case APP_LED_IDLE_PWM:
		app_led_heartbeat(m_idle_channel);
		break;
	case APP_LED_IDLE_OFF:
	default:
		break;
	}
}

static void execute_blink(const struct app_led_blink_req *req)
{
	for (int i = 0; i < req->repetitions; i++) {
		set(req->color, 1);
		k_sleep(K_MSEC(req->duration));

		if (i < req->repetitions - 1 && req->space > 0) {
			set(req->color, 0);
			k_sleep(K_MSEC(req->space));
		}
	}

	set(req->color, 0);
}

static void execute_play(const struct app_led_play_req *req)
{
	int length = 0;
	for (int i = 0; i < APP_LED_PLAY_MAX_COMMANDS; i++) {
		if (req->commands[i].type == APP_LED_CMD_END) {
			length = i;
			break;
		}
	}

	if (length == 0) {
		LOG_WRN("Play request has no commands (missing END sentinel?)");
		return;
	}

	for (int rep = 0; rep < req->repetitions; rep++) {
		for (int i = 0; i < length; i++) {
			const struct app_led_cmd *cmd = &req->commands[i];

			/* Skip last delay on last repetition */
			if (rep == req->repetitions - 1 && i == length - 1 &&
			    cmd->type == APP_LED_CMD_DELAY) {
				break;
			}

			switch (cmd->type) {
			case APP_LED_CMD_SET:
				set(cmd->set.channel, cmd->set.state);
				break;
			case APP_LED_CMD_DELAY:
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

	set(APP_LED_CHANNEL_R, 0);
	set(APP_LED_CHANNEL_G, 0);
	set(APP_LED_CHANNEL_Y, 0);
}

static void thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&m_led_sem, K_FOREVER);

		int64_t now = k_uptime_get();

		/* Check play queue first (higher priority) */
		struct play_request play_req;
		if (k_msgq_get(&m_play_msgq, &play_req, K_NO_WAIT) == 0) {
			if (now - play_req.timestamp <= REQUEST_MAX_AGE_MS) {
				execute_play(&play_req.play);
			} else {
				LOG_WRN("Discarding stale LED play request");
			}
		} else {
			struct blink_request blink_req;
			if (k_msgq_get(&m_blink_msgq, &blink_req, K_NO_WAIT) == 0) {
				if (now - blink_req.timestamp <= REQUEST_MAX_AGE_MS) {
					execute_blink(&blink_req.blink);
				} else {
					LOG_WRN("Discarding stale LED blink request");
				}
			}
		}

		k_sleep(K_MSEC(REQUEST_MIN_DELAY_MS));
	}
}

int app_led_init(void)
{
	int ret;

	if (!pwm_is_ready_dt(&m_led_r) || !pwm_is_ready_dt(&m_led_g) ||
	    !gpio_is_ready_dt(&m_led_y)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	/* Start with all LEDs off: PWM pulse 0 (red/green), GPIO inactive (yellow). */
	ret = pwm_set_dt(&m_led_r, m_led_r.period, 0);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("pwm_set_dt", ret);
		return ret;
	}

	ret = pwm_set_dt(&m_led_g, m_led_g.period, 0);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("pwm_set_dt", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&m_led_y, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	static struct k_thread thread;
	m_led_thread_id = k_thread_create(&thread, m_led_thread_stack,
					  K_THREAD_STACK_SIZEOF(m_led_thread_stack), thread_entry,
					  NULL, NULL, NULL, LED_THREAD_PRIORITY, 0, K_NO_WAIT);

	k_thread_name_set(m_led_thread_id, "led");

	return 0;
}

int app_led_blink(const struct app_led_blink_req *req)
{
	if (!m_led_thread_id) {
		return -EAGAIN;
	}

	if (!req || req->repetitions <= 0 || req->duration <= 0) {
		return -EINVAL;
	}

	struct blink_request request = {.timestamp = k_uptime_get(), .blink = *req};

	int ret = k_msgq_put(&m_blink_msgq, &request, K_NO_WAIT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("k_msgq_put", ret);
		return ret;
	}

	k_sem_give(&m_led_sem);

	return 0;
}

int app_led_play(const struct app_led_play_req *req)
{
	if (!m_led_thread_id) {
		return -EAGAIN;
	}

	if (!req || req->repetitions <= 0) {
		return -EINVAL;
	}

	struct play_request request = {.timestamp = k_uptime_get(), .play = *req};

	int ret = k_msgq_put(&m_play_msgq, &request, K_NO_WAIT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("k_msgq_put", ret);
		return ret;
	}

	k_sem_give(&m_led_sem);

	return 0;
}
