/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config_legacy.h"
#include "app_config.h"

/* Zephyr includes */
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

LOG_MODULE_REGISTER(app_config_legacy, LOG_LEVEL_INF);

/* #498: the bool GP capabilities replaced by cap_gpio_mode / cap_gpio_input_a/_b.
 * Full settings names, so cleanup needs no formatting (snprintk would link the
 * whole printf into the release image); h_set gets them without the prefix. */
#define KEY_PFX "config/"

enum legacy_key {
	LEGACY_INPUT_A,
	LEGACY_INPUT_B,
	LEGACY_ANALOG_A,
	LEGACY_ANALOG_B,
	LEGACY_PIR,
	LEGACY_BUZZER,
	LEGACY_COUNT,
};

static const char *const m_keys[LEGACY_COUNT] = {
	[LEGACY_INPUT_A] = KEY_PFX "cap-input-a",   [LEGACY_INPUT_B] = KEY_PFX "cap-input-b",
	[LEGACY_ANALOG_A] = KEY_PFX "cap-analog-a", [LEGACY_ANALOG_B] = KEY_PFX "cap-analog-b",
	[LEGACY_PIR] = KEY_PFX "cap-pir-detector",  [LEGACY_BUZZER] = KEY_PFX "cap-buzzer",
};

static bool m_value[LEGACY_COUNT];
static uint32_t m_loaded; /* BIT(legacy_key) per key found in NVS */

int app_config_legacy_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	for (int i = 0; i < LEGACY_COUNT; i++) {
		if (!settings_name_steq(key, m_keys[i] + sizeof(KEY_PFX) - 1, &next) || next) {
			continue;
		}
		if (len != sizeof(bool)) {
			return -EINVAL;
		}

		bool v;
		int ret = read_cb(cb_arg, &v, sizeof(v));

		if (ret < 0) {
			LOG_ERR("Call `read_cb` failed: %d", ret);
			return ret;
		}
		m_value[i] = v;
		m_loaded |= BIT(i);
		return 0;
	}

	return -ENOENT;
}

/* Value of legacy key `k`; false when the key was not in NVS. */
static bool legacy(enum legacy_key k)
{
	return (m_loaded & BIT(k)) && m_value[k];
}

static enum app_config_gpio_input pin_mode(bool digital, bool analog)
{
	if (digital) {
		return APP_CONFIG_GPIO_INPUT_DIGITAL;
	}
	return analog ? APP_CONFIG_GPIO_INPUT_ANALOG : APP_CONFIG_GPIO_INPUT_DISABLED;
}

bool app_config_legacy_commit(struct app_config *cfg)
{
	if (!m_loaded) {
		return false;
	}

	/* Same precedence app_sensor_init() applied to the bool caps, so the
	 * device keeps doing exactly what it did before the upgrade. Legacy keys
	 * exist only until this conversion is persisted, so they always win over
	 * the (default) new parameters. */
	enum app_config_gpio_input a = pin_mode(legacy(LEGACY_INPUT_A), legacy(LEGACY_ANALOG_A));
	enum app_config_gpio_input b = pin_mode(legacy(LEGACY_INPUT_B), legacy(LEGACY_ANALOG_B));

	if (legacy(LEGACY_PIR)) {
		cfg->cap_gpio_mode = APP_CONFIG_GPIO_MODE_PIR;
	} else if (legacy(LEGACY_BUZZER)) {
		cfg->cap_gpio_mode = APP_CONFIG_GPIO_MODE_BUZZER;
	} else if (a != APP_CONFIG_GPIO_INPUT_DISABLED || b != APP_CONFIG_GPIO_INPUT_DISABLED) {
		cfg->cap_gpio_mode = APP_CONFIG_GPIO_MODE_INPUTS;
	} else {
		cfg->cap_gpio_mode = APP_CONFIG_GPIO_MODE_NONE;
	}

	/* PIR / buzzer own both pins, so the input modes would be inactive
	 * anyway; store none rather than a stale mode the old config lost. */
	if (cfg->cap_gpio_mode == APP_CONFIG_GPIO_MODE_INPUTS) {
		cfg->cap_gpio_input_a = a;
		cfg->cap_gpio_input_b = b;
	} else {
		cfg->cap_gpio_input_a = APP_CONFIG_GPIO_INPUT_DISABLED;
		cfg->cap_gpio_input_b = APP_CONFIG_GPIO_INPUT_DISABLED;
	}

	LOG_WRN("Converted legacy GP caps (0x%02x) to cap-gpio-mode %d, input-a %d, input-b %d",
		m_loaded, cfg->cap_gpio_mode, cfg->cap_gpio_input_a, cfg->cap_gpio_input_b);

	return true;
}

void app_config_legacy_cleanup(void)
{
	for (int i = 0; i < LEGACY_COUNT; i++) {
		if (!(m_loaded & BIT(i))) {
			continue;
		}

		int ret = settings_delete(m_keys[i]);

		if (ret) {
			LOG_ERR("Call `settings_delete` failed (%s): %d", m_keys[i], ret);
			continue;
		}
		m_loaded &= ~BIT(i);
	}
}
