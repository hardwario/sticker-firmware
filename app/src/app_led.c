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

LOG_MODULE_REGISTER(app_led, LOG_LEVEL_DBG);

static const struct gpio_dt_spec m_led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(led_r), gpios);
static const struct gpio_dt_spec m_led_g = GPIO_DT_SPEC_GET(DT_NODELABEL(led_g), gpios);
static const struct gpio_dt_spec m_led_y = GPIO_DT_SPEC_GET(DT_NODELABEL(led_y), gpios);

void app_led_set(enum app_led_channel channel, int state)
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
		LOG_ERR_CALL_FAILED_INT("gpio_pin_set", ret);
		return;
	}
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

	return 0;
}

SYS_INIT(init, APPLICATION, 0);
