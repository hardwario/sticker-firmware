/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_led.h"
#include "app_sensor.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <stddef.h>
#include <string.h>

LOG_MODULE_REGISTER(app_tester, LOG_LEVEL_DBG);

#define SHELL_PFX "tester"

void cmd_cycle_led(void)
{
	app_led_set(APP_LED_CHANNEL_R, 1);
	k_sleep(K_MSEC(1000));
	app_led_set(APP_LED_CHANNEL_R, 0);
	k_sleep(K_MSEC(200));
	app_led_set(APP_LED_CHANNEL_Y, 1);
	k_sleep(K_MSEC(1000));
	app_led_set(APP_LED_CHANNEL_Y, 0);
	k_sleep(K_MSEC(200));
	app_led_set(APP_LED_CHANNEL_G, 1);
	k_sleep(K_MSEC(1000));
	app_led_set(APP_LED_CHANNEL_G, 0);
}

static void cmd_switch_led(const struct shell *shell, size_t argc, char **argv)
{
	enum app_led_channel channel;

	if (strcmp(argv[1], "red") == 0) {
		channel = APP_LED_CHANNEL_R;

	} else if (strcmp(argv[1], "green") == 0) {
		channel = APP_LED_CHANNEL_G;

	} else if (strcmp(argv[1], "yellow") == 0) {
		channel = APP_LED_CHANNEL_Y;

	} else {
		shell_error(shell, "invalid channel name");
		shell_help(shell);
		return;
	}

	if (strcmp(argv[2], "on") == 0) {
		app_led_set(channel, true);

	} else if (strcmp(argv[2], "off") == 0) {
		app_led_set(channel, false);

	} else {
		shell_error(shell, "invalid command");
		shell_help(shell);
	}
}

static void cmd_print_voltage(const struct shell *shell)
{
	app_sensor_sample();
	shell_print(shell, SHELL_PFX " voltage %.2f", (double)g_app_sensor_data.voltage);
}

static void cmd_print_orientation(const struct shell *shell)
{
	app_sensor_sample();
	shell_print(shell, SHELL_PFX " orientation %d", (int)g_app_sensor_data.orientation);
}

static void cmd_print_temperature(const struct shell *shell)
{
	app_sensor_sample();
	shell_print(shell, SHELL_PFX " temperature %.2f", (double)g_app_sensor_data.temperature);
}

static void cmd_print_humidity(const struct shell *shell)
{
	app_sensor_sample();
	shell_print(shell, SHELL_PFX " humidity %.2f", (double)g_app_sensor_data.humidity);
}

static void cmd_print_illuminance(const struct shell *shell)
{
	app_sensor_sample();
	shell_print(shell, SHELL_PFX " illuminance %.2f", (double)g_app_sensor_data.illuminance);
}

static void cmd_print_t1_temperature(const struct shell *shell)
{
	app_sensor_sample();
	shell_print(shell, SHELL_PFX " t1 temperature %.2f",
		    (double)g_app_sensor_data.t1_temperature);
}

static void cmd_print_t2_temperature(const struct shell *shell)
{
	app_sensor_sample();
	shell_print(shell, SHELL_PFX " t2 temperature %.2f",
		    (double)g_app_sensor_data.t2_temperature);
}

static void cmd_print_motion_count(const struct shell *shell)
{
	shell_print(shell, SHELL_PFX " motion count %d", (int)g_app_sensor_data.motion_count);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_test, SHELL_CMD_ARG(led_cycle, NULL, "Cycle LED (R/G/Y).", cmd_cycle_led, 1, 0),
	SHELL_CMD_ARG(led_switch, NULL, "Switch LED channel (format red|yellow|green on|off).",
		      cmd_switch_led, 3, 0),
	SHELL_CMD_ARG(voltage, NULL, "Print voltage.", cmd_print_voltage, 1, 0),
	SHELL_CMD_ARG(orientation, NULL, "Print orientation.", cmd_print_orientation, 1, 0),
	SHELL_CMD_ARG(temperature, NULL, "Print temperature.", cmd_print_temperature, 1, 0),
	SHELL_CMD_ARG(humidity, NULL, "Print humidity.", cmd_print_humidity, 1, 0),
	SHELL_CMD_ARG(illuminance, NULL, "Print illuminance.", cmd_print_illuminance, 1, 0),
	SHELL_CMD_ARG(t1_temperature, NULL, "Print T1 temperature.", cmd_print_t1_temperature, 1,
		      0),
	SHELL_CMD_ARG(t2_temperature, NULL, "Print T2 temperature.", cmd_print_t2_temperature, 1,
		      0),
	SHELL_CMD_ARG(motion, NULL, "Print number of PIR activations", cmd_print_motion_count, 1,
		      0),

	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(tester, &sub_test, "Tester commands.", NULL);
