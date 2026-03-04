/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "app_ds18b20.h"
#include "app_led.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_machine_probe.h"
#include "app_sht4x.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <errno.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_calibration, LOG_LEVEL_DBG);

#define SENTINEL          ((int16_t)0x7FFF)
#define PAYLOAD_SIZE      28
#define SEND_INTERVAL_SEC 10

static int m_count_ds18b20;
static int m_count_machine_probe;

static void compose_calibration_payload(uint8_t *buf)
{
	int ret;

	/* Offset 0-3: Serial number (uint32, LE) */
	sys_put_le32(g_app_config.serial_number, &buf[0]);

	/* Offset 4-7: Uptime in seconds (uint32, LE) */
	sys_put_le32((uint32_t)(k_uptime_get() / 1000), &buf[4]);

	/* Offset 8-11: SHT40 internal temperature and humidity */
	int16_t int_temp = SENTINEL;
	int16_t int_hum = SENTINEL;
	{
		float temperature, humidity;

		ret = app_sht4x_read(&temperature, &humidity);
		if (ret) {
			LOG_WRN("SHT40 read failed: %d", ret);
		} else {
			int_temp = (int16_t)(temperature * 100.0f);
			int_hum = (int16_t)(humidity * 100.0f);

			LOG_INF("SHT40 Temperature: %.2f C", (double)temperature);
			LOG_INF("SHT40 Humidity: %.1f %%", (double)humidity);

			printk("Serial number: %010u / Temperature: %.2f C / Humidity: %.1f %%\n",
			       g_app_config.serial_number, (double)temperature, (double)humidity);
		}
	}
	sys_put_le16((uint16_t)int_temp, &buf[8]);
	sys_put_le16((uint16_t)int_hum, &buf[10]);

	/* Offset 12-15: Hygrometer temp/humidity (Machine Probe[0] hygrometer)
	 * Offset 20-23: Probe 1 temp/humidity (same as hygrometer — Machine Probe[0])
	 * Offset 24-27: Probe 2 temp/humidity (Machine Probe[1])
	 */
	int16_t hygro_temp = SENTINEL;
	int16_t hygro_hum = SENTINEL;
	int16_t p1_temp = SENTINEL;
	int16_t p1_hum = SENTINEL;
	int16_t p2_temp = SENTINEL;
	int16_t p2_hum = SENTINEL;

	if (m_count_machine_probe > 0) {
		uint64_t sn;
		float temperature, humidity;

		ret = app_machine_probe_read_hygrometer(0, &sn, &temperature, &humidity);
		if (ret) {
			LOG_WRN("Machine Probe[0] hygrometer read failed: %d", ret);
		} else {
			int16_t t = (int16_t)(temperature * 100.0f);
			int16_t h = (int16_t)(humidity * 100.0f);

			hygro_temp = t;
			hygro_hum = h;
			p1_temp = t;
			p1_hum = h;

			LOG_INF("Machine Probe[0] Temperature: %.2f C / Humidity: %.1f %%",
				(double)temperature, (double)humidity);

			printk("Serial number: %010u / Probe 1 Temperature: %.2f C / "
			       "Humidity: %.1f %%\n",
			       g_app_config.serial_number, (double)temperature,
			       (double)humidity);
		}

		if (m_count_machine_probe > 1) {
			ret = app_machine_probe_read_hygrometer(1, &sn, &temperature, &humidity);
			if (ret) {
				LOG_WRN("Machine Probe[1] hygrometer read failed: %d", ret);
			} else {
				p2_temp = (int16_t)(temperature * 100.0f);
				p2_hum = (int16_t)(humidity * 100.0f);

				LOG_INF("Machine Probe[1] Temperature: %.2f C / Humidity: %.1f %%",
					(double)temperature, (double)humidity);

				printk("Serial number: %010u / Probe 2 Temperature: %.2f C / "
				       "Humidity: %.1f %%\n",
				       g_app_config.serial_number, (double)temperature,
				       (double)humidity);
			}
		}
	}
	sys_put_le16((uint16_t)hygro_temp, &buf[12]);
	sys_put_le16((uint16_t)hygro_hum, &buf[14]);

	/* Offset 16-19: DS18B20 T1 and T2 */
	int16_t t1 = SENTINEL;
	int16_t t2 = SENTINEL;

	if (m_count_ds18b20 > 0) {
		uint64_t sn;
		float temperature;

		ret = app_ds18b20_read(0, &sn, &temperature);
		if (ret) {
			LOG_WRN("DS18B20[0] read failed: %d", ret);
		} else {
			t1 = (int16_t)(temperature * 100.0f);

			LOG_INF("DS18B20[0] Temperature: %.2f C", (double)temperature);

			printk("Serial number: %010u / DS18B20 T1: %.2f C\n",
			       g_app_config.serial_number, (double)temperature);
		}

		if (m_count_ds18b20 > 1) {
			ret = app_ds18b20_read(1, &sn, &temperature);
			if (ret) {
				LOG_WRN("DS18B20[1] read failed: %d", ret);
			} else {
				t2 = (int16_t)(temperature * 100.0f);

				LOG_INF("DS18B20[1] Temperature: %.2f C", (double)temperature);

				printk("Serial number: %010u / DS18B20 T2: %.2f C\n",
				       g_app_config.serial_number, (double)temperature);
			}
		}
	}
	sys_put_le16((uint16_t)t1, &buf[16]);
	sys_put_le16((uint16_t)t2, &buf[18]);

	/* Probe 1 and 2 (offset 20-27) — already computed above */
	sys_put_le16((uint16_t)p1_temp, &buf[20]);
	sys_put_le16((uint16_t)p1_hum, &buf[22]);
	sys_put_le16((uint16_t)p2_temp, &buf[24]);
	sys_put_le16((uint16_t)p2_hum, &buf[26]);
}

int app_calibration_init(void)
{
	int ret;

	if (!g_app_config.calibration) {
		return 0;
	}

	LOG_WRN("Calibration mode is enabled");

	/* Init 1-Wire bus (DS2484) — non-fatal on failure */
	bool w1_ready = false;

	if (g_app_config.cap_1w_thermometer || g_app_config.cap_1w_machine_probe) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));

		ret = device_init(dev);
		if (ret && ret != -EALREADY) {
			LOG_WRN("DS2484 init failed: %d - 1-Wire sensors disabled", ret);
		} else {
			w1_ready = true;
		}
	}

	/* Init DS18B20 sensors */
	if (w1_ready && g_app_config.cap_1w_thermometer) {
		const struct device *dev_0 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_0));

		ret = device_init(dev_0);
		if (ret && ret != -EALREADY) {
			LOG_WRN("ds18b20_0 init failed: %d", ret);
		}

		const struct device *dev_1 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_1));

		ret = device_init(dev_1);
		if (ret && ret != -EALREADY) {
			LOG_WRN("ds18b20_1 init failed: %d", ret);
		}

		ret = app_ds18b20_scan();
		if (ret) {
			LOG_WRN("app_ds18b20_scan failed: %d", ret);
		} else {
			m_count_ds18b20 = app_ds18b20_get_count();
		}
	}

	/* Init Machine Probe sensors */
	if (w1_ready && g_app_config.cap_1w_machine_probe) {
		const struct device *dev_0 = DEVICE_DT_GET(DT_NODELABEL(machine_probe_0));

		ret = device_init(dev_0);
		if (ret && ret != -EALREADY) {
			LOG_WRN("machine_probe_0 init failed: %d", ret);
		}

		const struct device *dev_1 = DEVICE_DT_GET(DT_NODELABEL(machine_probe_1));

		ret = device_init(dev_1);
		if (ret && ret != -EALREADY) {
			LOG_WRN("machine_probe_1 init failed: %d", ret);
		}

		ret = app_machine_probe_scan();
		if (ret) {
			LOG_WRN("app_machine_probe_scan failed: %d", ret);
		} else {
			m_count_machine_probe = app_machine_probe_get_count();
		}
	}

	LOG_INF("Sensors: DS18B20=%d, Machine Probe=%d", m_count_ds18b20, m_count_machine_probe);

	for (;;) {
		LOG_INF("Alive");

#if defined(CONFIG_WATCHDOG)
		ret = app_wdog_feed();
		if (ret) {
			LOG_ERR("app_wdog_feed failed: %d", ret);
		}
#endif /* defined(CONFIG_WATCHDOG) */

		struct app_led_blink_req req = {
			.color = APP_LED_CHANNEL_Y, .duration = 100, .space = 0, .repetitions = 1};
		app_led_blink(&req);

		uint8_t buf[PAYLOAD_SIZE];
		compose_calibration_payload(buf);

#if defined(CONFIG_LORAWAN)
		if (app_lrw_is_ready()) {
			LOG_INF("Sending calibration data...");

			ret = lorawan_send(1, buf, PAYLOAD_SIZE, LORAWAN_MSG_UNCONFIRMED);
			if (ret) {
				LOG_ERR("lorawan_send failed: %d", ret);
			} else {
				LOG_INF("Calibration data sent");
			}
		} else {
			LOG_INF("LoRaWAN not ready, skipping send");
		}
#endif /* defined(CONFIG_LORAWAN) */

		k_sleep(K_SECONDS(SEND_INTERVAL_SEC));
	}

	return 0;
}
