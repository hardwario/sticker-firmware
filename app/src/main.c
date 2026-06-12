/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_battery.h"
#include "app_calibration.h"
#include "app_clock.h"
#include "app_config.h"
#include "app_history.h"
#include "app_version.h"
#include "app_led.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_nfc.h"
#include "app_sensor.h"
#include "app_settings.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define BLINK_INTERVAL_SECONDS 3
#define NFC_CHECK_BLINKS       10

#define APP_ALARM_ORANGE_RATE_LIMIT_MS 500
#define APP_ALARM_ORANGE_AUTO_OFF_MS   (60 * 60 * 1000)
#define APP_ALARM_ORANGE_BLINK_MS      50

enum app_mode {
	APP_MODE_NORMAL = 0,
	APP_MODE_CALIBRATION,
};

static int m_nfc_counter;

static void die(void)
{
	LOG_ERR("Rebooting in 60 seconds due to fatal error");

	for (int i = 0; i < 60; i++) {
#if defined(CONFIG_WATCHDOG)
		app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */
		k_sleep(K_SECONDS(1));
	}

	sys_reboot(SYS_REBOOT_COLD);
}

static void play_carousel_boot(void)
{
	struct app_led_play_req req = {
		.commands = {{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
			     {.type = APP_LED_CMD_DELAY, .duration = 500},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
			     {.type = APP_LED_CMD_DELAY, .duration = 250},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
			     {.type = APP_LED_CMD_DELAY, .duration = 500},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
			     {.type = APP_LED_CMD_DELAY, .duration = 250},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_ON}},
			     {.type = APP_LED_CMD_DELAY, .duration = 1500},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_OFF}},
			     {.type = APP_LED_CMD_END}},
		.repetitions = 1};

	app_led_play(&req);
	k_sleep(K_MSEC(5000));
}

static void play_carousel_nfc(void)
{
	struct app_led_blink_req req = {
		.color = APP_LED_CHANNEL_Y, .duration = 100, .space = 100, .repetitions = 10};
	app_led_blink(&req);
	k_sleep(K_MSEC(10 * 200 - 100));
}

static enum app_mode detect_mode(void)
{
	if (app_calibration_detect_magnets()) {
		LOG_WRN("Both magnets detected at boot — entering calibration mode");
		return APP_MODE_CALIBRATION;
	}

	if (g_app_config.calibration) {
		LOG_WRN("Calibration flag set in config — entering calibration mode");
		return APP_MODE_CALIBRATION;
	}

	return APP_MODE_NORMAL;
}

static void orange_event_handler(enum app_alarm_source source, bool active, void *user_data)
{
	ARG_UNUSED(source);
	ARG_UNUSED(active);
	ARG_UNUSED(user_data);

	static int64_t last_blink_ms;
	int64_t now = k_uptime_get();

	if (now > APP_ALARM_ORANGE_AUTO_OFF_MS) {
		return;
	}

	if (last_blink_ms != 0 && (now - last_blink_ms) < APP_ALARM_ORANGE_RATE_LIMIT_MS) {
		return;
	}

	struct app_led_blink_req req = {.color = APP_LED_CHANNEL_Y,
					.duration = APP_ALARM_ORANGE_BLINK_MS,
					.space = 0,
					.repetitions = 1};
	app_led_blink(&req);

	last_blink_ms = now;
}

static int init(void)
{
	k_sleep(K_MSEC(500));

	return 0;
}

SYS_INIT(init, POST_KERNEL, 0);

int main(void)
{
	int ret;

	LOG_INF("Firmware version: %d.%d.%d (%s, %s)", APP_VERSION_MAJOR, APP_VERSION_MINOR,
		APP_VERSION_PATCH, app_build_type_str(APP_BUILD_TYPE),
		app_version_is_debug() ? "debug" : "release");
	LOG_INF("Build time: " __DATE__ " " __TIME__);

	/* Shared HW init */
#if defined(CONFIG_WATCHDOG)
	ret = app_wdog_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_wdog_init", ret);
		die();
	}
#endif /* defined(CONFIG_WATCHDOG) */

	ret = app_led_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_led_init", ret);
		die();
	}

	/* Mode detection */
	enum app_mode mode = detect_mode();

	switch (mode) {
	case APP_MODE_CALIBRATION:
		ret = app_calibration_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_calibration_init", ret);
			die();
		}

		app_calibration_run();
		/* Never reached */
		break;

	case APP_MODE_NORMAL:
		break;
	}

	/* --- Normal mode --- */

	ret = app_nfc_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_nfc_init", ret);
		die();
	}

	enum app_nfc_action action;

	/* A stale/replay config left on the tag makes app_nfc_check() fail
	 * (anti-replay) on every boot. Do NOT die() here — that would brick the
	 * device into a reboot loop. Log and continue, like the periodic check
	 * in the main loop does. */
	ret = app_nfc_check(&action);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_nfc_check", ret);
	}

	if (action == APP_NFC_ACTION_SAVE) {
		play_carousel_nfc();

		ret = app_settings_save(true);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_settings_save", ret);
			die();
		}
	} else if (action == APP_NFC_ACTION_RESET) {
		play_carousel_nfc();

		ret = app_settings_reset();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_settings_reset", ret);
			die();
		}
	}

#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

	play_carousel_boot();

	ret = app_clock_init();
	if (ret) {
		LOG_WRN("app_clock_init failed: %d (wall-clock unavailable)", ret);
	}

	ret = app_history_init();
	if (ret) {
		LOG_WRN("app_history_init failed: %d (history unavailable)", ret);
	}

#if defined(CONFIG_LORAWAN)
	ret = app_lrw_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_lrw_init", ret);
		die();
	}
#endif /* defined(CONFIG_LORAWAN) */

	ret = app_battery_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_battery_init", ret);
		die();
	}

#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

	ret = app_sensor_init();
	if (ret) {
		LOG_WRN("Sensor init partially failed: %d (continuing)", ret);
	}

#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

#if defined(CONFIG_LORAWAN)
	app_lrw_join();
#endif /* defined(CONFIG_LORAWAN) */

	app_alarm_set_event_callback(orange_event_handler, NULL);

	/* Normal mode main loop */
	for (;;) {
		LOG_INF("Alive");

#if defined(CONFIG_WATCHDOG)
		ret = app_wdog_feed();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_wdog_feed", ret);
		}
#endif /* defined(CONFIG_WATCHDOG) */

		if (app_nfc_periodic_enabled() && ++m_nfc_counter >= NFC_CHECK_BLINKS) {
			m_nfc_counter = 0;

			ret = app_nfc_poll(&action);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_nfc_check", ret);
			}

			if (action == APP_NFC_ACTION_SAVE) {
				play_carousel_nfc();

				ret = app_settings_save(true);
				if (ret) {
					LOG_ERR_CALL_FAILED_INT("app_settings_save", ret);
				}
			} else if (action == APP_NFC_ACTION_RESET) {
				play_carousel_nfc();

				ret = app_settings_reset();
				if (ret) {
					LOG_ERR_CALL_FAILED_INT("app_settings_reset", ret);
				}
			}
		}

		/* Detect magnet on BOTH Hall sensors → reboot into calibration mode */
		if (k_uptime_get() < (int64_t)APP_CALIBRATION_ACTIVATION_WINDOW_MIN * 60 * 1000) {
			app_calibration_check_trigger();
		}

		bool led_handled = false;

#if defined(CONFIG_LORAWAN)
		enum app_lrw_state lrw_state = app_lrw_get_state();

		if (lrw_state == APP_LRW_STATE_JOINING || lrw_state == APP_LRW_STATE_RECONNECT) {
			struct app_led_play_req req = {
				.commands = {{.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 10},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
					     {.type = APP_LED_CMD_DELAY, .duration = 200},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 10},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
					     {.type = APP_LED_CMD_DELAY, .duration = 200},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 80},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
					     {.type = APP_LED_CMD_END}},
				.repetitions = 1};
			app_led_play(&req);
			led_handled = true;
		} else if (lrw_state == APP_LRW_STATE_WARNING) {
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_Y,
							.duration = 10,
							.space = 200,
							.repetitions = 3};
			app_led_blink(&req);
			led_handled = true;
		}
#endif /* defined(CONFIG_LORAWAN) */

		if (!led_handled && app_alarm_poll()) {
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_R,
							.duration = 5,
							.space = 0,
							.repetitions = 1};
			app_led_blink(&req);
			led_handled = true;
		}

		if (!led_handled) {
#if defined(CONFIG_FW_DEBUG)
			/* Debug: green + yellow LED blink */
			struct app_led_play_req req = {
				.commands = {{.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_G, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 5},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_G, APP_LED_OFF}},
					     {.type = APP_LED_CMD_DELAY, .duration = 50},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 5},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
					     {.type = APP_LED_CMD_END}},
				.repetitions = 1};
			app_led_play(&req);
#else
			/* Release: green LED blink */
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_G,
							.duration = 5,
							.space = 0,
							.repetitions = 1};
			app_led_blink(&req);
#endif /* defined(CONFIG_FW_DEBUG) */
		}

		k_sleep(K_SECONDS(BLINK_INTERVAL_SECONDS));
	}

	return 0;
}

#if defined(CONFIG_SHELL) && defined(CONFIG_LORAWAN)

static int cmd_join(const struct shell *shell, size_t argc, char **argv)
{
	app_lrw_join();

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_send(const struct shell *shell, size_t argc, char **argv)
{
	app_lrw_send();

	shell_print(shell, "command succeeded");

	return 0;
}

SHELL_CMD_REGISTER(join, NULL, "Join LoRaWAN network.", cmd_join);
SHELL_CMD_REGISTER(send, NULL, "Send LoRaWAN data.", cmd_send);

#endif /* defined(CONFIG_SHELL) && defined(CONFIG_LORAWAN) */
