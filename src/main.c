#include "app_ds18b20.h"
#include "app_sht40.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <stdint.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
	int ret;
	bool led_state = true;

	LOG_INF("Build time: " __DATE__ " " __TIME__);

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT);
	if (ret) {
		LOG_ERR("Call `gpio_pin_configure_dt` failed: %d", ret);
		return ret;
	}

	ret = app_ds18b20_scan();
	if (ret) {
		LOG_ERR("Call `app_ds18b20_scan` failed: %d", ret);
		return ret;
	}

	for (;;) {
		LOG_INF("Alive");

		app_sht40_read(NULL, NULL);

		ret = gpio_pin_toggle_dt(&led);
		if (ret < 0) {
			LOG_ERR("Call `gpio_pin_toggle_dt` failed: %d", ret);
			return ret;
		}

		led_state = !led_state;

		int count = app_ds18b20_get_count();

		for (int i = 0; i < count; i++) {
			uint64_t serial_number;
			double temperature;
			ret = app_ds18b20_read(i, &serial_number, &temperature);
			if (ret) {
				LOG_ERR("Call `app_ds18b20_read` failed: %d", ret);
			} else {
				LOG_INF("Serial number: %llu / Temperature: %.2f C", serial_number,
					temperature);
			}
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}
