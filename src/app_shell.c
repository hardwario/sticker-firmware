/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: LicenseRef-HARDWARIO-5-Clause
 */

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(app_shell, LOG_LEVEL_DBG);

#define SETTINGS_PFX "application"

uint32_t serial_number = 0;

static int cmd_serialno(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc == 1) {
		shell_print(shell, "%010u", serial_number);
		return 0;
	}

	if (argc == 2) {
		size_t len = strlen(argv[1]);

		if (len != 10) {
			shell_error(shell, "invalid format");
			return -EINVAL;
		}

		for (size_t i = 0; i < len; i++) {
			if (!isdigit((int)argv[1][i])) {
				shell_error(shell, "invalid format");
				return -EINVAL;
			}
		}

		serial_number = strtoul(argv[1], NULL, 10);

		ret = settings_save();
		if (ret) {
			LOG_ERR("Call `settings_save` failed: %d", ret);
			return ret;
		}

		return 0;
	}

	shell_help(shell);

	return -EINVAL;
}

SHELL_CMD_REGISTER(serialno, NULL, "Get/set device serial number.", cmd_serialno);

static int h_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	int ret;
	const char *next;

#define SETTINGS_SET(_key, _var, _size)                                                            \
	do {                                                                                       \
		if (settings_name_steq(key, _key, &next) && !next) {                               \
			if (len != _size) {                                                        \
				return -EINVAL;                                                    \
			}                                                                          \
                                                                                                   \
			ret = read_cb(cb_arg, _var, len);                                          \
                                                                                                   \
			if (ret < 0) {                                                             \
				LOG_ERR("Call `read_cb` failed: %d", ret);                         \
				return ret;                                                        \
			}                                                                          \
                                                                                                   \
			return 0;                                                                  \
		}                                                                                  \
	} while (0)

	SETTINGS_SET("serialno", &serial_number, sizeof(serial_number));

#undef SETTINGS_SET

	return -ENOENT;
}

static int h_commit(void)
{
	LOG_DBG("Loaded settings in full");
	return 0;
}

static int h_export(int (*export_func)(const char *name, const void *val, size_t val_len))
{
#define EXPORT_FUNC(_key, _var, _size)                                                             \
	do {                                                                                       \
		(void)export_func(SETTINGS_PFX "/" _key, _var, _size);                             \
	} while (0)

	EXPORT_FUNC("serialno", &serial_number, sizeof(serial_number));

#undef EXPORT_FUNC

	return 0;
}

static int init(void)
{
	int ret;

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("Call `settings_subsys_init` failed: %d", ret);
		return ret;
	}

	static struct settings_handler sh = {
		.name = SETTINGS_PFX,
		.h_set = h_set,
		.h_commit = h_commit,
		.h_export = h_export,
	};

	ret = settings_register(&sh);
	if (ret) {
		LOG_ERR("Call `settings_register` failed: %d", ret);
		return ret;
	}

	ret = settings_load_subtree(SETTINGS_PFX);
	if (ret) {
		LOG_ERR("Call `settings_load_subtree` failed: %d", ret);
		return ret;
	}

	return 0;
}

SYS_INIT(init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
