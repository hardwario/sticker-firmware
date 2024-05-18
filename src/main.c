#if 1

#include "app_compose.h"
#include "app_lrw.h"
#include "app_sensor.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>

/* Standard includes */
#include <stddef.h>
#include <stdint.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define REPORT_INTERVAL_SECONDS 30

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(led_r), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_NODELABEL(led_g), gpios);
static const struct gpio_dt_spec led_y = GPIO_DT_SPEC_GET(DT_NODELABEL(led_y), gpios);

static struct k_timer m_send_timer;

static K_THREAD_STACK_DEFINE(m_send_work_stack, 4096);
static struct k_work_q m_send_work_q;

static void send_work_handler(struct k_work *work)
{
	int ret;

	int timeout = REPORT_INTERVAL_SECONDS;

#if defined(CONFIG_ENTROPY_GENERATOR)
	timeout += (int32_t)sys_rand32_get() % (REPORT_INTERVAL_SECONDS / 10);
#endif /* defined(CONFIG_ENTROPY_GENERATOR) */

	LOG_INF("Scheduling next timeout in %d seconds", timeout);

	k_timer_start(&m_send_timer, K_SECONDS(timeout), K_FOREVER);

	uint8_t buf[12];
	size_t len;
	ret = app_compose(buf, sizeof(buf), &len);
	if (ret) {
		LOG_ERR("Call `app_compose` failed: %d", ret);
		return;
	}

#if 1
	LOG_INF("Sending data...");

	ret = app_lrw_send(buf, len);
	if (ret) {
		LOG_ERR("Call `app_lrw_send` failed: %d", ret);
		return;
	}

	LOG_INF("Data sent");
#endif
}

static K_WORK_DEFINE(m_send_work, send_work_handler);

static void send_timer_handler(struct k_timer *timer)
{
	k_work_submit_to_queue(&m_send_work_q, &m_send_work);
}

static K_TIMER_DEFINE(m_send_timer, send_timer_handler, NULL);

int main(void)
{
	int ret;

	LOG_INF("Build time: " __DATE__ " " __TIME__);

	if (!gpio_is_ready_dt(&led_r) || !gpio_is_ready_dt(&led_g) || !gpio_is_ready_dt(&led_y)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("Call `gpio_pin_configure_dt` failed: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("Call `gpio_pin_configure_dt` failed: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&led_y, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("Call `gpio_pin_configure_dt` failed: %d", ret);
		return ret;
	}

	k_sleep(K_SECONDS(1));

	/*
	k_sleep(K_SECONDS(3));
	LOG_INF("Oopsing");
	k_sleep(K_SECONDS(1));
	k_oops();
	*/

	ret = app_lrw_join();
	if (ret) {
		LOG_ERR("Call `app_lrw_join` failed: %d", ret);
		return ret;
	}

	k_work_queue_init(&m_send_work_q);

	k_work_queue_start(&m_send_work_q, m_send_work_stack,
			   K_THREAD_STACK_SIZEOF(m_send_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_timer_start(&m_send_timer, K_SECONDS(1), K_FOREVER);

	for (;;) {
		LOG_INF("Alive");

		gpio_pin_set_dt(&led_g, 1);
		k_sleep(K_MSEC(10));
		gpio_pin_set_dt(&led_g, 0);

		k_sleep(K_SECONDS(5));
	}

	return 0;
}

#else

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

int main(void)
{
	int ret;

	LOG_INF("Build time: " __DATE__ " " __TIME__);

	struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("Port not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT);
	if (ret) {
		LOG_ERR("Call `gpio_pin_configure_dt` failed: %d", ret);
		return ret;
	}

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));

	ret = pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret && ret != -EALREADY) {
		LOG_ERR("Call `pm_device_action_run` failed: %d", ret);
		return ret;
	}

	for (;;) {
		LOG_INF("Alive");

		ret = gpio_pin_toggle_dt(&led);
		if (ret) {
			LOG_ERR("Call `gpio_pin_toggle_dt` failed: %d", ret);
			return ret;
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}

#endif

static int boot_delay(void)
{
	k_sleep(K_MSEC(500));

	return 0;
}

SYS_INIT(boot_delay, POST_KERNEL, 0);

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
