#include "app_alarm.h"
#include "app_config.h"
#include "app_led.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_nfc.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#if defined(CONFIG_DEBUG)
#define BLINK_INTERVAL_SECONDS 1
#define NFC_CHECK_BLINKS       1
#else
#define BLINK_INTERVAL_SECONDS 3
#define NFC_CHECK_BLINKS       10
#endif /* defined(CONFIG_DEBUG) */

static void play_carousel_boot(void)
{
	app_led_set(APP_LED_CHANNEL_R, 1);
	k_sleep(K_MSEC(500));
	app_led_set(APP_LED_CHANNEL_R, 0);
	k_sleep(K_MSEC(250));
	app_led_set(APP_LED_CHANNEL_Y, 1);
	k_sleep(K_MSEC(500));
	app_led_set(APP_LED_CHANNEL_Y, 0);
	k_sleep(K_MSEC(250));
	app_led_set(APP_LED_CHANNEL_G, 1);
	k_sleep(K_MSEC(1500));
	app_led_set(APP_LED_CHANNEL_G, 0);
}

static void play_carousel_nfc(void)
{
	for (int i = 10; i; i--) {
		app_led_set(APP_LED_CHANNEL_Y, 1);
		k_sleep(K_MSEC(100));
		app_led_set(APP_LED_CHANNEL_Y, 0);

		if (i == 1) {
			break;
		}

		k_sleep(K_MSEC(100));
	}
}

int main(void)
{
	int ret;

	LOG_INF("Build time: " __DATE__ " " __TIME__);

	enum app_nfc_action action;
	ret = app_nfc_check(&action);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_nfc_check", ret);
	}

	if (action == APP_NFC_ACTION_SAVE) {
		play_carousel_nfc();

		ret = app_config_save();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_config_save", ret);
		}
	} else if (action == APP_NFC_ACTION_RESET) {
		play_carousel_nfc();

		ret = app_config_reset();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_config_reset", ret);
		}
	}

	play_carousel_boot();

	app_lrw_join();

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

				ret = app_config_save();
				if (ret) {
					LOG_ERR_CALL_FAILED_INT("app_config_save", ret);
				}
			} else if (action == APP_NFC_ACTION_RESET) {
				play_carousel_nfc();

				ret = app_config_reset();
				if (ret) {
					LOG_ERR_CALL_FAILED_INT("app_config_reset", ret);
				}
			}
		}

		if (app_alarm_is_active()) {
			app_led_set(APP_LED_CHANNEL_R, 1);
			k_sleep(K_MSEC(5));
			app_led_set(APP_LED_CHANNEL_R, 0);
		} else {
			app_led_set(APP_LED_CHANNEL_G, 1);
			k_sleep(K_MSEC(5));
			app_led_set(APP_LED_CHANNEL_G, 0);
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

#if defined(CONFIG_SHELL)

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

#endif /* defined(CONFIG_SHELL) */
