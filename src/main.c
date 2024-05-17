#if 1

#include "app_accel.h"
#include "app_ds18b20.h"
#include "app_lrw.h"
#include "app_opt3001.h"
#include "app_sht40.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <stdint.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(led_r), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_NODELABEL(led_g), gpios);
static const struct gpio_dt_spec led_y = GPIO_DT_SPEC_GET(DT_NODELABEL(led_y), gpios);

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

#if defined(CONFIG_W1)
	ret = app_ds18b20_scan();
	if (ret) {
		LOG_ERR("Call `app_ds18b20_scan` failed: %d", ret);
		return ret;
	}
#endif
	/*
	#if defined(CONFIG_LORAWAN)
		ret = app_lrw_join();
		if (ret) {
			LOG_ERR("Call `app_lrw_join` failed: %d", ret);
			return ret;
		}
	#endif
	*/

	for (;;) {
		LOG_INF("Alive");

		gpio_pin_set_dt(&led_g, 1);
		k_sleep(K_MSEC(50));
		gpio_pin_set_dt(&led_g, 0);

		float temperature, humidity;
		ret = app_sht40_read(&temperature, &humidity);
		if (ret < 0) {
			LOG_ERR("Call `app_sht40_read` failed: %d", ret);
			return ret;
		}

		float accel_x, accel_y, accel_z;
		int orientation;
		ret = app_accel_read(&accel_x, &accel_y, &accel_z, &orientation);
		if (ret < 0) {
			LOG_ERR("Call `app_accel_read` failed: %d", ret);
			return ret;
		}

		float illuminance;
		ret = app_opt3001_read(&illuminance);
		if (ret < 0) {
			LOG_ERR("Call `app_opt3001_read` failed: %d", ret);
			return ret;
		}

#if defined(CONFIG_W1)
		int count = app_ds18b20_get_count();

		for (int i = 0; i < count; i++) {
			uint64_t serial_number;
			float temperature;
			ret = app_ds18b20_read(i, &serial_number, &temperature);
			if (ret) {
				LOG_ERR("Call `app_ds18b20_read` failed: %d", ret);
			} else {
				LOG_INF("Serial number: %llu / Temperature: %.2f C", serial_number,
					(double)temperature);

				gpio_pin_set_dt(&led_r, 1);
				k_sleep(K_MSEC(50));
				gpio_pin_set_dt(&led_r, 0);
			}
		}
#endif

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
