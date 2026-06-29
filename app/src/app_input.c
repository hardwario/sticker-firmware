/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_input.h"
#include "app_alarm.h"
#include "app_config.h"
#include "app_gpio_count.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_input, LOG_LEVEL_DBG);

static const struct gpio_dt_spec m_input_a = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
static const struct gpio_dt_spec m_input_b = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios);

static struct app_input_data m_input_data;

K_MUTEX_DEFINE(m_input_data_mutex);

static const struct app_gpio_count_chan m_chan_a = {
	.name = "Input A",
	.counter_en = &g_app_config.input_a_counter,
	.stored_active = &m_input_data.input_a_is_active,
	.count = &m_input_data.input_a_count,
};

static const struct app_gpio_count_chan m_chan_b = {
	.name = "Input B",
	.counter_en = &g_app_config.input_b_counter,
	.stored_active = &m_input_data.input_b_is_active,
	.count = &m_input_data.input_b_count,
};

static int poll(void)
{
	bool input_a_is_active = false;
	bool input_b_is_active = false;

	if (!g_app_config.cap_input_a) {
		input_a_is_active = false;
	}

	if (!g_app_config.cap_input_b) {
		input_b_is_active = false;
	}

	if (g_app_config.cap_input_a) {
		int val = gpio_pin_get_dt(&m_input_a);
		if (val < 0) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_get_dt", val);
			return val;
		}
		input_a_is_active = !val;
	}

	if (g_app_config.cap_input_b) {
		int val = gpio_pin_get_dt(&m_input_b);
		if (val < 0) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_get_dt", val);
			return val;
		}
		input_b_is_active = !val;
	}

	k_mutex_lock(&m_input_data_mutex, K_FOREVER);
	int a_edge = app_gpio_count_apply(&m_chan_a, input_a_is_active);
	int b_edge = app_gpio_count_apply(&m_chan_b, input_b_is_active);
	k_mutex_unlock(&m_input_data_mutex);

	/* Fire alarm events after releasing the data mutex (see app_hall.c): keeps
	 * the data lock off the alarm lock + uplink-enqueue path. */
	if (a_edge > 0) {
		app_alarm_event(APP_ALARM_SRC_INPUT_A, true);
	} else if (a_edge < 0) {
		app_alarm_event(APP_ALARM_SRC_INPUT_A, false);
	}
	if (b_edge > 0) {
		app_alarm_event(APP_ALARM_SRC_INPUT_B, true);
	} else if (b_edge < 0) {
		app_alarm_event(APP_ALARM_SRC_INPUT_B, false);
	}

	return 0;
}

static void input_poll_work_handler(struct k_work *work)
{
	if (!g_app_config.cap_input_a && !g_app_config.cap_input_b) {
		return;
	}

	int ret = poll();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("poll", ret);
	}
}

static K_WORK_DEFINE(m_input_poll_work, input_poll_work_handler);

static void input_timer_handler(struct k_timer *timer)
{
	k_work_submit(&m_input_poll_work);
}

static K_TIMER_DEFINE(m_input_timer, input_timer_handler, NULL);

int app_input_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&m_input_a)) {
		LOG_ERR("Input A GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&m_input_b)) {
		LOG_ERR("Input B GPIO device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&m_input_a, GPIO_INPUT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&m_input_b, GPIO_INPUT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	k_timer_start(&m_input_timer, K_MSEC(100), K_MSEC(100));

	return 0;
}

int app_input_get_data(struct app_input_data *data)
{
	if (!data) {
		return -EINVAL;
	}

	k_mutex_lock(&m_input_data_mutex, K_FOREVER);
	*data = m_input_data;
	k_mutex_unlock(&m_input_data_mutex);

	return 0;
}

void app_input_reset_count(bool input_a, bool input_b)
{
	k_mutex_lock(&m_input_data_mutex, K_FOREVER);
	if (input_a) {
		m_input_data.input_a_count = 0;
	}
	if (input_b) {
		m_input_data.input_b_count = 0;
	}
	k_mutex_unlock(&m_input_data_mutex);
}

void app_input_reset_counts(void)
{
	app_input_reset_count(true, true);
}

void app_input_set_counts(uint32_t input_a, uint32_t input_b)
{
	k_mutex_lock(&m_input_data_mutex, K_FOREVER);
	m_input_data.input_a_count = input_a;
	m_input_data.input_b_count = input_b;
	k_mutex_unlock(&m_input_data_mutex);
}
