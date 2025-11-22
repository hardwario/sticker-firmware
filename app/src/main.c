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

#if defined(CONFIG_LORAWAN)
	app_lrw_join();
#endif /* defined(CONFIG_LORAWAN) */

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
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_R,
							.duration = 5,
							.space = 0,
							.repetitions = 1};
			app_led_blink(&req);
		} else {
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_G,
							.duration = 5,
							.space = 0,
							.repetitions = 1};
			app_led_blink(&req);
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

#if defined(CONFIG_LORAWAN)

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

#endif /* defined(CONFIG_LORAWAN) */

#endif /* defined(CONFIG_SHELL) */
