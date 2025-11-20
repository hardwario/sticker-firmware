/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_ds18b20.h"
#include "app_led.h"
#include "app_machine_probe.h"
#include "app_sensor.h"

/* Zephyr includes */
#include <zephyr/drivers/hwinfo.h>
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

static void cmd_print_hw_deveui(const struct shell *shell)
{
	uint8_t dev_id[16];
	ssize_t length;

	/* Get hardware device ID (UID) */
	length = hwinfo_get_device_id(dev_id, sizeof(dev_id));

	if (length < 0) {
		shell_error(shell, "Failed to read hardware device ID: %d", length);
		return;
	}

	shell_print(shell, SHELL_PFX " Hardware Device ID length: %d bytes", length);

	/* Display full device ID in hex */
	shell_print(shell, SHELL_PFX " Hardware Device ID (hex): ");
	for (int i = 0; i < length; i++) {
		shell_fprintf(shell, SHELL_NORMAL, "%02x", dev_id[i]);
	}
	shell_fprintf(shell, SHELL_NORMAL, "\n");

	/* Derive 8-byte DevEUI from device ID
	 * For STM32, UID is 96 bits (12 bytes), we take first 8 bytes
	 */
	uint8_t hw_deveui[8];
	int deveui_len = MIN(8, length);
	memcpy(hw_deveui, dev_id, deveui_len);

	/* If device ID is shorter than 8 bytes, pad with zeros */
	if (deveui_len < 8) {
		memset(hw_deveui + deveui_len, 0, 8 - deveui_len);
	}

	/* Print as decimal (8 bytes as uint64) */
	uint64_t deveui = 0;
	for (int i = 0; i < 8; i++) {
		deveui = (deveui << 8) | hw_deveui[i];
	}

	shell_print(shell, SHELL_PFX " Hardware DevEUI (decimal): %llu", deveui);

	/* Also print as hex */
	shell_print(shell, SHELL_PFX " Hardware DevEUI (hex): "
		    "%02x%02x%02x%02x%02x%02x%02x%02x",
		    hw_deveui[0], hw_deveui[1], hw_deveui[2], hw_deveui[3],
		    hw_deveui[4], hw_deveui[5], hw_deveui[6], hw_deveui[7]);
}

static void cmd_print_serial_numbers(const struct shell *shell)
{
	int ret;
	int count;

	/* DS18B20 sensors (T1, T2) */
	count = app_ds18b20_get_count();
	shell_print(shell, SHELL_PFX " DS18B20 count: %d", count);

	for (int i = 0; i < count; i++) {
		uint64_t serial_number;
		float temperature;

		ret = app_ds18b20_read(i, &serial_number, &temperature);
		if (ret) {
			shell_error(shell, "Failed to read DS18B20 sensor %d: %d", i, ret);
			continue;
		}

		shell_print(shell, SHELL_PFX " DS18B20[%d] serial: %llu", i, serial_number);
	}

	/* Machine Probe sensors (MP1, MP2) */
	count = app_machine_probe_get_count();
	shell_print(shell, SHELL_PFX " Machine Probe count: %d", count);

	for (int i = 0; i < count; i++) {
		uint64_t serial_number;
		float temperature;
		float humidity;

		ret = app_machine_probe_read_hygrometer(i, &serial_number, &temperature, &humidity);
		if (ret) {
			shell_error(shell, "Failed to read Machine Probe sensor %d: %d", i, ret);
			continue;
		}

		shell_print(shell, SHELL_PFX " Machine Probe[%d] serial: %llu", i, serial_number);
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
	shell_print(shell, "    t1_temperature, t2_temperature");
	shell_print(shell, "    mp1_temperature, mp2_temperature");
	shell_print(shell, "    mp1_humidity, mp2_humidity");
	shell_print(shell, "    altitude, pressure");
	shell_print(shell, "  ---Integer sensors---:");
	shell_print(shell, "    orientation");
	shell_print(shell, "  ---Counter sensors---:");
	shell_print(shell, "    motion_count, hall_left_count, hall_right_count");
	shell_print(shell, "    input_a_count, input_b_count");
	shell_print(shell, "  ---Boolean sensors---:");
	shell_print(shell, "    mp1_is_tilt_alert, mp2_is_tilt_alert");
	shell_print(shell, "    hall_left_is_active, hall_right_is_active");
	shell_print(shell, "    input_a_is_active, input_b_is_active");
}

static void cmd_check_sensor(const struct shell *shell, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(shell, "Usage: tester sensors check <sensor_name> [timeout_sec]");
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
	} else if (strcmp(sensor_name, "t1_temperature") == 0) {
		prev_float = g_app_sensor_data.t1_temperature;
		is_float = true;
	} else if (strcmp(sensor_name, "t2_temperature") == 0) {
		prev_float = g_app_sensor_data.t2_temperature;
		is_float = true;
	} else if (strcmp(sensor_name, "mp1_temperature") == 0) {
		prev_float = g_app_sensor_data.mp1_temperature;
		is_float = true;
	} else if (strcmp(sensor_name, "mp2_temperature") == 0) {
		prev_float = g_app_sensor_data.mp2_temperature;
		is_float = true;
	} else if (strcmp(sensor_name, "mp1_humidity") == 0) {
		prev_float = g_app_sensor_data.mp1_humidity;
		is_float = true;
	} else if (strcmp(sensor_name, "mp2_humidity") == 0) {
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
	} else if (strcmp(sensor_name, "motion_count") == 0) {
		prev_uint32 = g_app_sensor_data.motion_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "hall_left_count") == 0) {
		prev_uint32 = g_app_sensor_data.hall_left_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "hall_right_count") == 0) {
		prev_uint32 = g_app_sensor_data.hall_right_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "input_a_count") == 0) {
		prev_uint32 = g_app_sensor_data.input_a_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "input_b_count") == 0) {
		prev_uint32 = g_app_sensor_data.input_b_count;
		is_uint32 = true;
	} else if (strcmp(sensor_name, "mp1_is_tilt_alert") == 0) {
		prev_bool = g_app_sensor_data.mp1_is_tilt_alert;
		is_bool = true;
	} else if (strcmp(sensor_name, "mp2_is_tilt_alert") == 0) {
		prev_bool = g_app_sensor_data.mp2_is_tilt_alert;
		is_bool = true;
	} else if (strcmp(sensor_name, "hall_left_is_active") == 0) {
		prev_bool = g_app_sensor_data.hall_left_is_active;
		is_bool = true;
	} else if (strcmp(sensor_name, "hall_right_is_active") == 0) {
		prev_bool = g_app_sensor_data.hall_right_is_active;
		is_bool = true;
	} else if (strcmp(sensor_name, "input_a_is_active") == 0) {
		prev_bool = g_app_sensor_data.input_a_is_active;
		is_bool = true;
	} else if (strcmp(sensor_name, "input_b_is_active") == 0) {
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
			} else if (strcmp(sensor_name, "t1_temperature") == 0) {
				curr_float = g_app_sensor_data.t1_temperature;
			} else if (strcmp(sensor_name, "t2_temperature") == 0) {
				curr_float = g_app_sensor_data.t2_temperature;
			} else if (strcmp(sensor_name, "mp1_temperature") == 0) {
				curr_float = g_app_sensor_data.mp1_temperature;
			} else if (strcmp(sensor_name, "mp2_temperature") == 0) {
				curr_float = g_app_sensor_data.mp2_temperature;
			} else if (strcmp(sensor_name, "mp1_humidity") == 0) {
				curr_float = g_app_sensor_data.mp1_humidity;
			} else if (strcmp(sensor_name, "mp2_humidity") == 0) {
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
			if (strcmp(sensor_name, "motion_count") == 0) {
				curr_uint32 = g_app_sensor_data.motion_count;
			} else if (strcmp(sensor_name, "hall_left_count") == 0) {
				curr_uint32 = g_app_sensor_data.hall_left_count;
			} else if (strcmp(sensor_name, "hall_right_count") == 0) {
				curr_uint32 = g_app_sensor_data.hall_right_count;
			} else if (strcmp(sensor_name, "input_a_count") == 0) {
				curr_uint32 = g_app_sensor_data.input_a_count;
			} else if (strcmp(sensor_name, "input_b_count") == 0) {
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
			if (strcmp(sensor_name, "mp1_is_tilt_alert") == 0) {
				curr_bool = g_app_sensor_data.mp1_is_tilt_alert;
			} else if (strcmp(sensor_name, "mp2_is_tilt_alert") == 0) {
				curr_bool = g_app_sensor_data.mp2_is_tilt_alert;
			} else if (strcmp(sensor_name, "hall_left_is_active") == 0) {
				curr_bool = g_app_sensor_data.hall_left_is_active;
			} else if (strcmp(sensor_name, "hall_right_is_active") == 0) {
				curr_bool = g_app_sensor_data.hall_right_is_active;
			} else if (strcmp(sensor_name, "input_a_is_active") == 0) {
				curr_bool = g_app_sensor_data.input_a_is_active;
			} else if (strcmp(sensor_name, "input_b_is_active") == 0) {
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

	shell_print(shell, SHELL_PFX " orientation:              %d",
		    g_app_sensor_data.orientation);
	shell_print(shell, SHELL_PFX " voltage:                  %.2f V",
		    (double)g_app_sensor_data.voltage);
	shell_print(shell, SHELL_PFX " temperature:              %.2f C",
		    (double)g_app_sensor_data.temperature);
	shell_print(shell, SHELL_PFX " humidity:                 %.2f %%",
		    (double)g_app_sensor_data.humidity);
	shell_print(shell, SHELL_PFX " illuminance:              %.2f lux",
		    (double)g_app_sensor_data.illuminance);
	shell_print(shell, SHELL_PFX " t1_temperature:           %.2f C",
		    (double)g_app_sensor_data.t1_temperature);
	shell_print(shell, SHELL_PFX " t2_temperature:           %.2f C",
		    (double)g_app_sensor_data.t2_temperature);
	shell_print(shell, SHELL_PFX " motion_count:             %u",
		    g_app_sensor_data.motion_count);
	shell_print(shell, SHELL_PFX " altitude:                 %.2f m",
		    (double)g_app_sensor_data.altitude);
	shell_print(shell, SHELL_PFX " pressure:                 %.2f Pa",
		    (double)g_app_sensor_data.pressure);
	shell_print(shell, SHELL_PFX " machine_probe_temp_1:     %.2f C",
		    (double)g_app_sensor_data.mp1_temperature);
	shell_print(shell, SHELL_PFX " machine_probe_temp_2:     %.2f C",
		    (double)g_app_sensor_data.mp2_temperature);
	shell_print(shell, SHELL_PFX " machine_probe_humidity_1: %.2f %%",
		    (double)g_app_sensor_data.mp1_humidity);
	shell_print(shell, SHELL_PFX " machine_probe_humidity_2: %.2f %%",
		    (double)g_app_sensor_data.mp2_humidity);
	shell_print(shell, SHELL_PFX " machine_probe_tilt_1:     %s",
		    g_app_sensor_data.mp1_is_tilt_alert ? "true" : "false");
	shell_print(shell, SHELL_PFX " machine_probe_tilt_2:     %s",
		    g_app_sensor_data.mp2_is_tilt_alert ? "true" : "false");
	shell_print(shell, SHELL_PFX " hall_left_count:          %u",
		    g_app_sensor_data.hall_left_count);
	shell_print(shell, SHELL_PFX " hall_right_count:         %u",
		    g_app_sensor_data.hall_right_count);
	shell_print(shell, SHELL_PFX " hall_left_active:         %s",
		    g_app_sensor_data.hall_left_is_active ? "true" : "false");
	shell_print(shell, SHELL_PFX " hall_right_active:        %s",
		    g_app_sensor_data.hall_right_is_active ? "true" : "false");
	shell_print(shell, SHELL_PFX " input_a_count:            %u",
		    g_app_sensor_data.input_a_count);
	shell_print(shell, SHELL_PFX " input_b_count:            %u",
		    g_app_sensor_data.input_b_count);
	shell_print(shell, SHELL_PFX " input_a_active:           %s",
		    g_app_sensor_data.input_a_is_active ? "true" : "false");
	shell_print(shell, SHELL_PFX " input_b_active:           %s",
		    g_app_sensor_data.input_b_is_active ? "true" : "false");
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sensors,
	SHELL_CMD_ARG(sample, NULL, "Print all sensor values.", cmd_print_sample, 1, 0),
	SHELL_CMD_ARG(reset, NULL, "Reset sensor counters.", cmd_reset_sample, 1, 0),
	SHELL_CMD_ARG(serial, NULL, "Print sensor serial numbers.", cmd_print_serial_numbers, 1,
		      0),
	SHELL_CMD_ARG(check, NULL, "Monitor sensor for changes. Usage: check <sensor> [timeout]",
		      cmd_check_sensor, 2, 1),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_test,
	SHELL_CMD_ARG(led_cycle, NULL,
		      "Cycle LED (R/Y/G/off). Usage: led_cycle [count] (default=1, 0=stop, 1-99=cycles)",
		      cmd_cycle_led, 1, 1),
	SHELL_CMD_ARG(led_switch, NULL, "Switch LED channel (format red|yellow|green on|off).",
		      cmd_switch_led, 3, 0),
	SHELL_CMD(sensors, &sub_sensors, "Sensor commands.", NULL),
	SHELL_CMD_ARG(hw_deveui, NULL, "Print hardware DevEUI from chip UID.",
		      cmd_print_hw_deveui, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(tester, &sub_test, "Tester commands.", NULL);

static int app_tester_init(void)
{
	k_work_init_delayable(&g_led_cycle_work, led_cycle_work_handler);
	return 0;
}

SYS_INIT(app_tester_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
