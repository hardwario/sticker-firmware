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
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_led, LOG_LEVEL_DBG);

#define REQUEST_QUEUE_SIZE   3
#define REQUEST_MAX_AGE_MS   2000
#define REQUEST_MIN_DELAY_MS 500

#define LED_THREAD_STACK_SIZE 2048
#define LED_THREAD_PRIORITY   K_PRIO_PREEMPT(5)

static const struct gpio_dt_spec m_led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(led_r), gpios);
static const struct gpio_dt_spec m_led_g = GPIO_DT_SPEC_GET(DT_NODELABEL(led_g), gpios);
static const struct gpio_dt_spec m_led_y = GPIO_DT_SPEC_GET(DT_NODELABEL(led_y), gpios);

enum request_type {
	REQUEST_TYPE_BLINK = 0,
	REQUEST_TYPE_PLAY = 1,
};

struct request {
	enum request_type type;
	int64_t timestamp;
	union {
		struct app_led_blink_req blink;
		struct app_led_play_req play;
	};
};

K_MSGQ_DEFINE(m_led_msgq, sizeof(struct request), REQUEST_QUEUE_SIZE, 4);
static K_THREAD_STACK_DEFINE(m_led_thread_stack, LED_THREAD_STACK_SIZE);

static k_tid_t m_led_thread_id;

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

static void process_request(struct request *req)
{
	int64_t age = k_uptime_get() - req->timestamp;
	if (age > REQUEST_MAX_AGE_MS) {
		LOG_WRN("Discarding stale LED request (age: %lld ms)", age);
		return;
	}

	switch (req->type) {
	case REQUEST_TYPE_BLINK:
		execute_blink(&req->blink);
		break;
	case REQUEST_TYPE_PLAY:
		execute_play(&req->play);
		break;
	default:
		LOG_ERR("Invalid request type: %d", req->type);
		break;
	}
}

static void thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int ret;

	struct request req;

	for (;;) {
		ret = k_msgq_get(&m_led_msgq, &req, K_FOREVER);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("k_msgq_get", ret);
			continue;
		}

		process_request(&req);

		k_sleep(K_MSEC(REQUEST_MIN_DELAY_MS));
	}
}

int app_led_blink(const struct app_led_blink_req *req)
{
	if (!m_led_thread_id) {
		return -EAGAIN;
	}

	if (!req || req->repetitions <= 0 || req->duration <= 0) {
		return -EINVAL;
	}

	struct request request = {
		.type = REQUEST_TYPE_BLINK, .timestamp = k_uptime_get(), .blink = *req};

	int ret = k_msgq_put(&m_led_msgq, &request, K_NO_WAIT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("k_msgq_put", ret);
		return ret;
	}

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

	struct request request = {
		.type = REQUEST_TYPE_PLAY, .timestamp = k_uptime_get(), .play = *req};

	int ret = k_msgq_put(&m_led_msgq, &request, K_NO_WAIT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("k_msgq_put", ret);
		return ret;
	}

	return 0;
}

static int init(void)
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

SYS_INIT(init, APPLICATION, 0);
