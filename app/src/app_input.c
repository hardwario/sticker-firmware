/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_input.h"
#include "app_alarm.h"
#include "app_config.h"
#include "app_log.h"
#include "app_sensor.h"

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
#include <stdint.h>

LOG_MODULE_REGISTER(app_input, LOG_LEVEL_DBG);

static const struct gpio_dt_spec m_input_a = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
static const struct gpio_dt_spec m_input_b = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios);

static struct app_input_data m_input_data;

K_MUTEX_DEFINE(m_input_data_mutex);

/* #498: S0 pulse counting (IEC 62053-31: pulse >= 30 ms). The 100 ms poll would
 * miss short pulses, so an S0 pin counts on its asserting edge in the GPIO
 * interrupt; an edge within S0_DEBOUNCE_MS of the previous counted one is
 * contact bounce. The ISR only bumps m_s0_pending; the poll work folds it into
 * m_input_data under the mutex. */
#define S0_DEBOUNCE_MS 20

static struct gpio_callback m_s0_cb_a;
static struct gpio_callback m_s0_cb_b;
static atomic_t m_s0_pending_a;
static atomic_t m_s0_pending_b;
static uint32_t m_s0_last_a;
static uint32_t m_s0_last_b;

static void s0_edge(atomic_t *pending, uint32_t *last)
{
	uint32_t now = k_uptime_get_32();

	if (now - *last < S0_DEBOUNCE_MS) {
		return;
	}
	*last = now;
	atomic_inc(pending);
}

static void s0_a_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	s0_edge(&m_s0_pending_a, &m_s0_last_a);
}

static void s0_b_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	s0_edge(&m_s0_pending_b, &m_s0_last_b);
}

static int s0_setup(const struct gpio_dt_spec *spec, struct gpio_callback *cb,
		    gpio_callback_handler_t handler)
{
	int ret;

	gpio_init_callback(cb, handler, BIT(spec->pin));
	ret = gpio_add_callback_dt(spec, cb);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_add_callback_dt", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_interrupt_configure_dt", ret);
		return ret;
	}

	return 0;
}

/* #383: `prime` seeds m_input_data from the real GPIO level without treating it
 * as a rise/fall edge — used once at init so an input already asserted before
 * boot isn't reported as a brand-new activation on the first periodic poll
 * (mirrors app_hall.c's #340 M14 fix). */
static int poll_impl(bool prime)
{
	bool input_a_is_active = false;
	bool input_b_is_active = false;
	bool s0_a = g_app_config.cap_gpio_input_a == APP_CONFIG_GPIO_INPUT_S0;
	bool s0_b = g_app_config.cap_gpio_input_b == APP_CONFIG_GPIO_INPUT_S0;

	if (app_sensor_gpio_is_input(g_app_config.cap_gpio_input_a)) {
		int val = gpio_pin_get_dt(&m_input_a);
		if (val < 0) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_get_dt", val);
			return val;
		}
		/* gpio_pin_get_dt() already applies the devicetree GPIO_ACTIVE_LOW
		 * flag (sw2/gp_a), so val is 1 when the input is actually asserted —
		 * do not negate again (#352, matches app_hall.c). */
		input_a_is_active = val;
	}

	if (app_sensor_gpio_is_input(g_app_config.cap_gpio_input_b)) {
		int val = gpio_pin_get_dt(&m_input_b);
		if (val < 0) {
			LOG_ERR_CALL_FAILED_INT("gpio_pin_get_dt", val);
			return val;
		}
		input_b_is_active = val;
	}

	k_mutex_lock(&m_input_data_mutex, K_FOREVER);

	/* An S0 pin counts in its ISR and has no state edges (no state alarms). */
	bool a_rise = !prime && !s0_a && (!m_input_data.input_a_is_active && input_a_is_active);
	bool a_fall = !prime && !s0_a && (m_input_data.input_a_is_active && !input_a_is_active);
	bool b_rise = !prime && !s0_b && (!m_input_data.input_b_is_active && input_b_is_active);
	bool b_fall = !prime && !s0_b && (m_input_data.input_b_is_active && !input_b_is_active);

	/* S0 always counts: counting is all an S0 pin is for (input_x_counter only
	 * gates the digital pin's edge counter). */
	if (s0_a) {
		m_input_data.input_a_count += (uint32_t)atomic_clear(&m_s0_pending_a);
	}
	if (s0_b) {
		m_input_data.input_b_count += (uint32_t)atomic_clear(&m_s0_pending_b);
	}

	m_input_data.input_a_is_active = input_a_is_active;
	m_input_data.input_b_is_active = input_b_is_active;

	if (a_rise) {
		if (g_app_config.input_a_counter) {
			m_input_data.input_a_count++;
		}
		LOG_DBG("Input A activated, count: %u", m_input_data.input_a_count);
	} else if (a_fall) {
		LOG_DBG("Input A deactivated");
	}

	if (b_rise) {
		if (g_app_config.input_b_counter) {
			m_input_data.input_b_count++;
		}
		LOG_DBG("Input B activated, count: %u", m_input_data.input_b_count);
	} else if (b_fall) {
		LOG_DBG("Input B deactivated");
	}

	k_mutex_unlock(&m_input_data_mutex);

	/* Fire alarm events after releasing the data mutex (see app_hall.c): keeps
	 * the data lock off the alarm lock + uplink-enqueue path. */
	if (a_rise) {
		app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_STATE, true);
	} else if (a_fall) {
		app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_STATE, false);
	}
	if (b_rise) {
		app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_INPUT_B_STATE, true);
	} else if (b_fall) {
		app_alarm_event(APP_SENSOR_CH_MOTHERBOARD_INPUT_B_STATE, false);
	}

	return 0;
}

static int poll(void)
{
	return poll_impl(false);
}

static void input_poll_work_handler(struct k_work *work)
{
	if (!app_sensor_gpio_is_input(g_app_config.cap_gpio_input_a) &&
	    !app_sensor_gpio_is_input(g_app_config.cap_gpio_input_b)) {
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

	/* #383: seed m_input_data from the real level before the periodic timer's
	 * first tick, so an input already asserted at boot isn't latched as a fresh
	 * activation edge (spurious counter increment + alarm event). */
	ret = poll_impl(true);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("poll_impl", ret);
		return ret;
	}

	if (g_app_config.cap_gpio_input_a == APP_CONFIG_GPIO_INPUT_S0) {
		ret = s0_setup(&m_input_a, &m_s0_cb_a, s0_a_handler);
		if (ret) {
			return ret;
		}
	}
	if (g_app_config.cap_gpio_input_b == APP_CONFIG_GPIO_INPUT_S0) {
		ret = s0_setup(&m_input_b, &m_s0_cb_b, s0_b_handler);
		if (ret) {
			return ret;
		}
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
		atomic_clear(&m_s0_pending_a);
	}
	if (input_b) {
		m_input_data.input_b_count = 0;
		atomic_clear(&m_s0_pending_b);
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
