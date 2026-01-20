#include "app_alarm.h"
#include "app_battery.h"
#include "app_calibration.h"
#include "app_config.h"
#include "app_led.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_nfc.h"
#include "app_wdog.h"
#include "app_sensor.h"
#include "app_settings.h"

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


static void die(void)
{
	LOG_ERR("Rebooting in 60 seconds due to fatal error");
	k_sleep(K_SECONDS(60));
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

int main(void)
{
	int ret;

	LOG_INF("Build time: " __DATE__ " " __TIME__);

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

	ret = app_nfc_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_nfc_init", ret);
		die();
	}

	enum app_nfc_action action;
	ret = app_nfc_check(&action);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_nfc_check", ret);
		die();
	}

	if (action == APP_NFC_ACTION_SAVE) {
		play_carousel_nfc();

		ret = app_settings_save();
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

	play_carousel_boot();

#if defined(CONFIG_LORAWAN)
	ret = app_lrw_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_lrw_init", ret);
		die();
	}

	app_lrw_join();
#endif /* defined(CONFIG_LORAWAN) */

	ret = app_battery_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_battery_init", ret);
		die();
	}

	ret = app_sensor_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_sensor_init", ret);
		die();
	}

	ret = app_calibration_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_calibration_init", ret);
		die();
	}

	for (;;) {
		LOG_INF("Alive");

#if defined(CONFIG_WATCHDOG)
		ret = app_wdog_feed();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_wdog_feed", ret);
		}
#endif /* defined(CONFIG_WATCHDOG) */

		static int nfc_counter;

		if (nfc_counter++ == NFC_CHECK_BLINKS) {
			nfc_counter = 0;

			ret = app_nfc_check(&action);
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_nfc_check", ret);
			}

			if (action == APP_NFC_ACTION_SAVE) {
				play_carousel_nfc();

				ret = app_settings_save();
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

#if defined(CONFIG_LORAWAN)
		enum app_lrw_state lrw_state = app_lrw_get_state();

		 if (lrw_state == APP_LRW_STATE_JOINING ||
			 lrw_state == APP_LRW_STATE_RECONNECT) {
			struct app_led_play_req req = {
				.commands = {
					{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
					{.type = APP_LED_CMD_DELAY, .duration = 10},
					{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
					{.type = APP_LED_CMD_DELAY, .duration = 200},
					{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
					{.type = APP_LED_CMD_DELAY, .duration = 10},
					{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
					{.type = APP_LED_CMD_DELAY, .duration = 200},
					{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
					{.type = APP_LED_CMD_DELAY, .duration = 80},
					{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
					{.type = APP_LED_CMD_END}},
				.repetitions = 1};
			app_led_play(&req);
		} else if (lrw_state == APP_LRW_STATE_WARNING) {
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_Y,
							.duration = 10,
							.space = 200,
							.repetitions = 3};
			app_led_blink(&req);
		}
#endif /* defined(CONFIG_LORAWAN) */
		else if (app_alarm_is_active()) {
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_R,
							.duration = 5,
							.space = 0,
							.repetitions = 1};
			app_led_blink(&req);
		} else {
#if defined(CONFIG_FW_DEBUG)
			/* Debug: yellow LED blink */
			struct app_led_play_req req = {
				.commands = {
					{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
					{.type = APP_LED_CMD_DELAY, .duration = 5},
					{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
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

static int init(void)
{
	k_sleep(K_MSEC(500));

	return 0;
}

SYS_INIT(init, POST_KERNEL, 0);

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
