#if 1

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

/* Standard includes */
#include <stdint.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(led_r), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_NODELABEL(led_g), gpios);
static const struct gpio_dt_spec led_y = GPIO_DT_SPEC_GET(DT_NODELABEL(led_y), gpios);

static void send_work_handler(struct k_work *work)
{
	int ret;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	float temperature = g_app_sensor_data.temperature;
	float humidity = g_app_sensor_data.humidity;
	float illuminance = g_app_sensor_data.illuminance;
	int orientation = g_app_sensor_data.orientation;
	k_mutex_unlock(&g_app_sensor_data_lock);

	/*
	uint8_t buf[16];
	size_t len = 0;

	buf[len++] = (uint8_t)(temperature * 10);
	buf[1] = (uint8_t)(humidity * 2);
	buf[6] = (uint8_t)(illuminance * 10);
	buf[5] = (uint8_t)(orientation & 0xff);

	ret = app_lrw_send(buf, len);
	if (ret) {
		LOG_ERR("Call `app_lrw_send` failed: %d", ret);
	}
	*/
}

static K_WORK_DEFINE(m_send_work, send_work_handler);

static void send_timer_handler(struct k_timer *timer)
{
	k_work_submit(&m_send_work);
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

	for (;;) {
		LOG_INF("Alive");

		int32_t random = (int32_t)sys_rand32_get();

		LOG_INF("Random: %d", random);

		gpio_pin_set_dt(&led_g, 1);
		k_sleep(K_MSEC(50));
		gpio_pin_set_dt(&led_g, 0);

		k_sleep(K_SECONDS(1));
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
