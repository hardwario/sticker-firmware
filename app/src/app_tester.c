/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_led.h"
#include "app_lrw.h"
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

#if defined(CONFIG_LORAWAN)
static const char *lrw_state_to_str(enum app_lrw_state state)
{
	switch (state) {
	case APP_LRW_STATE_IDLE:
		return "IDLE";
	case APP_LRW_STATE_JOINING:
		return "JOINING";
	case APP_LRW_STATE_HEALTHY:
		return "HEALTHY";
	case APP_LRW_STATE_WARNING:
		return "WARNING";
	case APP_LRW_STATE_RECONNECT:
		return "RECONNECT";
	default:
		return "UNKNOWN";
	}
}

static int cmd_lrw_status(const struct shell *shell, size_t argc, char **argv)
{
	struct app_lrw_info info;
	int ret = app_lrw_get_info(&info);

	if (ret) {
		shell_error(shell, "Failed to get LRW info: %d", ret);
		return ret;
	}

	shell_print(shell, "state: %s", lrw_state_to_str(info.state));
	shell_print(shell, "devaddr: %08x", info.dev_addr);
	shell_print(shell, "fcnt up: %u", info.fcnt_up);
	shell_print(shell, "datarate: DR%d", info.datarate);
	shell_print(shell, "rssi: %d dBm", info.rssi);
	shell_print(shell, "snr: %d dB", info.snr);
	shell_print(shell, "margin: %u dB", info.margin);
	shell_print(shell, "gateways: %u", info.gw_count);
	shell_print(shell, "messages: %u", info.message_count);
	shell_print(shell, "healthy->warning: %u/%u",
		    info.consecutive_lc_fail, info.thresh_warning);
	shell_print(shell, "warning->healthy: %u/%u",
		    info.consecutive_lc_ok, info.thresh_healthy);
	shell_print(shell, "warning->reconnect: %u/%u",
		    info.warning_lc_fail_total, info.thresh_reconnect);

	return 0;
}

static int cmd_lrw_check(const struct shell *shell, size_t argc, char **argv)
{
	app_lrw_send_with_link_check();
	shell_print(shell, "Sending data with link check request");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lrw,
	SHELL_CMD_ARG(status, NULL, "Print LoRaWAN status.", cmd_lrw_status, 1, 0),
	SHELL_CMD_ARG(check, NULL, "Send data with link check.", cmd_lrw_check, 1, 0),
	SHELL_SUBCMD_SET_END);
#endif /* defined(CONFIG_LORAWAN) */

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
#if defined(CONFIG_LORAWAN)
	SHELL_CMD(lrw, &sub_lrw, "LoRaWAN commands.", NULL),
#endif /* defined(CONFIG_LORAWAN) */
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(tester, &sub_test, "Tester commands.", NULL);
