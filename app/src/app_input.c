/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_input.h"
#include "app_config.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_send.h"

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

static int poll(void)
{
	bool input_a_was_active;
	bool input_a_is_active;

	bool input_b_was_active;
	bool input_b_is_active;

	k_mutex_lock(&m_input_data_mutex, K_FOREVER);
	input_a_was_active = m_input_data.input_a_is_active;
	input_b_was_active = m_input_data.input_b_is_active;
	k_mutex_unlock(&m_input_data_mutex);

	if (!g_app_config.cap_input_a) {
		input_a_is_active = false;
	}

	if (!g_app_config.cap_input_b) {
		input_b_is_active = false;
	}

	if (g_app_config.cap_input_a) {
		input_a_is_active = !gpio_pin_get_dt(&m_input_a);
	}

	if (g_app_config.cap_input_b) {
		input_b_is_active = !gpio_pin_get_dt(&m_input_b);
	}

	k_mutex_lock(&m_input_data_mutex, K_FOREVER);

	m_input_data.input_a_is_active = input_a_is_active;
	m_input_data.input_b_is_active = input_b_is_active;

	if (!input_a_was_active && input_a_is_active) {
		if (g_app_config.input_a_counter) {
			m_input_data.input_a_count++;
		}

		LOG_DBG("Input A activated, count: %u", m_input_data.input_a_count);

		if (g_app_config.input_a_notify_act) {
			m_input_data.input_a_notify_act = true;
		}
	}

	if (input_a_was_active && !input_a_is_active) {
		LOG_DBG("Input A deactivated");

		if (g_app_config.input_a_notify_deact) {
			m_input_data.input_a_notify_deact = true;
		}
	}

	if (!input_b_was_active && input_b_is_active) {
		if (g_app_config.input_b_counter) {
			m_input_data.input_b_count++;
		}

		LOG_DBG("Input B activated, count: %u", m_input_data.input_b_count);

		if (g_app_config.input_b_notify_act) {
			m_input_data.input_b_notify_act = true;
		}
	}

	if (input_b_was_active && !input_b_is_active) {
		LOG_DBG("Input B deactivated");

		if (g_app_config.input_b_notify_deact) {
			m_input_data.input_b_notify_deact = true;
		}
	}

	k_mutex_unlock(&m_input_data_mutex);

	if (app_input_check_notify_event()) {
#if defined(CONFIG_LORAWAN)
		app_lrw_send();
#endif /* defined(CONFIG_LORAWAN) */
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

void app_input_clear_notify_flags(struct app_input_data *data)
{
	if (!data) {
		return;
	}

	k_mutex_lock(&m_input_data_mutex, K_FOREVER);

	if (data->input_a_notify_act) {
		m_input_data.input_a_notify_act = false;
	}

	if (data->input_a_notify_deact) {
		m_input_data.input_a_notify_deact = false;
	}

	if (data->input_b_notify_act) {
		m_input_data.input_b_notify_act = false;
	}

	if (data->input_b_notify_deact) {
		m_input_data.input_b_notify_deact = false;
	}

	k_mutex_unlock(&m_input_data_mutex);
}

bool app_input_check_notify_event(void)
{
	bool has_event;

	k_mutex_lock(&m_input_data_mutex, K_FOREVER);
	has_event = m_input_data.input_a_notify_act || m_input_data.input_a_notify_deact ||
		    m_input_data.input_b_notify_act || m_input_data.input_b_notify_deact;
	k_mutex_unlock(&m_input_data_mutex);

	return has_event;
}
