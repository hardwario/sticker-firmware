/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_calibration.h"
#include "app_config.h"
#include "app_ds18b20.h"
#include "app_hall.h"
#include "app_led.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_machine_probe.h"
#include "app_sensor.h"
#include "app_sht40.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <math.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_calibration, LOG_LEVEL_DBG);

/* Module state */
static bool m_calibration_active;
static bool m_hall_activated; /* true if activated via Hall gesture (has timeout) */

/* Hall gesture tracking */
static int64_t m_hall_left_active_since;
static int64_t m_hall_right_active_since;
static bool m_hall_left_was_active;
static bool m_hall_right_was_active;

static void read_sensors_rtt(void)
{
	int ret;

	/* SHT40 - on-board temperature/humidity sensor */
	float temperature = NAN;
	float humidity = NAN;
	uint32_t sht40_serial = 0;

	/* Sticker SN line (device identifier only) */
	printk("Sticker SN: %010u\n", g_app_config.serial_number);

	ret = app_sht40_read(&temperature, &humidity);
	if (ret == 0) {
		/* SHT4x sensor with serial number and values */
		ret = app_sht40_read_serial(&sht40_serial);
		if (ret == 0) {
			printk("SHT4x SN: %u / T: %.2f C / H: %.1f %%\n",
			       sht40_serial,
			       (double)temperature, (double)humidity);
		}
	}

	/* DS18B20 - external 1-Wire temperature probes */
	if (g_app_config.cap_1w_thermometer) {
		int count = app_ds18b20_get_count();
		for (int i = 0; i < count; i++) {
			uint64_t sn;
			float temp;
			ret = app_ds18b20_read(i, &sn, &temp);
			if (ret == 0) {
				printk("DS18B20-%d SN: %llu / T: %.2f C\n",
				       i + 1, sn, (double)temp);
			}
		}
	}

	/* Machine Probe - external SHT sensor via DS28E17 bridge */
	if (g_app_config.cap_1w_machine_probe) {
		int count = app_machine_probe_get_count();
		for (int i = 0; i < count; i++) {
			uint64_t ds28e17_sn;
			uint32_t sht_sn;
			float temp;
			float hum;

			ret = app_machine_probe_read_hygrometer(i, &ds28e17_sn, &temp, &hum);
			if (ret == 0) {
				/* MP bridge chip SN only */
				printk("MP-%d SN: %llu\n", i + 1, ds28e17_sn);

				/* Read SHT serial and values from machine probe */
				ret = app_machine_probe_read_hygrometer_serial(i, &ds28e17_sn,
									       &sht_sn);
				if (ret == 0 && sht_sn != 0) {
					printk("MP-%d-SHT SN: %u / T: %.2f C / H: %.1f %%\n",
					       i + 1, sht_sn, (double)temp, (double)hum);
				}
			}
		}
	}
}

static int send_lorawan_payload(void)
{
	int ret;

	if (!app_lrw_is_ready()) {
		LOG_WRN("LoRaWAN not ready, skipping send");
		return -EAGAIN;
	}

	/* Sample sensors before composing */
	app_sensor_sample();

	/* Calibration payload format (14 bytes):
	 * [4B serial_number][2B temperature][1B humidity][2B t1][2B t2][2B mp1_t][1B mp1_h]
	 */
	uint8_t buf[14];
	size_t len = 0;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	/* Serial number (4 bytes, big-endian) */
	buf[len++] = (g_app_config.serial_number >> 24) & 0xFF;
	buf[len++] = (g_app_config.serial_number >> 16) & 0xFF;
	buf[len++] = (g_app_config.serial_number >> 8) & 0xFF;
	buf[len++] = g_app_config.serial_number & 0xFF;

	/* Temperature (2 bytes, int16, value*100) */
	int16_t temp = isnan(g_app_sensor_data.temperature) ? 0x7FFF
			: (int16_t)(g_app_sensor_data.temperature * 100);
	buf[len++] = (temp >> 8) & 0xFF;
	buf[len++] = temp & 0xFF;

	/* Humidity (1 byte, value*2) */
	buf[len++] = isnan(g_app_sensor_data.humidity) ? 0xFF
			: (uint8_t)(g_app_sensor_data.humidity * 2);

	/* T1 temperature (2 bytes, int16, value*100) */
	int16_t t1 = isnan(g_app_sensor_data.t1_temperature) ? 0x7FFF
			: (int16_t)(g_app_sensor_data.t1_temperature * 100);
	buf[len++] = (t1 >> 8) & 0xFF;
	buf[len++] = t1 & 0xFF;

	/* T2 temperature (2 bytes, int16, value*100) */
	int16_t t2 = isnan(g_app_sensor_data.t2_temperature) ? 0x7FFF
			: (int16_t)(g_app_sensor_data.t2_temperature * 100);
	buf[len++] = (t2 >> 8) & 0xFF;
	buf[len++] = t2 & 0xFF;

	/* MP1 temperature (2 bytes, int16, value*100) */
	int16_t mp1_t = isnan(g_app_sensor_data.mp1_temperature) ? 0x7FFF
			: (int16_t)(g_app_sensor_data.mp1_temperature * 100);
	buf[len++] = (mp1_t >> 8) & 0xFF;
	buf[len++] = mp1_t & 0xFF;

	/* MP1 humidity (1 byte, value*2) */
	buf[len++] = isnan(g_app_sensor_data.mp1_humidity) ? 0xFF
			: (uint8_t)(g_app_sensor_data.mp1_humidity * 2);

	k_mutex_unlock(&g_app_sensor_data_lock);

	LOG_INF("Sending calibration data via LoRaWAN...");
	LOG_HEXDUMP_DBG(buf, len, "Calibration payload:");

	ret = lorawan_send(1, buf, len, LORAWAN_MSG_UNCONFIRMED);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lorawan_send", ret);
		return ret;
	}

	LOG_INF("Calibration data sent");
	return 0;
}

/* TODO: Hall activation - temporarily disabled
static int setup_calibration_abp(void)
{
	int ret;
	static const uint8_t nwkskey[] = CALIBRATION_NWKSKEY;
	static const uint8_t appskey[] = CALIBRATION_APPSKEY;

	LOG_INF("Setting up calibration ABP mode");

	g_app_config.lrw_region = APP_CONFIG_LRW_REGION_EU868;
	g_app_config.lrw_network = APP_CONFIG_LRW_NETWORK_PUBLIC;
	g_app_config.lrw_adr = false;
	g_app_config.lrw_activation = APP_CONFIG_LRW_ACTIVATION_ABP;

	g_app_config.lrw_devaddr[0] = (CALIBRATION_DEVADDR >> 24) & 0xFF;
	g_app_config.lrw_devaddr[1] = (CALIBRATION_DEVADDR >> 16) & 0xFF;
	g_app_config.lrw_devaddr[2] = (CALIBRATION_DEVADDR >> 8) & 0xFF;
	g_app_config.lrw_devaddr[3] = CALIBRATION_DEVADDR & 0xFF;

	memcpy(g_app_config.lrw_nwkskey, nwkskey, sizeof(nwkskey));
	memcpy(g_app_config.lrw_appskey, appskey, sizeof(appskey));

	app_lrw_join();
	k_sleep(K_SECONDS(2));

	ret = lorawan_set_datarate(CALIBRATION_DATARATE);
	if (ret) {
		LOG_WRN("Failed to set datarate: %d", ret);
	}

	lorawan_enable_adr(false);

	LOG_INF("Calibration ABP setup complete");
	return 0;
}
*/

static void calibration_loop(void)
{
	int ret;
	int64_t start_time = k_uptime_get();
	int64_t last_lrw_send = 0;
	int64_t last_rtt_output = 0;

	LOG_WRN("Entering calibration loop (hall_activated=%d)", m_hall_activated);

	for (;;) {
		int64_t now = k_uptime_get();

		/* Check 2h timeout ONLY for Hall-activated mode */
		if (m_hall_activated) {
			int64_t elapsed_s = (now - start_time) / 1000;
			if (elapsed_s >= CALIBRATION_MAX_DURATION_S) {
				LOG_WRN("Calibration timeout reached (%d seconds), exiting",
					CALIBRATION_MAX_DURATION_S);
				m_calibration_active = false;
				break;
			}
		}

#if defined(CONFIG_WATCHDOG)
		ret = app_wdog_feed();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_wdog_feed", ret);
		}
#endif /* defined(CONFIG_WATCHDOG) */

		/* Yellow LED blink */
		struct app_led_blink_req req = {
			.color = APP_LED_CHANNEL_Y, .duration = 100, .space = 0, .repetitions = 1};
		app_led_blink(&req);

		/* RTT output every CALIBRATION_RTT_INTERVAL_S */
		if ((now - last_rtt_output) >= CALIBRATION_RTT_INTERVAL_S * 1000) {
			read_sensors_rtt();
			last_rtt_output = now;
		}

#if defined(CONFIG_LORAWAN)
		/* LoRaWAN send every CALIBRATION_SEND_INTERVAL_S */
		if ((now - last_lrw_send) >= CALIBRATION_SEND_INTERVAL_S * 1000) {
			send_lorawan_payload();
			last_lrw_send = now;
		}
#endif /* defined(CONFIG_LORAWAN) */

		k_sleep(K_SECONDS(CALIBRATION_RTT_INTERVAL_S));
	}
}

bool app_calibration_check_hall_gesture(void)
{
	struct app_hall_data hall_data;
	int ret;

	/* Both Hall sensors must be enabled in config */
	if (!g_app_config.cap_hall_left || !g_app_config.cap_hall_right) {
		return false;
	}

	ret = app_hall_get_data(&hall_data);
	if (ret) {
		return false;
	}

	int64_t now = k_uptime_get();

	/* Track left Hall sensor hold time */
	if (hall_data.left_is_active) {
		if (!m_hall_left_was_active) {
			/* Just activated */
			m_hall_left_active_since = now;
		}
	} else {
		m_hall_left_active_since = 0;
	}
	m_hall_left_was_active = hall_data.left_is_active;

	/* Track right Hall sensor hold time */
	if (hall_data.right_is_active) {
		if (!m_hall_right_was_active) {
			/* Just activated */
			m_hall_right_active_since = now;
		}
	} else {
		m_hall_right_active_since = 0;
	}
	m_hall_right_was_active = hall_data.right_is_active;

	/* Check if both sensors are held for required time */
	if (m_hall_left_active_since == 0 || m_hall_right_active_since == 0) {
		return false;
	}

	int64_t left_hold_ms = now - m_hall_left_active_since;
	int64_t right_hold_ms = now - m_hall_right_active_since;

	/* Both must be held for at least CALIBRATION_HALL_HOLD_S */
	if (left_hold_ms < CALIBRATION_HALL_HOLD_S * 1000 ||
	    right_hold_ms < CALIBRATION_HALL_HOLD_S * 1000) {
		return false;
	}

	/* Check if activations are within CALIBRATION_HALL_TOLERANCE_S of each other */
	int64_t activation_diff_ms;
	if (m_hall_left_active_since > m_hall_right_active_since) {
		activation_diff_ms = m_hall_left_active_since - m_hall_right_active_since;
	} else {
		activation_diff_ms = m_hall_right_active_since - m_hall_left_active_since;
	}

	if (activation_diff_ms > CALIBRATION_HALL_TOLERANCE_S * 1000) {
		return false;
	}

	LOG_INF("Hall calibration gesture detected!");
	return true;
}

bool app_calibration_is_active(void)
{
	return m_calibration_active;
}

int app_calibration_init(void)
{
	int ret;

	/* Method 1: Config-based activation */
	if (g_app_config.calibration) {
		LOG_WRN("Calibration mode enabled via config");
		m_calibration_active = true;
		m_hall_activated = false;

		/* Initialize sensors using app_sensor_init - same as normal mode */
		ret = app_sensor_init();
		if (ret) {
			LOG_WRN("app_sensor_init failed: %d, continuing anyway", ret);
		}

		/* Use existing LoRaWAN connection (OTAA or ABP as configured) */
		calibration_loop();

		/* If we reach here, config-based calibration doesn't timeout */
		return 0;
	}

	/* TODO: Method 2: Hall sensor activation - temporarily disabled
	LOG_INF("Monitoring Hall sensors for calibration gesture (%d seconds window)",
		CALIBRATION_ACTIVATION_WINDOW_S);

	int64_t window_start = k_uptime_get();

	while ((k_uptime_get() - window_start) < CALIBRATION_ACTIVATION_WINDOW_S * 1000LL) {
#if defined(CONFIG_WATCHDOG)
		ret = app_wdog_feed();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_wdog_feed", ret);
		}
#endif

		if (app_calibration_check_hall_gesture()) {
			LOG_WRN("Calibration mode activated via Hall sensors");
			m_calibration_active = true;
			m_hall_activated = true;

			ret = app_sensor_init();
			if (ret) {
				LOG_WRN("app_sensor_init failed: %d, continuing anyway", ret);
			}

#if defined(CONFIG_LORAWAN)
			setup_calibration_abp();
#endif

			calibration_loop();

			LOG_INF("Returning to normal operation after calibration");
			return 0;
		}

		k_sleep(K_MSEC(100));
	}

	LOG_INF("Calibration activation window expired, continuing normal operation");
	*/

	return 0;
}
