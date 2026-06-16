/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_hall.h"
#include "app_alarm.h"
#include "app_config.h"
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

LOG_MODULE_REGISTER(app_hall, LOG_LEVEL_DBG);

static const struct gpio_dt_spec m_hall_left = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec m_hall_right = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);

static struct app_hall_data m_hall_data;

K_MUTEX_DEFINE(m_hall_data_mutex);

static int poll(void)
{
	int ret;

	bool left_was_active;
	bool left_is_active;

	bool right_was_active;
	bool right_is_active;

	k_mutex_lock(&m_hall_data_mutex, K_FOREVER);
	left_was_active = m_hall_data.left_is_active;
	right_was_active = m_hall_data.right_is_active;
	k_mutex_unlock(&m_hall_data_mutex);

	if (!g_app_config.cap_hall_left) {
		left_is_active = false;
	}

	if (!g_app_config.cap_hall_right) {
		right_is_active = false;
	}

	if (g_app_config.cap_hall_left) {
		ret = gpio_pin_configure_dt(&m_hall_left, GPIO_INPUT | GPIO_PULL_UP);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
			goto restore;
		}
	}

	if (g_app_config.cap_hall_right) {
		ret = gpio_pin_configure_dt(&m_hall_right, GPIO_INPUT | GPIO_PULL_UP);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
			goto restore;
		}
	}

	/* Let the pull-up settle before sampling. 2 us is borderline for the pin
	 * RC; 10 us costs nothing and is safely above it. */
	k_busy_wait(10);

	if (g_app_config.cap_hall_left) {
		int val = gpio_pin_get_dt(&m_hall_left);
		if (val < 0) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_get_dt", val);
			ret = val;
			goto restore;
		}
		left_is_active = !val;
	}

	if (g_app_config.cap_hall_right) {
		int val = gpio_pin_get_dt(&m_hall_right);
		if (val < 0) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_get_dt", val);
			ret = val;
			goto restore;
		}
		right_is_active = !val;
	}

restore:
	if (g_app_config.cap_hall_left) {
		int err = gpio_pin_configure_dt(&m_hall_left, GPIO_INPUT | GPIO_PULL_DOWN);
		if (err) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", err);
			if (!ret) {
				ret = err;
			}
		}
	}

	if (g_app_config.cap_hall_right) {
		int err = gpio_pin_configure_dt(&m_hall_right, GPIO_INPUT | GPIO_PULL_DOWN);
		if (err) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", err);
			if (!ret) {
				ret = err;
			}
		}
	}

	if (ret) {
		return ret;
	}

	k_mutex_lock(&m_hall_data_mutex, K_FOREVER);

	m_hall_data.left_is_active = left_is_active;
	m_hall_data.right_is_active = right_is_active;

	if (!left_was_active && left_is_active) {
		if (g_app_config.hall_left_counter) {
			m_hall_data.left_count++;
		}

		LOG_DBG("Left hall switch activated, count: %u", m_hall_data.left_count);

		app_alarm_event(APP_ALARM_SRC_HALL_LEFT, true);
	}

	if (left_was_active && !left_is_active) {
		LOG_DBG("Left hall switch deactivated");

		app_alarm_event(APP_ALARM_SRC_HALL_LEFT, false);
	}

	if (!right_was_active && right_is_active) {
		if (g_app_config.hall_right_counter) {
			m_hall_data.right_count++;
		}

		LOG_DBG("Right hall switch activated, count: %u", m_hall_data.right_count);

		app_alarm_event(APP_ALARM_SRC_HALL_RIGHT, true);
	}

	if (right_was_active && !right_is_active) {
		LOG_DBG("Right hall switch deactivated");

		app_alarm_event(APP_ALARM_SRC_HALL_RIGHT, false);
	}

	k_mutex_unlock(&m_hall_data_mutex);

	return 0;
}

static void hall_poll_work_handler(struct k_work *work)
{
	if (!g_app_config.cap_hall_left && !g_app_config.cap_hall_right) {
		return;
	}

	int ret = poll();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("poll", ret);
	}
}

static K_WORK_DEFINE(m_hall_poll_work, hall_poll_work_handler);

static void hall_timer_handler(struct k_timer *timer)
{
	k_work_submit(&m_hall_poll_work);
}

static K_TIMER_DEFINE(m_hall_timer, hall_timer_handler, NULL);

int app_hall_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&m_hall_left)) {
		LOG_ERR("Hall left GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&m_hall_right)) {
		LOG_ERR("Hall right GPIO device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&m_hall_left, GPIO_INPUT | GPIO_PULL_DOWN);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&m_hall_right, GPIO_INPUT | GPIO_PULL_DOWN);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	k_timer_start(&m_hall_timer, K_MSEC(100), K_MSEC(100));

	return 0;
}

int app_hall_get_data(struct app_hall_data *data)
{
	if (!data) {
		return -EINVAL;
	}

	k_mutex_lock(&m_hall_data_mutex, K_FOREVER);
	*data = m_hall_data;
	k_mutex_unlock(&m_hall_data_mutex);

	return 0;
}

void app_hall_reset_count(bool left, bool right)
{
	k_mutex_lock(&m_hall_data_mutex, K_FOREVER);
	if (left) {
		m_hall_data.left_count = 0;
	}
	if (right) {
		m_hall_data.right_count = 0;
	}
	k_mutex_unlock(&m_hall_data_mutex);
}

void app_hall_reset_counts(void)
{
	app_hall_reset_count(true, true);
}

void app_hall_set_counts(uint32_t left, uint32_t right)
{
	k_mutex_lock(&m_hall_data_mutex, K_FOREVER);
	m_hall_data.left_count = left;
	m_hall_data.right_count = right;
	k_mutex_unlock(&m_hall_data_mutex);
}
