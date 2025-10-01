/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_shell.h"

/* Zephyr includes */
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

const char APP_SHELL_MSG_INVALID_ARGS[] = "invalid number of arguments";
const char APP_SHELL_MSG_INVALID_RANGE[] = "invalid argument range";
const char APP_SHELL_MSG_INVALID_VALUE[] = "invalid argument value";
const char APP_SHELL_MSG_CMD_SUCCESS[] = "command succeeded";

int app_shell_cmd_bool(const struct shell *shell, size_t argc, char **argv,
                       bool *param, app_shell_print_func_t print_func)
{
	if (argc == 1) {
		if (print_func) {
			print_func(shell);
		}

		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_ARGS);
		return -EINVAL;
	}

	if (!strcmp(argv[1], "true")) {
		*param = true;
	} else if (!strcmp(argv[1], "false")) {
		*param = false;
	} else {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_VALUE);
		return -EINVAL;
	}

	shell_print(shell, "%s", APP_SHELL_MSG_CMD_SUCCESS);

	return 0;
}

int app_shell_cmd_int(const struct shell *shell, size_t argc, char **argv,
                      int *param, int min, int max, app_shell_print_func_t print_func)
{
	if (argc == 1) {
		if (print_func) {
			print_func(shell);
		}

		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_ARGS);
		return -EINVAL;
	}

	char *endptr;
	long value = strtol(argv[1], &endptr, 10);

	if (*endptr != '\0' || endptr == argv[1]) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_VALUE);
		return -EINVAL;
	}

	if (value < min || value > max) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_RANGE);
		return -EINVAL;
	}

	*param = (int)value;

	shell_print(shell, "%s", APP_SHELL_MSG_CMD_SUCCESS);

	return 0;
}

int app_shell_cmd_uint(const struct shell *shell, size_t argc, char **argv,
                       unsigned int *param, unsigned int min, unsigned int max,
                       app_shell_print_func_t print_func)
{
	if (argc == 1) {
		if (print_func) {
			print_func(shell);
		}

		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_ARGS);
		return -EINVAL;
	}

	char *endptr;
	unsigned long value = strtoul(argv[1], &endptr, 10);

	if (*endptr != '\0' || endptr == argv[1]) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_VALUE);
		return -EINVAL;
	}

	if (value < min || value > max) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_RANGE);
		return -EINVAL;
	}

	*param = (unsigned int)value;

	shell_print(shell, "%s", APP_SHELL_MSG_CMD_SUCCESS);

	return 0;
}

int app_shell_cmd_float(const struct shell *shell, size_t argc, char **argv,
                        float *param, float min, float max, app_shell_print_func_t print_func)
{
	if (argc == 1) {
		if (print_func) {
			print_func(shell);
		}

		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_ARGS);
		return -EINVAL;
	}

	char *endptr;
	float value = strtof(argv[1], &endptr);

	if (*endptr != '\0' || endptr == argv[1]) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_VALUE);
		return -EINVAL;
	}

	if (value < min || value > max) {
		shell_error(shell, "%s", APP_SHELL_MSG_INVALID_RANGE);
		return -EINVAL;
	}

	*param = value;

	shell_print(shell, "%s", APP_SHELL_MSG_CMD_SUCCESS);

	return 0;
}
