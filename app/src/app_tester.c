/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_ds18b20.h"
#include "app_led.h"
#include "app_lrw.h"
#include "app_machine_probe.h"
#include "app_sensor.h"
#include "app_sht4x.h"

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(app_tester, LOG_LEVEL_DBG);

#define SHELL_PFX "tester"
#define SENSOR_CHECK_POLL_INTERVAL_MS 300 /* Poll interval for sensor check in milliseconds */

static struct k_work_delayable g_led_cycle_work;
static volatile bool g_led_cycle_stop = false;
static volatile int g_led_cycle_remaining = 0;
static volatile int g_led_cycle_state = 0; /* 0=red, 1=yellow, 2=green, 3=off */

static void led_cycle_work_handler(struct k_work *work)
{
	if (g_led_cycle_stop || g_led_cycle_remaining <= 0) {
		/* Turn off all LEDs and stop */
		app_led_set(APP_LED_CHANNEL_R, 0);
		app_led_set(APP_LED_CHANNEL_Y, 0);
		app_led_set(APP_LED_CHANNEL_G, 0);
		g_led_cycle_remaining = 0;
		return;
	}

	switch (g_led_cycle_state) {
	case 0: /* Red on */
		app_led_set(APP_LED_CHANNEL_R, 1);
		app_led_set(APP_LED_CHANNEL_Y, 0);
		app_led_set(APP_LED_CHANNEL_G, 0);
		g_led_cycle_state = 1;
		k_work_schedule(&g_led_cycle_work, K_MSEC(1000));
		break;

	case 1: /* Yellow on */
		app_led_set(APP_LED_CHANNEL_R, 0);
		app_led_set(APP_LED_CHANNEL_Y, 1);
		app_led_set(APP_LED_CHANNEL_G, 0);
		g_led_cycle_state = 2;
		k_work_schedule(&g_led_cycle_work, K_MSEC(1000));
		break;

	case 2: /* Green on */
		app_led_set(APP_LED_CHANNEL_R, 0);
		app_led_set(APP_LED_CHANNEL_Y, 0);
		app_led_set(APP_LED_CHANNEL_G, 1);
		g_led_cycle_state = 3;
		k_work_schedule(&g_led_cycle_work, K_MSEC(1000));
		break;

	case 3: /* All off */
		app_led_set(APP_LED_CHANNEL_R, 0);
		app_led_set(APP_LED_CHANNEL_Y, 0);
		app_led_set(APP_LED_CHANNEL_G, 0);
		g_led_cycle_remaining--;
		g_led_cycle_state = 0;

		if (g_led_cycle_remaining > 0 && !g_led_cycle_stop) {
			k_work_schedule(&g_led_cycle_work, K_MSEC(1000));
		}
		break;
	}
}

static void cmd_cycle_led(const struct shell *shell, size_t argc, char **argv)
{
	int cycles = 1; /* Default to 1 cycle if no parameter */

	if (argc >= 2) {
		cycles = atoi(argv[1]);
	}

	if (cycles == 0) {
		/* Stop immediately */
		g_led_cycle_stop = true;
		g_led_cycle_remaining = 0;
		k_work_cancel_delayable(&g_led_cycle_work);
		app_led_set(APP_LED_CHANNEL_R, 0);
		app_led_set(APP_LED_CHANNEL_Y, 0);
		app_led_set(APP_LED_CHANNEL_G, 0);
		shell_print(shell, SHELL_PFX " LED cycle stopped");
		return;
	}

	if (cycles < 1 || cycles > 99) {
		shell_error(shell, "Invalid cycle count. Use 1-99 or 0 to stop.");
		return;
	}

	/* Start new cycle */
	g_led_cycle_stop = false;
	g_led_cycle_remaining = cycles;
	g_led_cycle_state = 0;
	shell_print(shell, SHELL_PFX " Starting %d LED cycle(s)", cycles);

	k_work_schedule(&g_led_cycle_work, K_NO_WAIT);
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

static void cmd_print_serial_numbers(const struct shell *shell)
{
	int ret;
	int count;

#if defined(CONFIG_SHT4X)
	/* SHT40 (onboard temperature/humidity sensor) */
	uint32_t sht40_serial;
	ret = app_sht4x_read_serial(&sht40_serial);
	if (ret) {
		shell_error(shell, "Failed to read SHT40 serial: %d", ret);
	} else {
		shell_print(shell, "SHT40 serial: %u", sht40_serial);
	}
#endif /* defined(CONFIG_SHT4X) */

	/* DS18B20 sensors (T1, T2) */
	count = app_ds18b20_get_count();
	shell_print(shell, "DS18B20 count: %d", count);

	for (int i = 0; i < count; i++) {
		uint64_t serial_number;
		float temperature;

		ret = app_ds18b20_read(i, &serial_number, &temperature);
		if (ret) {
			shell_error(shell, "Failed to read DS18B20 sensor %d: %d", i, ret);
			continue;
		}

		shell_print(shell, "DS18B20[%d] serial: %llu", i, serial_number);
	}

	/* Machine Probe sensors (MP1, MP2) */
	count = app_machine_probe_get_count();
	shell_print(shell, "Machine Probe count: %d", count);

	for (int i = 0; i < count; i++) {
		uint64_t serial_number;
		float temperature;
		float humidity;

		ret = app_machine_probe_read_hygrometer(i, &serial_number, &temperature, &humidity);
		if (ret) {
			shell_error(shell, "Failed to read Machine Probe sensor %d: %d", i, ret);
			continue;
		}

		shell_print(shell, "Machine Probe[%d] serial: %llu", i, serial_number);

		/* Read SHT serial from machine probe */
		uint32_t sht_serial;
		ret = app_machine_probe_read_hygrometer_serial(i, &serial_number, &sht_serial);
		if (ret) {
			shell_error(shell, "Failed to read Machine Probe SHT serial %d: %d", i, ret);
		} else {
			shell_print(shell, "Machine Probe[%d] SHT serial: %u", i, sht_serial);
		}
	}
}

static void cmd_reset_sample(const struct shell *shell)
{
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	/* Reset all counters */
	g_app_sensor_data.motion_count = 0;
	g_app_sensor_data.hall_left_count = 0;
	g_app_sensor_data.hall_right_count = 0;
	g_app_sensor_data.input_a_count = 0;
	g_app_sensor_data.input_b_count = 0;

	k_mutex_unlock(&g_app_sensor_data_lock);

	shell_print(shell, SHELL_PFX " Sensor counters reset");
}

static void print_available_sensors(const struct shell *shell)
{
	shell_print(shell, "Available sensors:");
	shell_print(shell, "  ---Float sensors---:");
	shell_print(shell, "    voltage, temperature, humidity, illuminance");
	shell_print(shell, "    t1-temperature, t2-temperature");
	shell_print(shell, "    mp1-temperature, mp2-temperature");
	shell_print(shell, "    mp1-humidity, mp2-humidity");
	shell_print(shell, "    altitude, pressure");
	shell_print(shell, "  ---Integer sensors---:");
	shell_print(shell, "    orientation");
	shell_print(shell, "  ---Counter sensors---:");
	shell_print(shell, "    motion-count, hall-left-count, hall-right-count");
	shell_print(shell, "    input-a-count, input-b-count");
	shell_print(shell, "  ---Boolean sensors---:");
	shell_print(shell, "    mp1-is-tilt-alert, mp2-is-tilt-alert");
	shell_print(shell, "    hall-left-is-active, hall-right-is-active");
	shell_print(shell, "    input-a-is-active, input-b-is-active");
}

static void cmd_check_sensor(const struct shell *shell, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(shell, "Usage: tester sensors check <sensor-name> [timeout-sec]");
		print_available_sensors(shell);
		return;
	}

	const char *sensor_name = argv[1];
	int timeout_sec = 10; /* Default timeout */

	if (argc >= 3) {
		timeout_sec = atoi(argv[2]);
		if (timeout_sec <= 0) {
			shell_error(shell, "Invalid timeout value");
			return;
		}
	}

	shell_print(shell, SHELL_PFX " Monitoring '%s' for %d seconds...", sensor_name,
		    timeout_sec);

	/* Store initial value */
	float prev_float = 0.0f;
	uint32_t prev_uint32 = 0;
	int prev_int = 0;
	bool prev_bool = false;
	bool is_float = false;
	bool is_uint32 = false;
	bool is_int = false;
	bool is_bool = false;

	/* Determine sensor type and get initial value */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	if (strcmp(sensor_name, "voltage") == 0) {
		prev_float = g_app_sensor_data.voltage;
		is_float = true;
	} else if (strcmp(sensor_name, "temperature") == 0) {
		prev_float = g_app_sensor_data.temperature;
		is_float = true;
	} else if (strcmp(sensor_name, "humidity") == 0) {
		prev_float = g_app_sensor_data.humidity;
		is_float = true;
	} else if (strcmp(sensor_name, "illuminance") == 0) {
		prev_float = g_app_sensor_data.illuminance;
		is_float = true;
	} else if (strcmp(sensor_name, "t1-temperature") == 0) {
		prev_float = g_app_sensor_data.t1_temperature;
		is_float = true;
	} else if (strcmp(sensor_name, "t2-temperature") == 0) {
		prev_float = g_app_sensor_data.t2_temperature;
		is_float = true;
	} else if (strcmp(sensor_name, "mp1-temperature") == 0) {
		prev_float = g_app_sensor_data.mp1_temperature;
		is_float = true;
	} else if (strcmp(sensor_name, "mp2-temperature") == 0) {
		prev_float = g_app_sensor_data.mp2_temperature;
		is_float = true;
	} else if (strcmp(sensor_name, "mp1-humidity") == 0) {
		prev_float = g_app_sensor_data.mp1_humidity;
		is_float = true;
	} else if (strcmp(sensor_name, "mp2-humidity") == 0) {
		prev_float = g_app_sensor_data.mp2_humidity;
		is_float = true;
	} else if (strcmp(sensor_name, "altitude") == 0) {
		prev_float = g_app_sensor_data.altitude;
		is_float = true;
	} else if (strcmp(sensor_name, "pressure") == 0) {
		prev_float = g_app_sensor_data.pressure;
		is_float = true;
	} else if (strcmp(sensor_name, "orientation") == 0) {
		prev_int = g_app_sensor_data.orientation;
		is_int = true;
	} else if (strcmp(sensor_name, "motion-count") == 0) {
		prev_uint32 = g_app_sensor_data.motion_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "hall-left-count") == 0) {
		prev_uint32 = g_app_sensor_data.hall_left_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "hall-right-count") == 0) {
		prev_uint32 = g_app_sensor_data.hall_right_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "input-a-count") == 0) {
		prev_uint32 = g_app_sensor_data.input_a_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "input-b-count") == 0) {
		prev_uint32 = g_app_sensor_data.input_b_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "mp1-is-tilt-alert") == 0) {
		prev_bool = g_app_sensor_data.mp1_is_tilt_alert;
		is_bool = true;
	} else if (strcmp(sensor_name, "mp2-is-tilt-alert") == 0) {
		prev_bool = g_app_sensor_data.mp2_is_tilt_alert;
		is_bool = true;
	} else if (strcmp(sensor_name, "hall-left-is-active") == 0) {
		prev_bool = g_app_sensor_data.hall_left_is_active;
		is_bool = true;
	} else if (strcmp(sensor_name, "hall-right-is-active") == 0) {
		prev_bool = g_app_sensor_data.hall_right_is_active;
		is_bool = true;
	} else if (strcmp(sensor_name, "input-a-is-active") == 0) {
		prev_bool = g_app_sensor_data.input_a_is_active;
		is_bool = true;
	} else if (strcmp(sensor_name, "input-b-is-active") == 0) {
		prev_bool = g_app_sensor_data.input_b_is_active;
		is_bool = true;
	} else {
		k_mutex_unlock(&g_app_sensor_data_lock);
		shell_error(shell, "Unknown sensor: %s", sensor_name);
		print_available_sensors(shell);
		return;
	}

	k_mutex_unlock(&g_app_sensor_data_lock);

	/* Monitor for changes */
	int64_t start_time = k_uptime_get();
	int64_t timeout_ms = timeout_sec * 1000;

	while ((k_uptime_get() - start_time) < timeout_ms) {
		app_sensor_sample();

		k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

		bool changed = false;
		float curr_float = 0.0f;
		uint32_t curr_uint32 = 0;
		int curr_int = 0;
		bool curr_bool = false;

		/* Check for change based on sensor type */
		if (is_float) {
			if (strcmp(sensor_name, "voltage") == 0) {
				curr_float = g_app_sensor_data.voltage;
			} else if (strcmp(sensor_name, "temperature") == 0) {
				curr_float = g_app_sensor_data.temperature;
			} else if (strcmp(sensor_name, "humidity") == 0) {
				curr_float = g_app_sensor_data.humidity;
			} else if (strcmp(sensor_name, "illuminance") == 0) {
				curr_float = g_app_sensor_data.illuminance;
			} else if (strcmp(sensor_name, "t1-temperature") == 0) {
				curr_float = g_app_sensor_data.t1_temperature;
			} else if (strcmp(sensor_name, "t2-temperature") == 0) {
				curr_float = g_app_sensor_data.t2_temperature;
			} else if (strcmp(sensor_name, "mp1-temperature") == 0) {
				curr_float = g_app_sensor_data.mp1_temperature;
			} else if (strcmp(sensor_name, "mp2-temperature") == 0) {
				curr_float = g_app_sensor_data.mp2_temperature;
			} else if (strcmp(sensor_name, "mp1-humidity") == 0) {
				curr_float = g_app_sensor_data.mp1_humidity;
			} else if (strcmp(sensor_name, "mp2-humidity") == 0) {
				curr_float = g_app_sensor_data.mp2_humidity;
			} else if (strcmp(sensor_name, "altitude") == 0) {
				curr_float = g_app_sensor_data.altitude;
			} else if (strcmp(sensor_name, "pressure") == 0) {
				curr_float = g_app_sensor_data.pressure;
			}

			if (curr_float != prev_float) {
				shell_print(shell, SHELL_PFX " %s: %.2f", sensor_name,
					    (double)curr_float);
				prev_float = curr_float;
				changed = true;
			}
		} else if (is_uint32) {
			if (strcmp(sensor_name, "motion-count") == 0) {
				curr_uint32 = g_app_sensor_data.motion_count;
			} else if (strcmp(sensor_name, "hall-left-count") == 0) {
				curr_uint32 = g_app_sensor_data.hall_left_count;
			} else if (strcmp(sensor_name, "hall-right-count") == 0) {
				curr_uint32 = g_app_sensor_data.hall_right_count;
			} else if (strcmp(sensor_name, "input-a-count") == 0) {
				curr_uint32 = g_app_sensor_data.input_a_count;
			} else if (strcmp(sensor_name, "input-b-count") == 0) {
				curr_uint32 = g_app_sensor_data.input_b_count;
			}

			if (curr_uint32 != prev_uint32) {
				shell_print(shell, SHELL_PFX " %s: %u", sensor_name, curr_uint32);
				prev_uint32 = curr_uint32;
				changed = true;
			}
		} else if (is_int) {
			curr_int = g_app_sensor_data.orientation;

			if (curr_int != prev_int) {
				shell_print(shell, SHELL_PFX " %s: %d", sensor_name, curr_int);
				prev_int = curr_int;
				changed = true;
			}
		} else if (is_bool) {
			if (strcmp(sensor_name, "mp1-is-tilt-alert") == 0) {
				curr_bool = g_app_sensor_data.mp1_is_tilt_alert;
			} else if (strcmp(sensor_name, "mp2-is-tilt-alert") == 0) {
				curr_bool = g_app_sensor_data.mp2_is_tilt_alert;
			} else if (strcmp(sensor_name, "hall-left-is-active") == 0) {
				curr_bool = g_app_sensor_data.hall_left_is_active;
			} else if (strcmp(sensor_name, "hall-right-is-active") == 0) {
				curr_bool = g_app_sensor_data.hall_right_is_active;
			} else if (strcmp(sensor_name, "input-a-is-active") == 0) {
				curr_bool = g_app_sensor_data.input_a_is_active;
			} else if (strcmp(sensor_name, "input-b-is-active") == 0) {
				curr_bool = g_app_sensor_data.input_b_is_active;
			}

			if (curr_bool != prev_bool) {
				shell_print(shell, SHELL_PFX " %s: %s", sensor_name,
					    curr_bool ? "true" : "false");
				prev_bool = curr_bool;
				changed = true;
			}
		}

		k_mutex_unlock(&g_app_sensor_data_lock);

		k_sleep(K_MSEC(SENSOR_CHECK_POLL_INTERVAL_MS));
	}

	shell_print(shell, SHELL_PFX " Monitoring timeout");
}

static void cmd_print_sample(const struct shell *shell)
{
	app_sensor_sample();

	shell_print(shell, "orientation:              %d", g_app_sensor_data.orientation);
	shell_print(shell, "voltage:                  %.2f V", (double)g_app_sensor_data.voltage);
	shell_print(shell, "temperature:              %.2f C",
		    (double)g_app_sensor_data.temperature);
	shell_print(shell, "humidity:                 %.2f %%", (double)g_app_sensor_data.humidity);
	shell_print(shell, "illuminance:              %.2f lux",
		    (double)g_app_sensor_data.illuminance);
	shell_print(shell, "t1-temperature:           %.2f C",
		    (double)g_app_sensor_data.t1_temperature);
	shell_print(shell, "t2-temperature:           %.2f C",
		    (double)g_app_sensor_data.t2_temperature);
	shell_print(shell, "motion-count:             %u", g_app_sensor_data.motion_count);
	shell_print(shell, "altitude:                 %.2f m", (double)g_app_sensor_data.altitude);
	shell_print(shell, "pressure:                 %.2f Pa", (double)g_app_sensor_data.pressure);
	shell_print(shell, "mp1-temperature:          %.2f C",
		    (double)g_app_sensor_data.mp1_temperature);
	shell_print(shell, "mp2-temperature:          %.2f C",
		    (double)g_app_sensor_data.mp2_temperature);
	shell_print(shell, "mp1-humidity:             %.2f %%",
		    (double)g_app_sensor_data.mp1_humidity);
	shell_print(shell, "mp2-humidity:             %.2f %%",
		    (double)g_app_sensor_data.mp2_humidity);
	shell_print(shell, "mp1-is-tilt-alert:        %s",
		    g_app_sensor_data.mp1_is_tilt_alert ? "true" : "false");
	shell_print(shell, "mp2-is-tilt-alert:        %s",
		    g_app_sensor_data.mp2_is_tilt_alert ? "true" : "false");
	shell_print(shell, "hall-left-count:          %u", g_app_sensor_data.hall_left_count);
	shell_print(shell, "hall-right-count:         %u", g_app_sensor_data.hall_right_count);
	shell_print(shell, "hall-left-is-active:      %s",
		    g_app_sensor_data.hall_left_is_active ? "true" : "false");
	shell_print(shell, "hall-right-is-active:     %s",
		    g_app_sensor_data.hall_right_is_active ? "true" : "false");
	shell_print(shell, "input-a-count:            %u", g_app_sensor_data.input_a_count);
	shell_print(shell, "input-b-count:            %u", g_app_sensor_data.input_b_count);
	shell_print(shell, "input-a-is-active:        %s",
		    g_app_sensor_data.input_a_is_active ? "true" : "false");
	shell_print(shell, "input-b-is-active:        %s",
		    g_app_sensor_data.input_b_is_active ? "true" : "false");
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sensors,
	SHELL_CMD_ARG(sample, NULL, "Print all sensor values.", cmd_print_sample, 1, 0),
	SHELL_CMD_ARG(reset, NULL, "Reset sensor counters.", cmd_reset_sample, 1, 0),
	SHELL_CMD_ARG(serial, NULL, "Print sensor serial numbers.", cmd_print_serial_numbers, 1,
		      0),
	SHELL_CMD_ARG(check, NULL, "Monitor sensor for changes. Usage: check <sensor> [timeout]",
		      cmd_check_sensor, 2, 1),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_led,
	SHELL_CMD_ARG(cycle, NULL,
		      "Cycle LED (R/Y/G/off). Usage: cycle [count] (default=1, 0=stop, 1-99=cycles)",
		      cmd_cycle_led, 1, 1),
	SHELL_CMD_ARG(switch, NULL, "Switch LED channel (format red|yellow|green on|off).",
		      cmd_switch_led, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_test,
	SHELL_CMD(led, &sub_led, "LED commands.", NULL),
	SHELL_CMD(sensors, &sub_sensors, "Sensor commands.", NULL),
#if defined(CONFIG_LORAWAN)
	SHELL_CMD(lrw, &sub_lrw, "LoRaWAN commands.", NULL),
#endif /* defined(CONFIG_LORAWAN) */
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(tester, &sub_test, "Tester commands.", NULL);

static int app_tester_init(void)
{
	k_work_init_delayable(&g_led_cycle_work, led_cycle_work_handler);
	return 0;
}

SYS_INIT(app_tester_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
