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

/* #498: pulse counting (e.g. S0 meter outputs, IEC 62053-31: pulse >= 30 ms).
 * The 100 ms poll would miss short pulses, so a pulse pin counts from its GPIO
 * interrupt. The pulse polarity is unknown (a meter may pull the line low or
 * drive it high), so the pin interrupts on both edges and a pulse is a move
 * AWAY from the idle level, i.e. the level at init. If init caught a pulse in
 * progress, the idle level is the pulse level: then every pulse counts on its
 * trailing edge instead of its leading one — still exactly once, and the
 * pulse caught at init counts on its release. The idle level is never
 * re-learned: a long pulse would otherwise count twice.
 *
 * Debounce (#503): every edge (re)starts a PULSE_SETTLE_MS one-shot timer, and
 * only when it expires, with the pin quiet for that long, is the level read
 * and compared with the last settled level. A change away from idle counts.
 * Reading the level once the bounce is over, not in the ISR of the first edge
 * (mid-bounce after a Stop2 wake), cannot miss a leading edge or count a
 * trailing one, and a pin that chatters counts once it settles. A pulse (or a
 * gap) shorter than PULSE_SETTLE_MS is not seen; S0 pulses are >= 30 ms. The
 * timer only bumps m_pulse_x.pending; the poll work folds it into
 * m_input_data under the mutex. */
#define PULSE_SETTLE_MS 10

struct pulse_state {
	const struct gpio_dt_spec *spec;
	struct gpio_callback cb;
	struct k_timer settle;
	atomic_t pending;
	atomic_t idle_level;
	atomic_t stable_level;
};

static struct pulse_state m_pulse_a = {.spec = &m_input_a};
static struct pulse_state m_pulse_b = {.spec = &m_input_b};

static void pulse_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct pulse_state *st = CONTAINER_OF(cb, struct pulse_state, cb);

	k_timer_start(&st->settle, K_MSEC(PULSE_SETTLE_MS), K_NO_WAIT);
}

static void pulse_settled(struct k_timer *timer)
{
	struct pulse_state *st = CONTAINER_OF(timer, struct pulse_state, settle);
	int level = gpio_pin_get_raw(st->spec->port, st->spec->pin);

	if (level < 0 || level == (int)atomic_get(&st->stable_level)) {
		return; /* bounce that returned, or a pulse shorter than the settle */
	}
	atomic_set(&st->stable_level, level);
	if (level != (int)atomic_get(&st->idle_level)) {
		atomic_inc(&st->pending);
	}
}

static int pulse_setup(struct pulse_state *st)
{
	int ret;
	int level = gpio_pin_get_raw(st->spec->port, st->spec->pin);

	if (level < 0) {
		LOG_ERR_CALL_FAILED_INT("gpio_pin_get_raw", level);
		return level;
	}
	atomic_set(&st->idle_level, level);
	atomic_set(&st->stable_level, level);
	k_timer_init(&st->settle, pulse_settled, NULL);

	gpio_init_callback(&st->cb, pulse_handler, BIT(st->spec->pin));
	ret = gpio_add_callback_dt(st->spec, &st->cb);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("gpio_add_callback_dt", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(st->spec, GPIO_INT_EDGE_BOTH);
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
	bool pulse_a = g_app_config.cap_gpio_input_a == APP_CONFIG_GPIO_INPUT_PULSE;
	bool pulse_b = g_app_config.cap_gpio_input_b == APP_CONFIG_GPIO_INPUT_PULSE;

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

	/* A pulse pin counts in its ISR and has no state edges (no state alarms). */
	bool a_rise = !prime && !pulse_a && (!m_input_data.input_a_is_active && input_a_is_active);
	bool a_fall = !prime && !pulse_a && (m_input_data.input_a_is_active && !input_a_is_active);
	bool b_rise = !prime && !pulse_b && (!m_input_data.input_b_is_active && input_b_is_active);
	bool b_fall = !prime && !pulse_b && (m_input_data.input_b_is_active && !input_b_is_active);

	/* A pulse pin always counts: counting is all a pulse pin is for (input_x_counter only
	 * gates the digital pin's edge counter). */
	uint32_t a_pulses = 0;
	uint32_t b_pulses = 0;

	if (pulse_a) {
		a_pulses = (uint32_t)atomic_clear(&m_pulse_a.pending);
		m_input_data.input_a_count += a_pulses;
	}
	if (pulse_b) {
		b_pulses = (uint32_t)atomic_clear(&m_pulse_b.pending);
		m_input_data.input_b_count += b_pulses;
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
	/* A pulse pin has no state edge, but shows its pulses on the commissioning
	 * event LED like a digital input does. */
	if (a_pulses) {
		app_alarm_activity(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_COUNT);
	}
	if (b_pulses) {
		app_alarm_activity(APP_SENSOR_CH_MOTHERBOARD_INPUT_B_COUNT);
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

	if (g_app_config.cap_gpio_input_a == APP_CONFIG_GPIO_INPUT_PULSE) {
		ret = pulse_setup(&m_pulse_a);
		if (ret) {
			return ret;
		}
	}
	if (g_app_config.cap_gpio_input_b == APP_CONFIG_GPIO_INPUT_PULSE) {
		ret = pulse_setup(&m_pulse_b);
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
		atomic_clear(&m_pulse_a.pending);
	}
	if (input_b) {
		m_input_data.input_b_count = 0;
		atomic_clear(&m_pulse_b.pending);
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
