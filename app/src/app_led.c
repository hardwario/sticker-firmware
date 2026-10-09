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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_led, LOG_LEVEL_DBG);

#define BLINK_QUEUE_SIZE     3
#define PLAY_QUEUE_SIZE      1
#define REQUEST_MAX_AGE_MS   2000
#define REQUEST_MIN_DELAY_MS 500

/* HW high-water mark 456 B (CONFIG_INIT_STACKS, 2026-10-09); >= 2x margin. */
#define LED_THREAD_STACK_SIZE 1024
#define LED_THREAD_PRIORITY   K_PRIO_PREEMPT(5)

static const struct gpio_dt_spec m_led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(led_r), gpios);
static const struct gpio_dt_spec m_led_g = GPIO_DT_SPEC_GET(DT_NODELABEL(led_g), gpios);
static const struct gpio_dt_spec m_led_y = GPIO_DT_SPEC_GET(DT_NODELABEL(led_y), gpios);

struct blink_request {
	int64_t timestamp;
	atomic_val_t gen;
	struct app_led_blink_req blink;
};

struct play_request {
	int64_t timestamp;
	atomic_val_t gen;
	struct app_led_play_req play;
};

K_MSGQ_DEFINE(m_blink_msgq, sizeof(struct blink_request), BLINK_QUEUE_SIZE, 4);
K_MSGQ_DEFINE(m_play_msgq, sizeof(struct play_request), PLAY_QUEUE_SIZE, 4);
static K_SEM_DEFINE(m_led_sem, 0, K_SEM_MAX_LIMIT);
static K_THREAD_STACK_DEFINE(m_led_thread_stack, LED_THREAD_STACK_SIZE);

static k_tid_t m_led_thread_id;

/* Indicator hold (app_led_hold): while set, the thread writes no pin, cuts a
 * running blink/play short and drops requests. m_hold_gen advances on every
 * take, so a request queued before it never runs after the release. */
static atomic_t m_hold;
static atomic_t m_hold_gen;

static void set(enum app_led_channel channel, int state)
{
	int ret;

	const struct gpio_dt_spec *led;

	switch (channel) {
	case APP_LED_CHANNEL_R:
		led = &m_led_r;
		break;
	case APP_LED_CHANNEL_G:
		led = &m_led_g;
		break;
	case APP_LED_CHANNEL_Y:
		led = &m_led_y;
		break;
	default:
		LOG_ERR("Invalid channel: %d", channel);
		return;
	}

	ret = gpio_pin_set_dt(led, state);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_set_dt", ret);
	}
}

void app_led_set(enum app_led_channel channel, int state)
{
	set(channel, state);
}

/* Pin write from the LED thread. The check and the write run under irq_lock, the
 * same lock the hold holder writes under, so no thread write lands after the
 * holder has taken the pins. */
static void thread_set(enum app_led_channel channel, int state)
{
	unsigned int key = irq_lock();

	if (!atomic_get(&m_hold)) {
		set(channel, state);
	}

	irq_unlock(key);
}

static bool held_since(atomic_val_t gen)
{
	return atomic_get(&m_hold) || atomic_get(&m_hold_gen) != gen;
}

static void all_off(void)
{
	thread_set(APP_LED_CHANNEL_R, 0);
	thread_set(APP_LED_CHANNEL_G, 0);
	thread_set(APP_LED_CHANNEL_Y, 0);
}

static void execute_blink(const struct app_led_blink_req *req, atomic_val_t gen)
{
	for (int i = 0; i < req->repetitions && !held_since(gen); i++) {
		thread_set(req->color, 1);
		k_sleep(K_MSEC(req->duration));

		if (i < req->repetitions - 1 && req->space > 0 && !held_since(gen)) {
			thread_set(req->color, 0);
			k_sleep(K_MSEC(req->space));
		}
	}

	thread_set(req->color, 0);
}

static void execute_play(const struct app_led_play_req *req, atomic_val_t gen)
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

			if (held_since(gen)) {
				goto out;
			}

			/* Skip last delay on last repetition */
			if (rep == req->repetitions - 1 && i == length - 1 &&
			    cmd->type == APP_LED_CMD_DELAY) {
				break;
			}

			switch (cmd->type) {
			case APP_LED_CMD_SET:
				thread_set(cmd->set.channel, cmd->set.state);
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

out:
	all_off();
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
			/* A stale request (queued behind a longer one, e.g. a heartbeat
			 * during the boot carousel) or one queued before the indicator
			 * was taken is dropped: every request is a periodic or best-effort
			 * indication, so the next one follows. */
			if (!held_since(play_req.gen) &&
			    now - play_req.timestamp <= REQUEST_MAX_AGE_MS) {
				execute_play(&play_req.play, play_req.gen);
			}
		} else {
			struct blink_request blink_req;
			if (k_msgq_get(&m_blink_msgq, &blink_req, K_NO_WAIT) == 0) {
				if (!held_since(blink_req.gen) &&
				    now - blink_req.timestamp <= REQUEST_MAX_AGE_MS) {
					execute_blink(&blink_req.blink, blink_req.gen);
				}
			}
		}

		k_sleep(K_MSEC(REQUEST_MIN_DELAY_MS));
	}
}

int app_led_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&m_led_r) || !gpio_is_ready_dt(&m_led_g) ||
	    !gpio_is_ready_dt(&m_led_y)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&m_led_r, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&m_led_g, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
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

	if (atomic_get(&m_hold)) {
		return -EBUSY;
	}

	struct blink_request request = {
		.timestamp = k_uptime_get(), .gen = atomic_get(&m_hold_gen), .blink = *req};

	/* A full queue drops the request (-ENOMSG), like a stale one. */
	int ret = k_msgq_put(&m_blink_msgq, &request, K_NO_WAIT);
	if (ret) {
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

	if (atomic_get(&m_hold)) {
		return -EBUSY;
	}

	struct play_request request = {
		.timestamp = k_uptime_get(), .gen = atomic_get(&m_hold_gen), .play = *req};

	int ret = k_msgq_put(&m_play_msgq, &request, K_NO_WAIT);
	if (ret) {
		return ret;
	}

	k_sem_give(&m_led_sem);

	return 0;
}

void app_led_hold(bool hold)
{
	if (!hold) {
		atomic_clear(&m_hold);
		return;
	}

	if (atomic_cas(&m_hold, 0, 1)) {
		atomic_inc(&m_hold_gen);
		/* Flags first, then the wake: a sleeping blink/play step returns early
		 * and sees the hold at its next check. */
		if (m_led_thread_id) {
			k_wakeup(m_led_thread_id);
		}
	}
}
