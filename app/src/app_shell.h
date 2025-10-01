/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SHELL_H_
#define APP_SHELL_H_

/* Zephyr includes */
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_shell_print_func_t)(const struct shell *shell);

int app_shell_cmd_bool(const struct shell *shell, size_t argc, char **argv,
                       bool *param, app_shell_print_func_t print_func);
int app_shell_cmd_int(const struct shell *shell, size_t argc, char **argv,
                      int *param, int min, int max, app_shell_print_func_t print_func);
int app_shell_cmd_uint(const struct shell *shell, size_t argc, char **argv,
                       unsigned int *param, unsigned int min, unsigned int max,
                       app_shell_print_func_t print_func);
int app_shell_cmd_float(const struct shell *shell, size_t argc, char **argv,
                        float *param, float min, float max, app_shell_print_func_t print_func);

extern const char APP_SHELL_MSG_INVALID_ARGS[];
extern const char APP_SHELL_MSG_INVALID_RANGE[];
extern const char APP_SHELL_MSG_INVALID_VALUE[];
extern const char APP_SHELL_MSG_CMD_SUCCESS[];

#ifdef __cplusplus
}
#endif

#endif /* APP_SHELL_H_ */
