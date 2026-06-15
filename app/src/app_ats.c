/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_accel.h"
#include "app_log.h"
#include "app_ds18b20.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_led.h"
#include "app_lrw.h"
#include "app_machine_probe.h"
#include "app_sensor.h"
#include "app_sht4x.h"
#include "app_cmd.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_w1_slots.h"

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(app_ats, LOG_LEVEL_DBG);

#define SHELL_PFX                     "ats"
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

static int cmd_print_serial_numbers(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

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
			shell_error(shell, "Failed to read Machine Probe SHT serial %d: %d", i,
				    ret);
		} else {
			shell_print(shell, "Machine Probe[%d] SHT serial: %u", i, sht_serial);
		}
	}

	return 0;
}

static int cmd_reset_sample(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	app_hall_reset_counts();
	app_input_reset_counts();

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	g_app_sensor_data.motion_count = 0;
	g_app_sensor_data.hall_left_count = 0;
	g_app_sensor_data.hall_right_count = 0;
	g_app_sensor_data.input_a_count = 0;
	g_app_sensor_data.input_b_count = 0;

	k_mutex_unlock(&g_app_sensor_data_lock);

	shell_print(shell, SHELL_PFX " Sensor counters reset");

	return 0;
}

static void print_available_sensors(const struct shell *shell)
{
	shell_print(shell, "Available sensors:");
	shell_print(shell, "  ---Float sensors---:");
	shell_print(shell, "    voltage, temperature, humidity, illuminance, altitude, pressure");
	shell_print(shell, "    sN-{temperature,humidity,illuminance,magnetic-field} (N=1..4)");
	shell_print(shell, "  ---Integer sensors---:");
	shell_print(shell, "    orientation");
	shell_print(shell, "  ---Counter sensors---:");
	shell_print(shell, "    motion-count, hall-left-count, hall-right-count");
	shell_print(shell, "    input-a-count, input-b-count");
	shell_print(shell, "  ---Boolean sensors---:");
	shell_print(shell, "    sN-tilt-alert (N=1..4)");
	shell_print(shell, "    hall-left-is-active, hall-right-is-active");
	shell_print(shell, "    input-a-is-active, input-b-is-active");
}

/* Sensor-value kind for the generic `sensors check` resolver. */
enum sval_kind {
	SVAL_NONE = 0,
	SVAL_FLOAT,
	SVAL_UINT,
	SVAL_INT,
	SVAL_BOOL,
};

/* Resolve a sensor name to its current value from g_app_sensor_data and return
 * its kind (SVAL_NONE if unknown). Caller must hold g_app_sensor_data_lock.
 * Slot sensors are `sN-<quantity>` (N=1..4 -> w1[N-1]), single source of truth
 * for both the initial snapshot and the monitor loop. */
static enum sval_kind resolve_sensor(const char *name, float *f, uint32_t *u, int *iv, bool *bv)
{
	const struct app_sensor_data *d = &g_app_sensor_data;

	if (strcmp(name, "voltage") == 0) {
		*f = d->voltage;
		return SVAL_FLOAT;
	} else if (strcmp(name, "temperature") == 0) {
		*f = d->temperature;
		return SVAL_FLOAT;
	} else if (strcmp(name, "humidity") == 0) {
		*f = d->humidity;
		return SVAL_FLOAT;
	} else if (strcmp(name, "illuminance") == 0) {
		*f = d->illuminance;
		return SVAL_FLOAT;
	} else if (strcmp(name, "altitude") == 0) {
		*f = d->altitude;
		return SVAL_FLOAT;
	} else if (strcmp(name, "pressure") == 0) {
		*f = d->pressure;
		return SVAL_FLOAT;
	} else if (strcmp(name, "orientation") == 0) {
		*iv = d->orientation;
		return SVAL_INT;
	} else if (strcmp(name, "motion-count") == 0) {
		*u = d->motion_count;
		return SVAL_UINT;
	} else if (strcmp(name, "hall-left-count") == 0) {
		*u = d->hall_left_count;
		return SVAL_UINT;
	} else if (strcmp(name, "hall-right-count") == 0) {
		*u = d->hall_right_count;
		return SVAL_UINT;
	} else if (strcmp(name, "input-a-count") == 0) {
		*u = d->input_a_count;
		return SVAL_UINT;
	} else if (strcmp(name, "input-b-count") == 0) {
		*u = d->input_b_count;
		return SVAL_UINT;
	} else if (strcmp(name, "hall-left-is-active") == 0) {
		*bv = d->hall_left_is_active;
		return SVAL_BOOL;
	} else if (strcmp(name, "hall-right-is-active") == 0) {
		*bv = d->hall_right_is_active;
		return SVAL_BOOL;
	} else if (strcmp(name, "input-a-is-active") == 0) {
		*bv = d->input_a_is_active;
		return SVAL_BOOL;
	} else if (strcmp(name, "input-b-is-active") == 0) {
		*bv = d->input_b_is_active;
		return SVAL_BOOL;
	}

	/* Slot sensors: sN-<quantity>, N=1..4 -> w1[N-1]. */
	if (name[0] == 's' && name[1] >= '1' && name[1] <= '4' && name[2] == '-') {
		const struct app_w1_slot_reading *s = &d->w1[name[1] - '1'];
		const char *q = name + 3;

		if (strcmp(q, "temperature") == 0) {
			*f = s->temperature;
			return SVAL_FLOAT;
		} else if (strcmp(q, "humidity") == 0) {
			*f = s->humidity;
			return SVAL_FLOAT;
		} else if (strcmp(q, "illuminance") == 0) {
			*f = s->illuminance;
			return SVAL_FLOAT;
		} else if (strcmp(q, "magnetic-field") == 0) {
			*f = s->magnetic_field;
			return SVAL_FLOAT;
		} else if (strcmp(q, "tilt-alert") == 0) {
			*bv = s->is_tilt_alert;
			return SVAL_BOOL;
		}
	}

	return SVAL_NONE;
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

	float pf = 0.0f, cf = 0.0f;
	uint32_t pu = 0, cu = 0;
	int pi = 0, ci = 0;
	bool pb = false, cb = false;

	/* Snapshot the initial value (and learn the sensor's kind). */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	enum sval_kind kind = resolve_sensor(sensor_name, &pf, &pu, &pi, &pb);
	k_mutex_unlock(&g_app_sensor_data_lock);

	if (kind == SVAL_NONE) {
		shell_error(shell, "Unknown sensor: %s", sensor_name);
		print_available_sensors(shell);
		return;
	}

	shell_print(shell, SHELL_PFX " Monitoring '%s' for %d seconds...", sensor_name,
		    timeout_sec);

	int64_t start_time = k_uptime_get();
	int64_t timeout_ms = timeout_sec * 1000;

	while ((k_uptime_get() - start_time) < timeout_ms) {
		app_sensor_sample();

		k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
		resolve_sensor(sensor_name, &cf, &cu, &ci, &cb);
		k_mutex_unlock(&g_app_sensor_data_lock);

		switch (kind) {
		case SVAL_FLOAT:
			if (cf != pf) {
				shell_print(shell, SHELL_PFX " %s: %s%d.%02d", sensor_name,
					    APP_FP2(cf));
				pf = cf;
			}
			break;
		case SVAL_UINT:
			if (cu != pu) {
				shell_print(shell, SHELL_PFX " %s: %u", sensor_name, cu);
				pu = cu;
			}
			break;
		case SVAL_INT:
			if (ci != pi) {
				shell_print(shell, SHELL_PFX " %s: %d", sensor_name, ci);
				pi = ci;
			}
			break;
		case SVAL_BOOL:
			if (cb != pb) {
				shell_print(shell, SHELL_PFX " %s: %s", sensor_name,
					    cb ? "true" : "false");
				pb = cb;
			}
			break;
		default:
			break;
		}

		k_sleep(K_MSEC(SENSOR_CHECK_POLL_INTERVAL_MS));
	}

	shell_print(shell, SHELL_PFX " Monitoring timeout");
}

/* Print "label: value unit" or "label: nan" when the reading is NaN
 * (absent/unsupported sensor), so the output mirrors the on-wire telemetry. */
static void print_float(const struct shell *shell, const char *label, float v, const char *unit)
{
	if (isnan(v)) {
		shell_print(shell, "  %-16s nan", label);
	} else {
		shell_print(shell, "  %-16s %s%d.%02d %s", label, APP_FP2(v), unit);
	}
}

static int cmd_print_sample(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	app_sensor_sample();

	const struct app_sensor_data *d = &g_app_sensor_data;

	/* On-device sensors + the device's own discrete inputs. */
	shell_print(shell, "== Device ==");
	print_float(shell, "voltage:", d->voltage, "V");
	print_float(shell, "temperature:", d->temperature, "C");
	print_float(shell, "humidity:", d->humidity, "%");
	print_float(shell, "pressure:", d->pressure, "Pa");
	print_float(shell, "altitude:", d->altitude, "m");
	print_float(shell, "illuminance:", d->illuminance, "lux");
	/* orientation + raw axes are meaningful only with the accelerometer enabled;
	 * read live (the onboard accel x/y/z are not cached in g_app_sensor_data). */
	if (g_app_config.cap_accelerometer) {
		float ax = NAN, ay = NAN, az = NAN;
		int ori = d->orientation;
		(void)app_accel_read(&ax, &ay, &az, &ori);
		shell_print(shell, "  %-16s %d", "orientation:", ori);
		shell_print(shell, "  %-16s x=%s%d.%02d y=%s%d.%02d z=%s%d.%02d m/s^2",
			    "accel:", APP_FP2(ax), APP_FP2(ay), APP_FP2(az));
	} else {
		shell_print(shell, "  %-16s nan", "orientation:");
		shell_print(shell, "  %-16s nan", "accel:");
	}
	shell_print(shell, "  %-16s %u", "motion-count:", d->motion_count);
	shell_print(shell, "  %-16s %u", "accel-motion:", d->accel_motion_count);
	shell_print(shell, "  %-16s count=%u active=%s", "hall-left:", d->hall_left_count,
		    d->hall_left_is_active ? "true" : "false");
	shell_print(shell, "  %-16s count=%u active=%s", "hall-right:", d->hall_right_count,
		    d->hall_right_is_active ? "true" : "false");
	shell_print(shell, "  %-16s count=%u active=%s", "input-a:", d->input_a_count,
		    d->input_a_is_active ? "true" : "false");
	shell_print(shell, "  %-16s count=%u active=%s", "input-b:", d->input_b_count,
		    d->input_b_is_active ? "true" : "false");

	/* 1-Wire ROM-bound slots s1..s4 — only the quantities the bound sensor
	 * actually provides are non-NaN (a thermometer shows temperature only; a
	 * machine probe shows the full cluster). */
	for (int i = 0; i < APP_W1_SLOT_COUNT; i++) {
		enum app_w1_slot_type type = app_w1_slot_get_type(i);
		const struct app_w1_slot_reading *s = &d->w1[i];

		if (type == APP_W1_SLOT_EMPTY) {
			shell_print(shell, "== s%d: empty ==", i + 1);
			continue;
		}
		shell_print(shell, "== s%d: %s%s ==", i + 1, app_w1_slot_type_name(type),
			    s->present ? "" : " (absent)");
		print_float(shell, "temperature:", s->temperature, "C");
		print_float(shell, "humidity:", s->humidity, "%");
		print_float(shell, "illuminance:", s->illuminance, "lux");
		print_float(shell, "magnetic-field:", s->magnetic_field, "mT");
		if (!isnan(s->accel_x) || !isnan(s->accel_y) || !isnan(s->accel_z)) {
			shell_print(shell, "  %-16s x=%s%d.%02d y=%s%d.%02d z=%s%d.%02d m/s^2",
				    "accel:", APP_FP2(s->accel_x), APP_FP2(s->accel_y),
				    APP_FP2(s->accel_z));
		}
		shell_print(shell, "  %-16s %s",
			    "tilt-alert:", s->is_tilt_alert ? "true" : "false");
	}

	return 0;
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
	shell_print(shell, "messages: %d", info.message_count);
	shell_print(shell, "healthy->warning: %d/%d", info.consecutive_lc_fail,
		    info.thresh_warning);
	shell_print(shell, "warning->healthy: %d/%d", info.consecutive_lc_ok, info.thresh_healthy);
	shell_print(shell, "warning->reconnect: %d/%d", info.warning_lc_fail_total,
		    info.thresh_reconnect);

	return 0;
}

static int cmd_lrw_check(const struct shell *shell, size_t argc, char **argv)
{
	app_lrw_send_with_link_check();
	shell_print(shell, "Sending data with link check request");
	return 0;
}

static int cmd_lrw_reset(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = app_lrw_reset_nvm();
	if (ret) {
		shell_warn(shell, "NVM clear reported errors: %d", ret);
	}
	shell_print(shell, "LoRaWAN frame counters + DevNonce reset; rebooting...");
	k_sleep(K_MSEC(200)); /* let the shell flush */
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

/* Build the telemetry uplink WITHOUT sending it and dump the raw fPort-2 bytes
 * (decodable with ttn.js) — lets the tester verify exactly what would go on the
 * wire. Samples first so the payload reflects the current sensors. Optional
 * budget arg picks the data-rate payload size (default 51 = EU868 DR0); needed
 * because the live budget is 0 before a join. */
static int cmd_lrw_compose(const struct shell *shell, size_t argc, char **argv)
{
	uint8_t budget = 51;

	if (argc >= 2) {
		int b = atoi(argv[1]);
		if (b < 1 || b > 242) {
			shell_error(shell, "budget must be 1..242 B");
			return -EINVAL;
		}
		budget = (uint8_t)b;
	}

	app_sensor_sample();

	uint8_t buf[243];
	size_t len = 0;
	bool more = true;
	int frame = 0;

	shell_print(shell, "Telemetry uplink (fPort 2, budget %u B):", budget);
	while (more) {
		int ret = app_compose_ex(buf, sizeof(buf), &len, &more, budget);
		if (ret) {
			shell_error(shell, "compose failed: %d", ret);
			return ret;
		}
		if (len == 0) {
			shell_print(shell, "  (nothing to report)");
			break;
		}
		shell_fprintf(shell, SHELL_NORMAL, "  frame %d (%zu B): ", frame++, len);
		for (size_t i = 0; i < len; i++) {
			shell_fprintf(shell, SHELL_NORMAL, "%02x", buf[i]);
		}
		shell_fprintf(shell, SHELL_NORMAL, "\n");
	}
	return 0;
}

/* Debug: drive the state machine by injecting a synthetic link-check outcome,
 * so HEALTHY->WARNING->RECONNECT->rejoin (and the late-LC-in-RECONNECT guard)
 * can be exercised on the bench without a real RF outage. */
static int cmd_lrw_lc(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	bool ok;

	if (strcmp(argv[1], "ok") == 0) {
		ok = true;
	} else if (strcmp(argv[1], "fail") == 0) {
		ok = false;
	} else {
		shell_error(shell, "usage: lrw lc ok|fail");
		return -EINVAL;
	}

	app_lrw_debug_inject_lc(ok);
	shell_print(shell, "Injected link-check %s (see 'ats lrw status')", argv[1]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_lrw, SHELL_CMD_ARG(status, NULL, "Print LoRaWAN status.", cmd_lrw_status, 1, 0),
	SHELL_CMD_ARG(check, NULL, "Send data with link check.", cmd_lrw_check, 1, 0),
	SHELL_CMD_ARG(lc, NULL, "Debug: inject link-check result. Usage: lc ok|fail", cmd_lrw_lc, 2,
		      0),
	SHELL_CMD_ARG(compose, NULL,
		      "Build telemetry uplink without sending; dump fPort-2 hex. "
		      "Usage: compose [budget]",
		      cmd_lrw_compose, 1, 1),
	SHELL_CMD_ARG(reset, NULL, "Reset LoRaWAN frame counters + DevNonce (reboots).",
		      cmd_lrw_reset, 1, 0),
	SHELL_SUBCMD_SET_END);
#endif /* defined(CONFIG_LORAWAN) */

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_sensors,
	SHELL_CMD_ARG(sample, NULL, "Print all sensor values.", cmd_print_sample, 1, 0),
	SHELL_CMD_ARG(reset, NULL, "Reset sensor counters.", cmd_reset_sample, 1, 0),
	SHELL_CMD_ARG(serial, NULL, "Print sensor serial numbers.", cmd_print_serial_numbers, 1, 0),
	SHELL_CMD_ARG(check, NULL, "Monitor sensor for changes. Usage: check <sensor> [timeout]",
		      cmd_check_sensor, 2, 1),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_led,
	SHELL_CMD_ARG(
		cycle, NULL,
		"Cycle LED (R/Y/G/off). Usage: cycle [count] (default=1, 0=stop, 1-99=cycles)",
		cmd_cycle_led, 1, 1),
	SHELL_CMD_ARG(switch, NULL, "Switch LED channel (format red|yellow|green on|off).",
		      cmd_switch_led, 3, 0),
	SHELL_SUBCMD_SET_END);

#ifdef CONFIG_APP_CMD_DEBUG_SHELL
static int cmd_cmd_inject(const struct shell *sh, enum app_cmd_transport transport, const char *hex)
{
	size_t hex_len = strlen(hex);

	if (hex_len == 0 || (hex_len % 2) != 0) {
		shell_error(sh, "Hex length must be non-zero and even");
		return -EINVAL;
	}

	static uint8_t in[64];
	if (hex_len / 2 > sizeof(in)) {
		shell_error(sh, "Hex too long: %zu B (max %zu)", hex_len / 2, sizeof(in));
		return -EMSGSIZE;
	}

	size_t in_len = hex2bin(hex, hex_len, in, sizeof(in));
	if (in_len == 0) {
		shell_error(sh, "Failed to parse hex");
		return -EINVAL;
	}

	static uint8_t out[64];
	size_t out_len = 0;
	enum app_cmd_action action = APP_CMD_ACTION_NONE;
	int ret = app_cmd_handle(transport, in, in_len, out, sizeof(out), &out_len, &action);
	if (ret) {
		shell_error(sh, "app_cmd_handle failed: %d", ret);
		return ret;
	}

	LOG_HEXDUMP_INF(out, out_len, "Response:");

	/* Don't reboot the bench from a shell inject — just report what an LRW
	 * downlink would trigger. Use `settings save` / `settings reset` to apply. */
	if (action != APP_CMD_ACTION_NONE) {
		shell_print(sh, "deferred action %d (not executed from shell inject)", (int)action);
	}

#if defined(CONFIG_LORAWAN)
	if (transport == APP_CMD_TRANSPORT_LRW && out_len > 0) {
		ret = app_lrw_queue_response(85, out, out_len);
		if (ret) {
			shell_warn(sh, "queue_response failed: %d", ret);
		}
	}
#endif

	return 0;
}

static int cmd_cmd_lrw(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_cmd_inject(sh, APP_CMD_TRANSPORT_LRW, argv[1]);
}

static int cmd_cmd_nfc(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_cmd_inject(sh, APP_CMD_TRANSPORT_NFC, argv[1]);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cmd,
	SHELL_CMD_ARG(lrw, NULL, "Inject over LoRaWAN transport. Usage: lrw <hex>", cmd_cmd_lrw, 2,
		      0),
	SHELL_CMD_ARG(nfc, NULL, "Inject over NFC transport. Usage: nfc <hex>", cmd_cmd_nfc, 2, 0),
	SHELL_SUBCMD_SET_END);
#endif /* CONFIG_APP_CMD_DEBUG_SHELL */

/* Prints the same device info a GetInfo command returns over LoRaWAN, via the
 * shared app_cmd_get_info() (single source of truth). */
static int cmd_device_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	static const char *const build_type_name[] = {"main", "dev", "custom"};

	struct app_cmd_info info;
	app_cmd_get_info(&info);

	const char *bt = info.build_type < ARRAY_SIZE(build_type_name)
				 ? build_type_name[info.build_type]
				 : "unknown";

	shell_print(sh, "FW version:    %u.%u.%u", info.fw_major, info.fw_minor, info.fw_patch);
	shell_print(sh, "Build type:    %s (%s)", bt, info.debug ? "debug" : "release");
	shell_print(sh, "Serial number: %u", info.serial_number);
	shell_print(sh, "Uptime:        %u s", info.uptime_s);

	if (info.has_unix_time) {
		time_t t = (time_t)info.unix_time;
		struct tm tm;
		gmtime_r(&t, &tm);
		shell_print(sh, "Wall clock:    %04d-%02d-%02d %02d:%02d:%02d UTC",
			    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
			    tm.tm_sec);
	} else {
		shell_print(sh, "Wall clock:    RTC not synced");
	}

	return 0;
}

/* App-level cold reboot from the shell. The same reboot is available remotely
 * via the `reboot` command over LoRaWAN/NFC (app_cmd.c, APP_CMD_ACTION_REBOOT). */
static int cmd_device_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Rebooting...");
	k_sleep(K_MSEC(200)); /* let the shell flush */
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_device,
	SHELL_CMD_ARG(info, NULL,
		      "Print device info (FW version, build type, serial, uptime, clock).",
		      cmd_device_info, 1, 0),
	SHELL_CMD_ARG(reboot, NULL, "Cold-reboot the device.", cmd_device_reboot, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ats,
			       SHELL_CMD(device, &sub_device, "Device info & control.", NULL),
			       SHELL_CMD(led, &sub_led, "LED commands.", NULL),
			       SHELL_CMD(sensors, &sub_sensors, "Sensor commands.", NULL),
#if defined(CONFIG_LORAWAN)
			       SHELL_CMD(lrw, &sub_lrw, "LoRaWAN commands.", NULL),
#endif /* defined(CONFIG_LORAWAN) */
#ifdef CONFIG_APP_CMD_DEBUG_SHELL
			       SHELL_CMD(cmd, &sub_cmd, "Inject Command (protobuf hex).", NULL),
#endif
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ats, &sub_ats, "Automated test system commands.", NULL);

static int app_ats_init(void)
{
	k_work_init_delayable(&g_led_cycle_work, led_cycle_work_handler);
	return 0;
}

SYS_INIT(app_ats_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
