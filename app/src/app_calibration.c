/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_calibration.h"
#include "app_config.h"
#include "app_ds18b20.h"
#include "app_led.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_machine_probe.h"
#include "app_sht4x.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

/* Standard includes */
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_calibration, LOG_LEVEL_DBG);

#define SENTINEL          ((int16_t)0x7FFF)
#define PAYLOAD_SIZE      24
#define LOOP_INTERVAL_SEC 1
#define SEND_INTERVAL_SEC 30
#define SETTLE_DELAY_SEC  2
#define ENTRY_BLINKS      5
#define CALIBRATION_PORT        10
#define MAGNET_PENDING_MAX      2

/* Fixed ABP keys for calibration mode — all devices use the same credentials */
static const uint8_t m_cal_deveui[] = {0x02, 0x40, 0x3b, 0x84, 0xfd, 0x45, 0x1f, 0x37};
static const uint8_t m_cal_devaddr[] = {0x01, 0x2c, 0x86, 0x9a};
static const uint8_t m_cal_nwkskey[] = {0xaf, 0x4f, 0x05, 0x0b, 0xd3, 0x74, 0x0d, 0x19,
					0x70, 0xd3, 0x63, 0xb2, 0xb4, 0x9e, 0x1a, 0x7b};
static const uint8_t m_cal_appskey[] = {0xd9, 0xa9, 0xc4, 0x1a, 0xcf, 0x55, 0x99, 0xdc,
					0xe1, 0x16, 0x8e, 0xfe, 0x6d, 0x29, 0x1d, 0xab};

static int m_count_ds18b20;
static int m_count_machine_probe;

static struct app_led_play_req m_orange_blink = {
	.commands = {{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
		     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_ON}},
		     {.type = APP_LED_CMD_DELAY, .duration = 50},
		     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
		     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_OFF}},
		     {.type = APP_LED_CMD_END}},
	.repetitions = 1};

static bool read_hall_gpio(bool *out_left, bool *out_right)
{
	const struct gpio_dt_spec halls[] = {
		GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
		GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
	};
	bool active[ARRAY_SIZE(halls)] = {false};

	for (int i = 0; i < ARRAY_SIZE(halls); i++) {
		if (!gpio_is_ready_dt(&halls[i])) {
			return false;
		}

		gpio_pin_configure_dt(&halls[i], GPIO_INPUT | GPIO_PULL_UP);
	}

	k_busy_wait(100);

	for (int i = 0; i < ARRAY_SIZE(halls); i++) {
		int val = gpio_pin_get_dt(&halls[i]);
		if (val >= 0) {
			active[i] = val;
		}

		gpio_pin_configure_dt(&halls[i], GPIO_INPUT | GPIO_PULL_DOWN);
	}

	if (out_left) {
		*out_left = active[0];
	}

	if (out_right) {
		*out_right = active[1];
	}

	return true;
}

bool app_calibration_detect_magnets(void)
{
	bool left, right;

	if (!read_hall_gpio(&left, &right)) {
		return false;
	}

	return left && right;
}

void app_calibration_check_trigger(void)
{
	static bool magnet_pending;
	static bool magnet_expired;
	static int magnet_pending_cycles;
	bool left, right;

	if (!read_hall_gpio(&left, &right)) {
		return;
	}

	if (magnet_expired) {
		if (!left && !right) {
			magnet_expired = false;
		}
		return;
	}

	if (left && right) {
		LOG_WRN("Both magnets detected — rebooting into calibration mode");
		sys_reboot(SYS_REBOOT_COLD);
	} else if (left || right) {
		if (!magnet_pending) {
			magnet_pending = true;
			magnet_pending_cycles = 0;
		} else if (++magnet_pending_cycles >= MAGNET_PENDING_MAX) {
			magnet_pending = false;
			magnet_expired = true;
		}
	} else {
		magnet_pending = false;
	}
}

static void compose_calibration_payload(uint8_t *buf)
{
	int ret;

	/* Offset 0-3: Serial number (uint32, LE) */
	sys_put_le32(g_app_config.serial_number, &buf[0]);

	/* Offset 4-7: Uptime in seconds (uint32, LE) */
	sys_put_le32((uint32_t)(k_uptime_get() / 1000), &buf[4]);

	/* Offset 8-11: SHT40 temperature and humidity */
	int16_t sht_temp = SENTINEL;
	int16_t sht_hum = SENTINEL;
	{
		float temperature, humidity;

		ret = app_sht4x_read(&temperature, &humidity);
		if (!ret) {
			sht_temp = (int16_t)(temperature * 100.0f);
			sht_hum = (int16_t)(humidity * 100.0f);
		}
	}
	sys_put_le16((uint16_t)sht_temp, &buf[8]);
	sys_put_le16((uint16_t)sht_hum, &buf[10]);

	/* Offset 12-15: DS18B20 T1 and T2 */
	int16_t t1 = SENTINEL;
	int16_t t2 = SENTINEL;

	if (m_count_ds18b20 > 0) {
		uint64_t sn;
		float temperature;

		ret = app_ds18b20_read(0, &sn, &temperature);
		if (!ret) {
			t1 = (int16_t)(temperature * 100.0f);
		}

		if (m_count_ds18b20 > 1) {
			ret = app_ds18b20_read(1, &sn, &temperature);
			if (!ret) {
				t2 = (int16_t)(temperature * 100.0f);
			}
		}
	}

	sys_put_le16((uint16_t)t1, &buf[12]);
	sys_put_le16((uint16_t)t2, &buf[14]);

	/* Offset 16-23: Machine Probe 1 and 2 temp/humidity */
	int16_t p1_temp = SENTINEL;
	int16_t p1_hum = SENTINEL;
	int16_t p2_temp = SENTINEL;
	int16_t p2_hum = SENTINEL;

	if (m_count_machine_probe > 0) {
		uint64_t sn;
		float temperature, humidity;

		ret = app_machine_probe_read_hygrometer(0, &sn, &temperature, &humidity);
		if (!ret) {
			p1_temp = (int16_t)(temperature * 100.0f);
			p1_hum = (int16_t)(humidity * 100.0f);
		}

		if (m_count_machine_probe > 1) {
			ret = app_machine_probe_read_hygrometer(1, &sn, &temperature, &humidity);
			if (!ret) {
				p2_temp = (int16_t)(temperature * 100.0f);
				p2_hum = (int16_t)(humidity * 100.0f);
			}
		}
	}

	sys_put_le16((uint16_t)p1_temp, &buf[16]);
	sys_put_le16((uint16_t)p1_hum, &buf[18]);
	sys_put_le16((uint16_t)p2_temp, &buf[20]);
	sys_put_le16((uint16_t)p2_hum, &buf[22]);
}

int app_calibration_init(void)
{
	int ret;

	/* Set calibration ABP keys */
	g_app_config.calibration = true;
	g_app_config.lrw_activation = APP_CONFIG_LRW_ACTIVATION_ABP;
	memcpy(g_app_config.lrw_deveui, m_cal_deveui, sizeof(m_cal_deveui));
	memset(g_app_config.lrw_joineui, 0, sizeof(g_app_config.lrw_joineui));
	memcpy(g_app_config.lrw_devaddr, m_cal_devaddr, sizeof(m_cal_devaddr));
	memcpy(g_app_config.lrw_nwkskey, m_cal_nwkskey, sizeof(m_cal_nwkskey));
	memcpy(g_app_config.lrw_appskey, m_cal_appskey, sizeof(m_cal_appskey));

	/* Init LoRaWAN */
#if defined(CONFIG_LORAWAN)
	ret = app_lrw_init();
	if (ret) {
		return ret;
	}

	app_lrw_join();

	lorawan_enable_adr(false);
	ret = lorawan_set_datarate(LORAWAN_DR_5);
#endif /* defined(CONFIG_LORAWAN) */

	/* Init 1-Wire bus (DS2484) — non-fatal on failure */
	bool w1_ready = false;

	if (g_app_config.cap_1w_thermometer || g_app_config.cap_1w_machine_probe) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));

		ret = device_init(dev);
		if (!ret || ret == -EALREADY) {
			w1_ready = true;
		}
	}

	/* Init DS18B20 sensors */
	if (w1_ready && g_app_config.cap_1w_thermometer) {
		device_init(DEVICE_DT_GET(DT_NODELABEL(ds18b20_0)));
		device_init(DEVICE_DT_GET(DT_NODELABEL(ds18b20_1)));

		if (!app_ds18b20_scan()) {
			m_count_ds18b20 = app_ds18b20_get_count();
		}
	}

	/* Init Machine Probe sensors */
	if (w1_ready && g_app_config.cap_1w_machine_probe) {
		device_init(DEVICE_DT_GET(DT_NODELABEL(machine_probe_0)));
		device_init(DEVICE_DT_GET(DT_NODELABEL(machine_probe_1)));

		if (!app_machine_probe_scan()) {
			m_count_machine_probe = app_machine_probe_get_count();
		}
	}

#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

	/* One-time 5x fast yellow blink to indicate calibration entry */
	{
		struct app_led_blink_req req = {
			.color = APP_LED_CHANNEL_Y,
			.duration = 100,
			.space = 100,
			.repetitions = ENTRY_BLINKS};
		app_led_blink(&req);
	}

	return 0;
}

void app_calibration_run(void)
{
	k_sleep(K_SECONDS(SETTLE_DELAY_SEC));

	int64_t deadline = k_uptime_get()
			 + (int64_t)APP_CALIBRATION_DURATION_MIN * 60 * 1000;
	int counter = SEND_INTERVAL_SEC;

	for (;;) {
		if (k_uptime_get() >= deadline) {
			sys_reboot(SYS_REBOOT_COLD);
		}

#if defined(CONFIG_WATCHDOG)
		app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

		if (++counter >= SEND_INTERVAL_SEC) {
			counter = 0;

			uint8_t buf[PAYLOAD_SIZE];
			compose_calibration_payload(buf);

#if defined(CONFIG_LORAWAN)
			if (app_lrw_is_ready()) {
				lorawan_send(CALIBRATION_PORT, buf,
					     PAYLOAD_SIZE,
					     LORAWAN_MSG_UNCONFIRMED);
			}
#endif /* defined(CONFIG_LORAWAN) */
		}

		app_led_play(&m_orange_blink);

		k_sleep(K_SECONDS(LOOP_INTERVAL_SEC));
	}
}
