#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_ds18b20.h"

#include <stdint.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define SLEEP_TIME_MS 1000

#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int hygro_read(float *temperature, float *humidity)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(sht40));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(dev);
	if (ret) {
		LOG_ERR("Call `sensor_sample_fetch` failed: %d", ret);
		return ret;
	}

	struct sensor_value t;
	ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &t);
	if (ret) {
		LOG_ERR("Call `sensor_channel_get` failed: %d", ret);
		return ret;
	}

	double temperature_ = sensor_value_to_double(&t);

	LOG_DBG("Temperature: %.2f C", temperature_);

	if (temperature) {
		*temperature = temperature_;
	}

	struct sensor_value h;
	ret = sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &h);
	if (ret) {
		LOG_ERR("Call `sensor_channel_get` failed: %d", ret);
		return ret;
	}

	double humidity_ = sensor_value_to_double(&h);

	LOG_DBG("Humidity: %.1f %%", humidity_);

	if (humidity) {
		*humidity = humidity_;
	}

	return 0;
}

#if 0

void w1_search_callback(struct w1_rom rom, void *user_data)
{
	LOG_INF("Device found; family: 0x%02x, serial: 0x%016llx", rom.family,
		w1_rom_to_uint64(&rom));
}

int w1_scan(void)
{
	int ret;
	int res = 0;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = w1_lock_bus(dev);
	if (ret) {
		LOG_ERR("Call `w1_lock_bus` failed: %d", ret);
		res = ret;
		goto error;
	}

	ret = pm_device_action_run(dev, PM_DEVICE_ACTION_RESUME);
	if (ret && ret != -EALREADY) {
		LOG_ERR("Call `pm_device_action_run` failed: %d", ret);
		res = ret;
		goto error;
	}

	k_sleep(K_MSEC(3));

	int num_devices = w1_search_rom(dev, w1_search_callback, NULL);

	LOG_INF("Number of devices found on bus: %d", num_devices);

	ret = pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret && ret != -EALREADY) {
		LOG_ERR("Call `pm_device_action_run` failed: %d", ret);
		res = ret;
		goto error;
	}

	ret = w1_unlock_bus(dev);
	if (ret) {
		LOG_ERR("Call `w1_unlock_bus` failed: %d", ret);
		return ret;
	}

	return 0;

error:
	ret = w1_unlock_bus(dev);
	if (ret) {
		LOG_ERR("Call `w1_unlock_bus` failed: %d", ret);
		res = res ? res : ret;
	}

	return res;
}
#endif

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
		return 0;
	}

	ret = app_ds18b20_scan();
	if (ret) {
		LOG_ERR("Call `app_ds18b20_scan` failed: %d", ret);
		return 0;
	}

	while (1) {
		LOG_INF("Alive");

		hygro_read(NULL, NULL);

		ret = gpio_pin_toggle_dt(&led);
		if (ret < 0) {
			return 0;
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

		k_msleep(SLEEP_TIME_MS);
	}

	return 0;
}
