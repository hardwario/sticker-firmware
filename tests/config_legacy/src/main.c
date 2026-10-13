/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for app_config_legacy.c (#498): NVS values of the removed bool GP
 * caps (cap-input-a/-b, cap-analog-a/-b, cap-pir-detector, cap-buzzer) become
 * cap_gpio_mode / cap_gpio_input_a/_b with the precedence app_sensor_init()
 * used to apply at run time (PIR > buzzer > digital > analog).
 */

#include "app_config.h"
#include "app_config_legacy.h"

#include <errno.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/ztest.h>

/* ---- settings stubs (no backend in this build) ---------------------------- */

int settings_name_steq(const char *name, const char *key, const char **next)
{
	size_t n = strlen(key);

	if (strncmp(name, key, n) != 0) {
		return 0;
	}
	if (name[n] == '\0') {
		*next = NULL;
		return 1;
	}
	if (name[n] == '/') {
		*next = &name[n + 1];
		return 1;
	}
	return 0;
}

static char m_deleted[8][32];
static int m_deleted_n;

int settings_delete(const char *name)
{
	strncpy(m_deleted[m_deleted_n], name, sizeof(m_deleted[0]) - 1);
	m_deleted_n++;
	return 0;
}

/* ---- helpers --------------------------------------------------------------- */

static ssize_t read_bool(void *cb_arg, void *data, size_t len)
{
	memcpy(data, cb_arg, len);
	return (ssize_t)len;
}

static int load(const char *key, bool value)
{
	return app_config_legacy_set(key, sizeof(bool), read_bool, &value);
}

static struct app_config m_cfg;

static void before(void *unused)
{
	ARG_UNUSED(unused);
	/* Forget the previous case's keys (also exercises the cleanup path). */
	app_config_legacy_cleanup();
	m_deleted_n = 0;
	memset(m_deleted, 0, sizeof(m_deleted));
	m_cfg = (struct app_config){0};
}

ZTEST_SUITE(config_legacy, NULL, NULL, before, NULL, NULL);

/* ---- cases ----------------------------------------------------------------- */

ZTEST(config_legacy, test_no_legacy_keys_no_conversion)
{
	m_cfg.cap_gpio_mode = APP_CONFIG_GPIO_MODE_INPUTS;
	m_cfg.cap_gpio_input_a = APP_CONFIG_GPIO_INPUT_S0;

	zassert_false(app_config_legacy_commit(&m_cfg), "nothing to convert");
	zassert_equal(m_cfg.cap_gpio_mode, APP_CONFIG_GPIO_MODE_INPUTS, "new config untouched");
	zassert_equal(m_cfg.cap_gpio_input_a, APP_CONFIG_GPIO_INPUT_S0, "new config untouched");
}

ZTEST(config_legacy, test_unknown_key_and_bad_length)
{
	bool v = true;

	zassert_equal(load("interval-report", true), -ENOENT, "not a legacy key");
	zassert_equal(load("cap-input-a/x", true), -ENOENT, "subkey is not the key");
	zassert_equal(app_config_legacy_set("cap-input-a", 4, read_bool, &v), -EINVAL, "len");
	zassert_false(app_config_legacy_commit(&m_cfg), "nothing loaded");
}

ZTEST(config_legacy, test_digital_wins_over_analog_per_pin)
{
	zassert_ok(load("cap-input-a", true));
	zassert_ok(load("cap-analog-a", true));
	zassert_ok(load("cap-input-b", false));
	zassert_ok(load("cap-analog-b", true));

	zassert_true(app_config_legacy_commit(&m_cfg));
	zassert_equal(m_cfg.cap_gpio_mode, APP_CONFIG_GPIO_MODE_INPUTS);
	zassert_equal(m_cfg.cap_gpio_input_a, APP_CONFIG_GPIO_INPUT_DIGITAL);
	zassert_equal(m_cfg.cap_gpio_input_b, APP_CONFIG_GPIO_INPUT_ANALOG);
}

ZTEST(config_legacy, test_pir_wins_over_buzzer_and_inputs)
{
	zassert_ok(load("cap-pir-detector", true));
	zassert_ok(load("cap-buzzer", true));
	zassert_ok(load("cap-input-a", true));

	zassert_true(app_config_legacy_commit(&m_cfg));
	zassert_equal(m_cfg.cap_gpio_mode, APP_CONFIG_GPIO_MODE_PIR);
	zassert_equal(m_cfg.cap_gpio_input_a, APP_CONFIG_GPIO_INPUT_DISABLED,
		      "PIR owns the pins: no stale input mode");
	zassert_equal(m_cfg.cap_gpio_input_b, APP_CONFIG_GPIO_INPUT_DISABLED);
}

ZTEST(config_legacy, test_buzzer_wins_over_inputs)
{
	zassert_ok(load("cap-buzzer", true));
	zassert_ok(load("cap-analog-b", true));

	zassert_true(app_config_legacy_commit(&m_cfg));
	zassert_equal(m_cfg.cap_gpio_mode, APP_CONFIG_GPIO_MODE_BUZZER);
	zassert_equal(m_cfg.cap_gpio_input_b, APP_CONFIG_GPIO_INPUT_DISABLED);
}

ZTEST(config_legacy, test_all_false_is_none_and_still_converted)
{
	/* A v1.4.0 unit stores every bool cap, false ones included: the keys must
	 * still be converted (and deleted), to cap-gpio-mode none. */
	m_cfg.cap_gpio_mode = APP_CONFIG_GPIO_MODE_PIR;
	zassert_ok(load("cap-input-a", false));
	zassert_ok(load("cap-pir-detector", false));

	zassert_true(app_config_legacy_commit(&m_cfg));
	zassert_equal(m_cfg.cap_gpio_mode, APP_CONFIG_GPIO_MODE_NONE);
}

ZTEST(config_legacy, test_cleanup_deletes_loaded_keys_once)
{
	zassert_ok(load("cap-input-b", true));
	zassert_ok(load("cap-buzzer", false));
	zassert_true(app_config_legacy_commit(&m_cfg));

	app_config_legacy_cleanup();
	zassert_equal(m_deleted_n, 2, "deleted %d keys", m_deleted_n);
	zassert_str_equal(m_deleted[0], "config/cap-input-b");
	zassert_str_equal(m_deleted[1], "config/cap-buzzer");

	/* Deleted keys are forgotten: no second conversion, no second delete. */
	zassert_false(app_config_legacy_commit(&m_cfg));
	app_config_legacy_cleanup();
	zassert_equal(m_deleted_n, 2);
}
