#include "app_alarm.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_led.h"
#include "app_lrw.h"
#include "app_sensor.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define BLINK_INTERVAL_SECONDS 3

static struct k_timer m_send_timer;

static K_THREAD_STACK_DEFINE(m_send_work_stack, 4096);
static struct k_work_q m_send_work_q;

static void send_work_handler(struct k_work *work)
{
	int ret;

	int timeout = g_app_config.interval_report;

#if defined(CONFIG_ENTROPY_GENERATOR)
	timeout += (int32_t)sys_rand32_get() % (g_app_config.interval_report / 10);
#endif /* defined(CONFIG_ENTROPY_GENERATOR) */

	LOG_INF("Scheduling next timeout in %d seconds", timeout);

	k_timer_start(&m_send_timer, K_SECONDS(timeout), K_FOREVER);

	if (!g_app_config.interval_sample) {
		app_sensor_sample();
	}

	uint8_t buf[51];
	size_t len;
	ret = app_compose(buf, sizeof(buf), &len);
	if (ret) {
		LOG_ERR("Call `app_compose` failed: %d", ret);
		return;
	}

	LOG_INF("Sending data...");

	ret = app_lrw_send(buf, len);
	if (ret) {
		LOG_ERR("Call `app_lrw_send` failed: %d", ret);
		return;
	}

	LOG_INF("Data sent");
}

static K_WORK_DEFINE(m_send_work, send_work_handler);

static void send_timer_handler(struct k_timer *timer)
{
	k_work_submit_to_queue(&m_send_work_q, &m_send_work);
}

static K_TIMER_DEFINE(m_send_timer, send_timer_handler, NULL);

static void carousel(void)
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

int main(void)
{
	int ret;

	LOG_INF("Build time: " __DATE__ " " __TIME__);

	if (g_app_config.has_mpl3115a2) {
		const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(mpl3115a2));

		ret = device_init(dev);
		if (ret) {
			LOG_ERR("Call `device_init` failed (mpl3115a2): %d", ret);
			return ret;
		}
	}

	carousel();

	ret = app_lrw_join();
	if (ret) {
		LOG_ERR("Call `app_lrw_join` failed: %d", ret);
		return ret;
	}

	k_work_queue_init(&m_send_work_q);

	k_work_queue_start(&m_send_work_q, m_send_work_stack,
			   K_THREAD_STACK_SIZEOF(m_send_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_timer_start(&m_send_timer, K_SECONDS(5), K_FOREVER);

	for (;;) {
		LOG_INF("Alive");

#if defined(CONFIG_WATCHDOG)
		ret = app_wdog_feed();
		if (ret) {
			LOG_ERR("Call `app_wdog_feed` failed: %d", ret);
			return ret;
		}
#endif /* defined(CONFIG_WATCHDOG) */

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
	int ret;

	ret = app_lrw_join();
	if (ret) {
		LOG_ERR("Call `app_lrw_join` failed: %d", ret);
		shell_print(shell, "command failed: %d", ret);
		return ret;
	}

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_send(const struct shell *shell, size_t argc, char **argv)
{
	k_timer_start(&m_send_timer, K_NO_WAIT, K_FOREVER);

	shell_print(shell, "command succeeded");

	return 0;
}

SHELL_CMD_REGISTER(join, NULL, "Join LoRaWAN network.", cmd_join);
SHELL_CMD_REGISTER(send, NULL, "Send LoRaWAN data.", cmd_send);

#endif /* defined(CONFIG_SHELL) */
