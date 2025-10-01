/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_pyq1648.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_pyq1648, LOG_LEVEL_INF);

struct pyq1648_param {
	int sensitivity;
	int blind_time;
	int pulse_counter;
	int window_time;
};

struct pyq1648_config {
	uint32_t reg;
	int bit;
};

static const struct gpio_dt_spec m_si_spec = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pir_si_gpios);
static const struct gpio_dt_spec m_dl_spec = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pir_dl_gpios);

static K_THREAD_STACK_DEFINE(m_stack_area, 2048);
static struct k_thread m_thread;

static K_MUTEX_DEFINE(m_lock);
static app_pyq1648_callback m_callback;
static void *m_user_data;

static void write_bit(struct pyq1648_config *config, int value)
{
	config->reg |= value ? BIT(24 - config->bit) : 0;
	config->bit++;
}

static void write_field(struct pyq1648_config *config, uint32_t value, int count)
{
	for (int i = 0; i < count; i++) {
		write_bit(config, value & (1 << (count - i - 1)) ? 1 : 0);
	}
}

static int configure(struct pyq1648_param *param)
{
	int ret;

	struct pyq1648_config config = {0};

	write_field(&config, param->sensitivity, 8);
	write_field(&config, param->blind_time, 4);
	write_field(&config, param->pulse_counter, 2);
	write_field(&config, param->window_time, 2);
	write_field(&config, 2, 2);
	write_field(&config, 0, 2);
	write_field(&config, 16, 5);

	unsigned int key = irq_lock();

	for (int i = 0; i < 25; i++) {
		int bit = (config.reg & BIT(24 - i)) ? 1 : 0;

		ret = gpio_pin_set_dt(&m_si_spec, 1);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_set_dt", ret);
			irq_unlock(key);
			return ret;
		}

		k_busy_wait(bit ? 95 : 5);

		ret = gpio_pin_set_dt(&m_si_spec, 0);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_set_dt", ret);
			irq_unlock(key);
			return ret;
		}

		k_busy_wait(bit ? 5 : 95);
	}

	irq_unlock(key);

	return 0;
}

static int clear_event(void)
{
	int ret;

	ret = gpio_pin_configure_dt(&m_dl_spec, GPIO_OUTPUT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	k_sleep(K_MSEC(10));

	ret = gpio_pin_configure_dt(&m_dl_spec, GPIO_INPUT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	k_sleep(K_MSEC(10));

	return 0;
}

static void thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int ret;

	k_sleep(K_SECONDS(30));

	LOG_INF("Motion detector ready");

	ret = clear_event();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("clear_event", ret);
	}

	for (;;) {
		k_sleep(K_MSEC(100));

		int value = gpio_pin_get_dt(&m_dl_spec);
		if (value < 0) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_get_dt", value);
			continue;
		} else if (!value) {
			continue;
		}

		LOG_DBG("Motion detected");

		ret = clear_event();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("clear_event", ret);
		}

		k_mutex_lock(&m_lock, K_FOREVER);

		if (m_callback) {
			m_callback(m_user_data);
		}

		k_mutex_unlock(&m_lock);
	}
}

int app_pyq1648_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&m_si_spec)) {
		LOG_ERR("GPIO port is not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&m_dl_spec)) {
		LOG_ERR("GPIO port is not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&m_si_spec, GPIO_OUTPUT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&m_dl_spec, GPIO_INPUT);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_configure_dt", ret);
		return ret;
	}

	struct pyq1648_param param = {
		.sensitivity = 32,
		.blind_time = 1,
		.pulse_counter = 1,
		.window_time = 0,
	};

	ret = configure(&param);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("configure", ret);
		return ret;
	}

	k_thread_create(&m_thread, m_stack_area, K_THREAD_STACK_SIZEOF(m_stack_area), thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);

	return 0;
}

void app_pyq1648_set_callback(app_pyq1648_callback callback, void *user_data)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	m_callback = callback;
	m_user_data = user_data;
	k_mutex_unlock(&m_lock);
}
