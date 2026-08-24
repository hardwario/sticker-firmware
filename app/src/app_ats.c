/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_accel.h"
#include "app_log.h"
#include "app_buzzer.h"
#include "app_ds18b20.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_led.h"
#include "app_lrw.h"
#include "app_nfc.h"
#include "app_radio.h"
#if defined(CONFIG_APP_LORA_P2P)
#include "app_p2p.h"
#endif
#include "app_report.h"
#include "app_machine_probe.h"
#include "app_sensor.h"
#include "app_sht4x.h"
#include "app_ccm.h"
#include "app_cmd.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_history.h"
#include "app_w1_slots.h"

/* Nanopb (ats cmd history bench driver) */
#include <pb_decode.h>
#include <pb_encode.h>
#include "src/app_config.pb.h"

/* Zephyr includes */
#include <zephyr/drivers/hwinfo.h>
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

	/* #340 L13: snapshot the whole struct under the lock so every field below
	 * reflects one consistent point in time, instead of reading the live,
	 * unlocked global across ~20 fields — unlike every other reader here
	 * (the sensor-watch loop, app_sensor_sample()'s own writers), which take
	 * g_app_sensor_data_lock. A concurrent sample on another thread mid-write
	 * could otherwise produce a torn read (mixed pre/post-sample values). */
	struct app_sensor_data snapshot;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	snapshot = g_app_sensor_data;
	k_mutex_unlock(&g_app_sensor_data_lock);

	const struct app_sensor_data *d = &snapshot;

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

#if defined(CONFIG_LORAWAN) || defined(CONFIG_APP_LORA_P2P)

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
	case APP_LRW_STATE_DISABLED:
		return "DISABLED";
	default:
		return "UNKNOWN";
	}
}
#endif /* defined(CONFIG_LORAWAN) */

#if defined(CONFIG_APP_LORA_P2P)
static const char *p2p_state_to_str(enum p2p_link_state state)
{
	switch (state) {
	case P2P_LINK_UNPAIRED:
		return "UNPAIRED";
	case P2P_LINK_JOINING:
		return "JOINING";
	case P2P_LINK_PAIRED:
		return "PAIRED";
	default:
		return "UNKNOWN";
	}
}
#endif /* defined(CONFIG_APP_LORA_P2P) */

/* Universal across whichever stack radio_mode selected at boot (app_radio
 * facade, #118) -- the one status command a tester runs regardless of
 * build/config. LoRaWAN exposes rich link diagnostics (devaddr/fcnt/rssi/
 * margin/...); P2P only pairing/session state, since the raw-LoRa protocol
 * has no per-frame link-quality feedback (doc/p2p.md). */
static int cmd_radio_status(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if defined(CONFIG_APP_LORA_P2P)
	if (app_radio_get_kind() == APP_RADIO_P2P) {
		struct app_p2p_info info;

		app_p2p_get_info(&info);

		shell_print(shell, "kind: P2P");
		shell_print(shell, "state: %s", p2p_state_to_str(info.link_state));
		shell_print(shell, "net_id: %08x", info.net_id);
		shell_print(shell, "dev_addr: %04x", info.dev_addr);
		shell_print(shell, "rx1_delay: %u s", info.rx1_delay_s);
		shell_print(shell, "fcnt: %u", info.fcnt);
		shell_print(shell, "dev_nonce: %u", info.dev_nonce);
		shell_print(shell, "ack retry pending: %u", info.ack_retry_pending);
		shell_print(shell, "max payload: %u B", app_radio_get_max_payload());
		return 0;
	}
#endif /* defined(CONFIG_APP_LORA_P2P) */
#if defined(CONFIG_LORAWAN)
	struct app_lrw_info info;
	int ret = app_lrw_get_info(&info);

	if (ret) {
		shell_error(shell, "Failed to get LRW info: %d", ret);
		return ret;
	}

	shell_print(shell, "kind: LoRaWAN");
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
#else
	return -ENODEV;
#endif /* defined(CONFIG_LORAWAN) */
}

#if defined(CONFIG_LORAWAN)
static int cmd_lrw_check(const struct shell *shell, size_t argc, char **argv)
{
	app_lrw_force_link_check();
	app_report_trigger();
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

/* app_compose.c documents app_compose_ex()/app_compose_reset() as running
 * "solely on m_work_q" (owned by app_lrw.c) and mutating static state
 * (m_active/m_pending/m_snapshot/a static frame buffer) with no lock on that
 * assumption. Calling app_compose_ex() directly from the shell thread below
 * would race the real telemetry TX path, which composes on m_work_q too. So
 * each frame is composed as a work item on m_work_q (via
 * app_lrw_run_on_work_q()); the shell thread blocks on it (k_work_flush),
 * prints that one frame, and loops for the next — mirroring the original
 * per-frame streaming print, one m_work_q round-trip per frame instead of
 * buffering the whole multi-frame report (which a full report can run to
 * several KB of static RAM the debug build cannot spare). */
struct compose_result {
	uint8_t budget;
	uint8_t buf[243];
	size_t len;
	bool more;
	int ret; /* app_compose_ex() error, 0 on success */
};

static struct compose_result m_compose_result;
static struct k_work m_compose_work;

/* Runs on m_work_q — must not touch the shell pointer (not the caller's
 * thread by the time this executes). Composes exactly one frame per call;
 * app_compose_ex()'s own static state tracks progress across calls. */
static void compose_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct compose_result *res = &m_compose_result;

	res->ret = app_compose_ex(res->buf, sizeof(res->buf), &res->len, &res->more, res->budget);
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

	struct compose_result *res = &m_compose_result;
	bool more = true;
	int frame = 0;

	shell_print(shell, "Telemetry uplink (fPort 2, budget %u B):", budget);
	while (more) {
		res->budget = budget;

		int sret = app_lrw_run_on_work_q(&m_compose_work);
		if (sret < 0) {
			shell_error(shell, "compose submit failed: %d", sret);
			return sret;
		}

		struct k_work_sync sync;
		k_work_flush(&m_compose_work, &sync);

		if (res->ret) {
			shell_error(shell, "compose failed: %d", res->ret);
			return res->ret;
		}
		if (res->len == 0) {
			shell_print(shell, "  (nothing to report)");
			break;
		}
		shell_fprintf(shell, SHELL_NORMAL, "  frame %d (%zu B): ", frame++, res->len);
		for (size_t i = 0; i < res->len; i++) {
			shell_fprintf(shell, SHELL_NORMAL, "%02x", res->buf[i]);
		}
		shell_fprintf(shell, SHELL_NORMAL, "\n");
		more = res->more;
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
		shell_error(shell, "usage: radio lc ok|fail");
		return -EINVAL;
	}

	app_lrw_debug_inject_lc(ok);
	shell_print(shell, "Injected link-check %s (see 'ats radio status')", argv[1]);
	return 0;
}
#endif /* defined(CONFIG_LORAWAN) */

#if defined(CONFIG_APP_LORA_P2P)
/* Bench two-STICKER rig (doc/p2p.md §14): drives the reference receiver on a
 * second STICKER without a real gateway/central. Not registered when P2P
 * transport is compiled out (flash-tight debug builds, doc/p2p.md §11). */
static int cmd_p2p_listen(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	bool enable;

	if (strcmp(argv[1], "on") == 0) {
		enable = true;
	} else if (strcmp(argv[1], "off") == 0) {
		enable = false;
	} else {
		shell_error(shell, "usage: radio listen on|off");
		return -EINVAL;
	}

	int ret = app_p2p_listen(enable);
	if (ret) {
		shell_error(shell, "listen %s failed: %d", enable ? "on" : "off", ret);
		return ret;
	}
	shell_print(shell, "P2P listen %s", enable ? "ON" : "OFF");
	return 0;
}

/* Clears the persisted pairing and reboots -- the P2P analogue of
 * cmd_lrw_reset(), named to match P2P's own join/JoinRequest/JoinAccept
 * vocabulary rather than "pairing". Fixes a real gap (#118): `factory_reset`
 * does NOT clear P2P pairing (doc/p2p.md's claim otherwise is wrong, the
 * pairing subtree is never wired into app_settings_factory_reset()), so
 * this was previously only reachable via a whole-NVS `settings erase` or a
 * live GDB call. For a LIVE re-join that doesn't need a reboot, see the
 * top-level `join` command (app_radio_rejoin()) instead. */
static int cmd_radio_unjoin(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int ret = app_p2p_unjoin();

	if (ret) {
		shell_warn(shell, "Unjoin reported errors: %d", ret);
	}
	shell_print(shell, "P2P pairing cleared; rebooting...");
	k_sleep(K_MSEC(200)); /* let the shell flush */
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

/* Debug: sweep rx1_delay on the bench without a full re-join (doc/p2p.md
 * §13's own proposal for this, "same idiom as the existing ats radio lc
 * debug helpers"). */
static int cmd_radio_rx1_delay(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	int s = atoi(argv[1]);

	if (s < 0 || s > 255) {
		shell_error(shell, "rx1_delay must be 0..255 s");
		return -EINVAL;
	}

	app_p2p_debug_set_rx1_delay((uint8_t)s);
	shell_print(shell,
		    "rx1_delay override -> %d s (not persisted; a real JoinAccept "
		    "restores it)",
		    s);
	return 0;
}

/* Debug: exercise the confirmed-uplink retry path (doc/p2p.md §6) without a
 * real RF outage, same idea as `ats radio lc` for the LoRaWAN link-check FSM. */
static int cmd_radio_ack_drop(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	int n = atoi(argv[1]);

	if (n < 0) {
		shell_error(shell, "count must be >= 0");
		return -EINVAL;
	}

	app_p2p_debug_drop_acks((uint32_t)n);
	shell_print(shell, "Next %d confirmed-uplink Ack(s) will appear dropped", n);
	return 0;
}

/* Build (frame + encrypt) telemetry frames WITHOUT transmitting or advancing
 * the frame counter -- lets a bench tech inspect the exact bytes that would
 * go on air. The P2P counterpart of cmd_lrw_compose(); dispatched from the
 * same universal `compose` entry as cmd_radio_compose() below. */
static int cmd_p2p_compose(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	app_sensor_sample();

	bool more = true;
	int frame = 0;

	shell_print(shell, "P2P TELEMETRY frame preview:");
	while (more) {
		uint8_t buf[APP_P2P_FRAME_MAX_LEN];
		size_t len = 0;

		int ret = app_p2p_debug_compose(buf, sizeof(buf), &len, &more);

		if (ret == -ENOTCONN) {
			shell_error(shell, "not paired yet");
			return ret;
		}
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
#endif /* defined(CONFIG_APP_LORA_P2P) */

/* Universal `compose`, same dispatch idiom as cmd_radio_status(): a name
 * collision would otherwise be unavoidable on a dual-stack build, since both
 * cmd_lrw_compose() and cmd_p2p_compose() would need to register under the
 * same `ats radio compose` slot. */
static int cmd_radio_compose(const struct shell *shell, size_t argc, char **argv)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (app_radio_get_kind() == APP_RADIO_P2P) {
		return cmd_p2p_compose(shell, argc, argv);
	}
#endif /* defined(CONFIG_APP_LORA_P2P) */
#if defined(CONFIG_LORAWAN)
	return cmd_lrw_compose(shell, argc, argv);
#else
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_error(shell, "no radio stack compiled in");
	return -ENODEV;
#endif /* defined(CONFIG_LORAWAN) */
}

/* Single ATS entry point for both radio stacks (#118): `ats radio status`
 * and `ats radio compose` work no matter which one radio_mode picked at
 * boot; the rest are necessarily stack-specific (LoRaWAN link-check/NVM
 * reset vs. P2P's pairing/retry/timing bench helpers) and only compiled in
 * for the stack that provides them. */
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_radio,
	SHELL_CMD_ARG(status, NULL, "Print radio status (LoRaWAN or P2P).", cmd_radio_status, 1, 0),
	SHELL_CMD_ARG(compose, NULL,
		      "Build a telemetry frame without sending; dump hex. "
		      "Usage: compose [budget] (budget: LoRaWAN only)",
		      cmd_radio_compose, 1, 1),
#if defined(CONFIG_LORAWAN)
	SHELL_CMD_ARG(check, NULL, "Send data with link check.", cmd_lrw_check, 1, 0),
	SHELL_CMD_ARG(lc, NULL, "Debug: inject link-check result. Usage: lc ok|fail", cmd_lrw_lc, 2,
		      0),
	SHELL_CMD_ARG(reset, NULL, "Reset LoRaWAN frame counters + DevNonce (reboots).",
		      cmd_lrw_reset, 1, 0),
#endif /* defined(CONFIG_LORAWAN) */
#if defined(CONFIG_APP_LORA_P2P)
	SHELL_CMD_ARG(listen, NULL,
		      "Toggle continuous-RX reference receiver (bench two-STICKER rig, "
		      "doc/p2p.md §14). Usage: listen on|off",
		      cmd_p2p_listen, 2, 0),
	SHELL_CMD_ARG(unjoin, NULL,
		      "Clear P2P pairing state (reboots); NOT covered by factory_reset.",
		      cmd_radio_unjoin, 1, 0),
	SHELL_CMD_ARG(rx1_delay, NULL,
		      "Debug: override rx1_delay, not persisted. Usage: rx1_delay <seconds>",
		      cmd_radio_rx1_delay, 2, 0),
	SHELL_CMD_ARG(ack_drop, NULL,
		      "Debug: force the next N confirmed-uplink Acks to appear dropped. "
		      "Usage: ack_drop <count>",
		      cmd_radio_ack_drop, 2, 0),
#endif /* defined(CONFIG_APP_LORA_P2P) */
	SHELL_SUBCMD_SET_END);

#endif /* defined(CONFIG_LORAWAN) || defined(CONFIG_APP_LORA_P2P) */

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

	/* Mirror the response to the shell terminal as one hex line. The log channel
	 * can drop the INF hexdump under debug-level noise, so this is the reliable
	 * way to capture the Response for an over-the-wire round-trip check. */
	char resp_hex[2 * sizeof(out) + 1];
	bin2hex(out, out_len, resp_hex, sizeof(resp_hex));
	shell_print(sh, "RESP %s", resp_hex);

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

/* Bench driver for the NFC paged history read (#260): drives the same
 * client-side loop the Manager-App runs — build a req_history_page Command,
 * feed it through app_cmd_handle(NFC), decode the HistoryFrame, advance the
 * cursor by next_ord, repeat until has_more=false. Exercises reassembly order,
 * empty-window termination and the response fitting the NFC buffer, all without
 * a phone. Usage: history [<from_unix> [<to_unix>]] (default whole buffer). */
static int cmd_cmd_history(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t from = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : 0;
	uint32_t to = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : UINT32_MAX;

	uint32_t cursor = 0;
	uint32_t total_bytes = 0;
	uint32_t prev_cursor = UINT32_MAX;
	int page = 0;
	const int max_pages = 512; /* safety bound against a wedge/infinite loop */

	shell_print(sh, "NFC history read [from=%u to=%u], stored=%zu records", from, to,
		    app_history_count());

	/* Static, not on-stack: a Command + Response are ~1.2 KB together and the
	 * shell_rtt thread stack is small (app_cmd_handle also holds its own pair
	 * internally). The shell is serial, so reusing them across iterations is safe;
	 * memset re-zeroes each pass (== Command_init_zero / Response_init_zero). */
	static Command cmd;
	static Response resp;

	for (; page < max_pages; page++) {
		memset(&cmd, 0, sizeof(cmd));
		cmd.seq = (uint32_t)page;
		cmd.which_body = Command_req_history_page_tag;
		Command_ReqHistoryPage *rq = &cmd.body.req_history_page;
		if (from != 0) {
			rq->has_from_unix = true;
			rq->from_unix = from;
		}
		if (to != UINT32_MAX) {
			rq->has_to_unix = true;
			rq->to_unix = to;
		}
		rq->has_start_ord = true;
		rq->start_ord = cursor;

		static uint8_t in[32];
		pb_ostream_t os = pb_ostream_from_buffer(in, sizeof(in));
		if (!pb_encode(&os, Command_fields, &cmd)) {
			shell_error(sh, "encode Command: %s", PB_GET_ERROR(&os));
			return -EINVAL;
		}

		/* Holds one encoded HistoryFrame (<= 242 B samples + envelope); the NFC
		 * rsp record itself lives in the 512 B ST25DV user memory. */
		static uint8_t out[512];
		size_t out_len = 0;
		enum app_cmd_action action = APP_CMD_ACTION_NONE;
		int ret = app_cmd_handle(APP_CMD_TRANSPORT_NFC, in, os.bytes_written, out,
					 sizeof(out), &out_len, &action);
		if (ret) {
			shell_error(sh, "app_cmd_handle: %d", ret);
			return ret;
		}

		/* Strip the 1-byte APP_PROTO_VERSION prefix, then decode the Response. */
		if (out_len < 1) {
			shell_error(sh, "empty response");
			return -EIO;
		}
		memset(&resp, 0, sizeof(resp));
		pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
		if (!pb_decode(&is, Response_fields, &resp)) {
			shell_error(sh, "decode Response: %s", PB_GET_ERROR(&is));
			return -EIO;
		}

		if (resp.which_body == Response_error_tag) {
			shell_error(sh, "page %d: Error code %d", page, (int)resp.body.error.code);
			return -EIO;
		}
		if (resp.which_body != Response_history_frame_tag) {
			shell_error(sh, "page %d: unexpected body tag %d", page,
				    (int)resp.which_body);
			return -EIO;
		}

		const Response_HistoryFrame *hf = &resp.body.history_frame;
		shell_print(sh,
			    "  page %d: %u B samples, t0=%u present=0x%x interval=%u synced=%d "
			    "next_ord=%u has_more=%d (resp %zu B)",
			    page, (unsigned)hf->samples.size, hf->t0_unix, hf->present,
			    hf->interval_s, hf->time_synced, hf->next_ord, hf->has_more, out_len);
		total_bytes += hf->samples.size;

		if (!hf->has_more) {
			shell_print(sh, "done: %d page(s), %u sample byte(s)", page + 1,
				    total_bytes);
			return 0;
		}
		/* Guard against a non-advancing cursor that would loop forever. */
		if (hf->next_ord == prev_cursor || hf->next_ord == cursor) {
			shell_error(sh, "cursor did not advance (%u); aborting", hf->next_ord);
			return -EIO;
		}
		prev_cursor = cursor;
		cursor = hf->next_ord;
	}

	shell_warn(sh, "stopped at page cap (%d) — has_more never cleared", max_pages);
	return -EAGAIN;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_cmd,
	SHELL_CMD_ARG(lrw, NULL, "Inject over LoRaWAN transport. Usage: lrw <hex>", cmd_cmd_lrw, 2,
		      0),
	SHELL_CMD_ARG(nfc, NULL, "Inject over NFC transport. Usage: nfc <hex>", cmd_cmd_nfc, 2, 0),
	SHELL_CMD_ARG(history, NULL,
		      "Drive the NFC paged history read (#260). Usage: history [<from> [<to>]]",
		      cmd_cmd_history, 1, 2),
	SHELL_SUBCMD_SET_END);

/* On-target self-test for app_ccm over the STM32WL HW AES peripheral (#261). Runs
 * the published nfc_crypto golden vectors through the real hardware and checks the
 * output is byte-identical to the RFC 3610 / mbedTLS reference — this is the bench
 * check the HW register byte-order (KEYR big-endian, DINR/DOUTR little-endian +
 * DATATYPE byte-swap) is correct on silicon, which native_sim cannot exercise. The
 * key here is the PUBLISHED TEST key (00..0f), never a device secret. */
static int cmd_ccm_selftest(const struct shell *sh, size_t argc, char **argv)
{
	static const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
					0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
	static const uint8_t hdr[8] = {0, 0, 0, 0, 0, 0, 0, 1}; /* serial 0, counter 1 */
	const struct {
		const char *name;
		uint8_t dir;
		uint8_t pt[8];
		size_t pt_len;
		const char *want_ct_tag; /* golden ciphertext||tag16, hex */
	} vec[] = {
		{"request",
		 0x00,
		 {0x01, 0x08, 0x01, 0x22, 0x0a},
		 5,
		 "9e91d455b31b7eb34212a122abc064170eed1ed238"},
		{"response",
		 0x01,
		 {0x01, 0x08, 0x01, 0x12, 0x00},
		 5,
		 "d67a6dd3cce76a8bae20443086737ea584e195dfa0"},
	};
	int fails = 0;

	for (size_t i = 0; i < ARRAY_SIZE(vec); i++) {
		uint8_t nonce[9], ct[16], tag[16], got[64], rt[16];
		char got_hex[2 * 64 + 1];

		memcpy(nonce, hdr, 8);
		nonce[8] = vec[i].dir;

		int ret = app_ccm_encrypt_and_tag(key, nonce, 9, hdr, 8, vec[i].pt, vec[i].pt_len,
						  ct, tag, 16);
		if (ret) {
			shell_error(sh, "%s: encrypt failed %d", vec[i].name, ret);
			fails++;
			continue;
		}

		memcpy(got, ct, vec[i].pt_len);
		memcpy(&got[vec[i].pt_len], tag, 16);
		bin2hex(got, vec[i].pt_len + 16, got_hex, sizeof(got_hex));

		bool enc_ok = strcmp(got_hex, vec[i].want_ct_tag) == 0;

		/* Round-trip: HW AES must also decrypt+authenticate its own output. */
		ret = app_ccm_auth_decrypt(key, nonce, 9, hdr, 8, ct, vec[i].pt_len, tag, 16, rt);
		bool dec_ok = ret == 0 && memcmp(rt, vec[i].pt, vec[i].pt_len) == 0;

		shell_print(sh, "%-8s encrypt %s  roundtrip %s  got %s", vec[i].name,
			    enc_ok ? "PASS" : "FAIL", dec_ok ? "PASS" : "FAIL", got_hex);
		if (!enc_ok || !dec_ok) {
			fails++;
		}
	}

	shell_print(sh, "ccm selftest: %s (%d failure(s))", fails == 0 ? "PASS" : "FAIL", fails);
	return fails == 0 ? 0 : -EIO;
}

/* Debug control surface for the HW variant with a CMI-9605IC-0380T buzzer wired
 * directly across the former PIR pins (app_buzzer.c, #338). Only meaningful with
 * cap_buzzer=true — otherwise app_buzzer_init() was never called (either
 * cap_pir_detector owns the pins, or neither capability is on) and driving the
 * GPIOs here would race whatever else configured them. */
static int cmd_buzzer_off(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!g_app_config.cap_buzzer) {
		shell_error(sh, "cap_buzzer is not enabled");
		return -ENODEV;
	}

	/* Same as the remote kind-0 stop: silences immediately (even mid-beep/
	 * -melody), drops a queued request, and cancels a pending repeat cycle. */
	int ret = app_buzzer_play(APP_BUZZER_KIND_STOP);
	if (ret) {
		shell_error(sh, "app_buzzer_play(stop) failed: %d", ret);
		return ret;
	}

	return 0;
}

static int cmd_buzzer_beep(const struct shell *sh, size_t argc, char **argv)
{
	if (!g_app_config.cap_buzzer) {
		shell_error(sh, "cap_buzzer is not enabled");
		return -ENODEV;
	}

	unsigned long ms = strtoul(argv[1], NULL, 0);
	if (ms == 0 || ms > 5000) {
		shell_error(sh, "duration must be 1-5000 ms");
		return -EINVAL;
	}

	int ret = app_buzzer_on((uint32_t)ms);
	if (ret) {
		shell_error(sh, "app_buzzer_on failed: %d", ret);
		return ret;
	}

	return 0;
}

static int cmd_buzzer_play(const struct shell *sh, size_t argc, char **argv)
{
	if (!g_app_config.cap_buzzer) {
		shell_error(sh, "cap_buzzer is not enabled");
		return -ENODEV;
	}

	uint32_t kind;

	if (strcmp(argv[1], "info") == 0) {
		kind = APP_BUZZER_KIND_INFO;
	} else if (strcmp(argv[1], "warning") == 0) {
		kind = APP_BUZZER_KIND_WARNING;
	} else if (strcmp(argv[1], "alarm") == 0) {
		kind = APP_BUZZER_KIND_ALARM;
	} else {
		shell_error(sh, "unknown melody: %s (expected info|warning|alarm)", argv[1]);
		return -EINVAL;
	}

	unsigned long repeat_s = 0;
	if (argc >= 3) {
		repeat_s = strtoul(argv[2], NULL, 0);
		if (repeat_s > 999) {
			shell_error(sh, "repeat_s must be 0-999");
			return -EINVAL;
		}
	}

	int ret = app_buzzer_play_repeating(kind, (uint16_t)repeat_s);
	if (ret) {
		shell_error(sh, "app_buzzer_play_repeating(%s) failed: %d", argv[1], ret);
		return ret;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_buzzer,
	SHELL_CMD_ARG(off, NULL, "Stop the buzzer immediately (even mid-beep/-melody).",
		      cmd_buzzer_off, 1, 0),
	SHELL_CMD_ARG(beep, NULL, "Pulse the buzzer. Usage: beep <ms> (1-5000)", cmd_buzzer_beep, 2,
		      0),
	SHELL_CMD_ARG(play, NULL,
		      "Play a fixed melody. Usage: play <info|warning|alarm> [repeat_s 0-999]",
		      cmd_buzzer_play, 2, 1),
	SHELL_SUBCMD_SET_END);
#endif /* CONFIG_APP_CMD_DEBUG_SHELL */

/* Decode a hwinfo reset-cause bitmask (from app_cmd_info.reset_cause, read at
 * boot via hwinfo_get_reset_cause) into a human-readable shell line. On the
 * STM32WLE5 the driver only ever sets the flags below; several can be set at
 * once (a cold power-up typically asserts power-on + pin + brownout together). */
static void print_reset_cause(const struct shell *sh, uint32_t cause)
{
	static const struct {
		uint32_t flag;
		const char *name;
	} names[] = {
		{RESET_PIN, "pin"},
		{RESET_SOFTWARE, "software"},
		{RESET_BROWNOUT, "brownout"},
		{RESET_POR, "power-on"},
		{RESET_WATCHDOG, "watchdog"},
		{RESET_SECURITY, "security"},
		{RESET_LOW_POWER_WAKE, "low-power-wake"},
	};

	char buf[96];
	size_t len = 0;

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if (cause & names[i].flag) {
			int n = snprintf(buf + len, sizeof(buf) - len, "%s%s", len ? ", " : "",
					 names[i].name);
			if (n < 0 || (size_t)n >= sizeof(buf) - len) {
				break; /* truncated */
			}
			len += n;
		}
	}

	shell_print(sh, "Reset cause:   0x%08x (%s)", cause,
		    len ? buf : (cause ? "other" : "unknown"));
}

static void print_device_status(const struct shell *sh, uint32_t status)
{
	static const struct {
		uint32_t flag;
		const char *name;
	} names[] = {
		{APP_DEVICE_STATUS_ALARM_ANY, "alarm-any"},
		{APP_DEVICE_STATUS_ALARM_THRESHOLD, "alarm-threshold"},
		{APP_DEVICE_STATUS_ALARM_STATE, "alarm-state"},
		{APP_DEVICE_STATUS_ALARM_RATE, "alarm-rate"},
		{APP_DEVICE_STATUS_ALARM_NO_DATA, "alarm-no-data"},
		{APP_DEVICE_STATUS_ALARM_LOW_BATT, "alarm-low-battery"},
		{APP_DEVICE_STATUS_NFC_DOWN, "nfc-down"},
		{APP_DEVICE_STATUS_HISTORY_DOWN, "history-down"},
		{APP_DEVICE_STATUS_I2C_WEDGED, "i2c-wedged"},
		{APP_DEVICE_STATUS_TIME_UNSYNCED, "time-unsynced"},
		{APP_DEVICE_STATUS_LRW_DISABLED, "lrw-disabled"},
	};

	char buf[128];
	size_t len = 0;

	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if (status & names[i].flag) {
			int n = snprintf(buf + len, sizeof(buf) - len, "%s%s", len ? ", " : "",
					 names[i].name);
			if (n < 0 || (size_t)n >= sizeof(buf) - len) {
				break; /* truncated */
			}
			len += n;
		}
	}

	shell_print(sh, "Device status: 0x%08x (%s)", status, len ? buf : "ok");
}

/* Prints the same device info a GetInfo command returns over LoRaWAN, via the
 * shared app_cmd_get_info() (single source of truth). */
static int cmd_device_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	static const char *const build_type_name[] = {"main", "dev", "custom"};
	static const char *const lrw_state_name[] = {"idle",    "joining",   "healthy",
						     "warning", "reconnect", "disabled"};

	struct app_cmd_info info;
	app_cmd_get_info(&info);

	const char *bt = info.build_type < ARRAY_SIZE(build_type_name)
				 ? build_type_name[info.build_type]
				 : "unknown";
	const char *ls = info.lrw_state < ARRAY_SIZE(lrw_state_name)
				 ? lrw_state_name[info.lrw_state]
				 : "unknown";

	shell_print(sh, "FW version:    %u.%u.%u", info.fw_major, info.fw_minor, info.fw_patch);
	shell_print(sh, "Build type:    %s (%s)", bt, info.debug ? "debug" : "release");
	shell_print(sh, "Serial number: %u", info.serial_number);
	shell_print(sh, "Uptime:        %u s", info.uptime_s);
	shell_print(sh, "LRW state:     %s", ls);
	if (info.battery_mv) {
		shell_print(sh, "Battery:       %u mV", info.battery_mv);
	} else {
		shell_print(sh, "Battery:       unavailable");
	}

	print_reset_cause(sh, info.reset_cause);
	print_device_status(sh, info.device_status);

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

	/* Device identity keys (local shell only). secret-key is confidential; the
	 * claim-token (#170) is shown as "(unset)" until commissioned. */
	char hexbuf[2 * 16 + 1];

	bin2hex(info.dev_eui, sizeof(info.dev_eui), hexbuf, sizeof(hexbuf));
	shell_print(sh, "DevEUI:        %s", hexbuf);

	bin2hex(g_app_config.secret_key, sizeof(g_app_config.secret_key), hexbuf, sizeof(hexbuf));
	shell_print(sh, "Secret key:    %s", hexbuf);

	bool claim_set = false;
	for (size_t i = 0; i < sizeof(g_app_config.claim_token); i++) {
		if (g_app_config.claim_token[i] != 0) {
			claim_set = true;
			break;
		}
	}
	if (claim_set) {
		bin2hex(g_app_config.claim_token, sizeof(g_app_config.claim_token), hexbuf,
			sizeof(hexbuf));
		shell_print(sh, "Claim token:   %s", hexbuf);
	} else {
		shell_print(sh, "Claim token:   (unset)");
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

/* Re-arm the claim record (#247/#351): drops the clm latch back to UNSET, which
 * auto-advances to PENDING (clm reappears on NFC) on the next nfc_check_locked()
 * poll, as long as claim_token is still set. Non-destructive alternative to
 * app_settings_vendor_reset() for bench re-testing the claim flow. */
static int cmd_claim_active(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	app_nfc_clm_reset();

	bool claim_set = false;
	for (size_t i = 0; i < sizeof(g_app_config.claim_token); i++) {
		if (g_app_config.claim_token[i] != 0) {
			claim_set = true;
			break;
		}
	}
	shell_print(sh, "clm state -> unset (re-arms to pending on next NFC poll)");
	if (!claim_set) {
		shell_print(sh, "warning: claim_token is unset - clm record will NOT reappear "
				"until one is provisioned (`config claim-token <hex>`)");
	}
	return 0;
}

/* Force the claim window closed (#308) without a phone deleting the clm record. */
static int cmd_claim_done(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	app_nfc_clm_ack();
	shell_print(sh, "clm state -> consumed");
	return 0;
}

/* #247: show the claim-record lifecycle latch (debug/HW-test visibility). Moved
 * here from `nfc clm` (#351) so claim-lifecycle commands live in one place. */
static int cmd_claim_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	static const char *const names[] = {"unset", "pending", "consumed"};
	uint8_t state = app_nfc_clm_state_get();

	bool claim_set = false;
	for (size_t i = 0; i < sizeof(g_app_config.claim_token); i++) {
		if (g_app_config.claim_token[i] != 0) {
			claim_set = true;
			break;
		}
	}
	shell_print(sh, "clm state:   %s (%u)", state < ARRAY_SIZE(names) ? names[state] : "?",
		    state);
	shell_print(sh, "claim token: %s", claim_set ? "set" : "unset");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_claim,
	SHELL_CMD_ARG(active, NULL,
		      "Re-arm the claim record (clm UNSET; auto-pending next NFC poll).",
		      cmd_claim_active, 1, 0),
	SHELL_CMD_ARG(done, NULL, "Force the claim window closed (clm CONSUMED).", cmd_claim_done,
		      1, 0),
	SHELL_CMD_ARG(status, NULL, "Show claim-record (#247) lifecycle state.", cmd_claim_status,
		      1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_ats, SHELL_CMD(device, &sub_device, "Device info & control.", NULL),
	SHELL_CMD(claim, &sub_claim, "Claim-lifecycle test commands (#247/#351).", NULL),
	SHELL_CMD(led, &sub_led, "LED commands.", NULL),
	SHELL_CMD(sensors, &sub_sensors, "Sensor commands.", NULL),
#if defined(CONFIG_LORAWAN) || defined(CONFIG_APP_LORA_P2P)
	SHELL_CMD(radio, &sub_radio, "Radio (LoRaWAN/P2P) commands.", NULL),
#endif /* defined(CONFIG_LORAWAN) || defined(CONFIG_APP_LORA_P2P) */
#ifdef CONFIG_APP_CMD_DEBUG_SHELL
	SHELL_CMD(cmd, &sub_cmd, "Inject Command (protobuf hex).", NULL),
	SHELL_CMD(ccm, NULL, "app_ccm HW AES self-test (golden vectors).", cmd_ccm_selftest),
	SHELL_CMD(buzzer, &sub_buzzer, "Buzzer HW variant commands (#338).", NULL),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ats, &sub_ats, "Automated test system commands.", NULL);

static int app_ats_init(void)
{
	k_work_init_delayable(&g_led_cycle_work, led_cycle_work_handler);
#if defined(CONFIG_LORAWAN)
	k_work_init(&m_compose_work, compose_work_handler);
#endif /* defined(CONFIG_LORAWAN) */
	return 0;
}

SYS_INIT(app_ats_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
