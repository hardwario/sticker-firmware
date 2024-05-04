#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>

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

int main(void)
{
	int ret;
	bool led_state = true;

	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
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
		k_msleep(SLEEP_TIME_MS);
	}

	return 0;
}
