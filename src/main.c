#if 1

#include "app_accel.h"
#include "app_ds18b20.h"
#include "app_sht40.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/init.h>
#include <zephyr/lorawan/lorawan.h>

/* Standard includes */
#include <stdint.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

int error_code = 0;

/* Customize based on network configuration */
#define LORAWAN_DEV_EUI                                                                            \
	{                                                                                          \
		0x70, 0xB3, 0xD5, 0x7E, 0xD0, 0x06, 0x76, 0x0E                                     \
	}
#define LORAWAN_JOIN_EUI                                                                           \
	{                                                                                          \
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00                                     \
	}
#define LORAWAN_APP_KEY                                                                            \
	{                                                                                          \
		0xC5, 0x87, 0x56, 0xD7, 0x08, 0xCD, 0x99, 0x44, 0x32, 0xFC, 0x9B, 0x46, 0x94,      \
			0xAA, 0x24, 0x13                                                           \
	}

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(led_r), gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_NODELABEL(led_g), gpios);
static const struct gpio_dt_spec led_y = GPIO_DT_SPEC_GET(DT_NODELABEL(led_y), gpios);

char data[] = {'h', 'e', 'l', 'l', 'o', 'w', 'o', 'r', 'l', 'd'};

static void dl_callback(uint8_t port, bool data_pending, int16_t rssi, int8_t snr, uint8_t len,
			const uint8_t *hex_data)
{
	LOG_INF("Port %d, Pending %d, RSSI %ddB, SNR %ddBm", port, data_pending, rssi, snr);
	if (hex_data) {
		LOG_HEXDUMP_INF(hex_data, len, "Payload: ");
	}
}

static void lorwan_datarate_changed(enum lorawan_datarate dr)
{
	uint8_t unused, max_size;

	lorawan_get_payload_sizes(&unused, &max_size);
	LOG_INF("New Datarate: DR_%d, Max Payload %d", dr, max_size);
}

int app_lrw_init(void)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(lora0));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

#if defined(CONFIG_LORAMAC_REGION_EU868)
	ret = lorawan_set_region(LORAWAN_REGION_EU868);
	if (ret < 0) {
		LOG_ERR("Call `lorawan_set_region` failed: %d", ret);
		return ret;
	}
#endif

	ret = lorawan_start();
	if (ret) {
		LOG_ERR("Call `lorawan_start` failed: %d", ret);
		return ret;
	}

	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("Build time: " __DATE__ " " __TIME__);

	if (!gpio_is_ready_dt(&led_r) || !gpio_is_ready_dt(&led_g) || !gpio_is_ready_dt(&led_y)) {
		LOG_ERR("Device not ready");
		goto error;
	}

	ret = gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("Call `gpio_pin_configure_dt` failed: %d", ret);
		goto error2;
	}

	ret = gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("Call `gpio_pin_configure_dt` failed: %d", ret);
		goto error2;
	}

	ret = gpio_pin_configure_dt(&led_y, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("Call `gpio_pin_configure_dt` failed: %d", ret);
		goto error2;
	}

	k_sleep(K_SECONDS(1));

#if defined(CONFIG_W1)
	ret = app_ds18b20_scan();
	if (ret) {
		LOG_ERR("Call `app_ds18b20_scan` failed: %d", ret);
		goto error3;
	}
#endif

#if defined(CONFIG_LORAWAN)
	struct lorawan_join_config join_cfg;
	uint8_t dev_eui[] = LORAWAN_DEV_EUI;
	uint8_t join_eui[] = LORAWAN_JOIN_EUI;
	uint8_t app_key[] = LORAWAN_APP_KEY;

	struct lorawan_downlink_cb downlink_cb = {.port = LW_RECV_PORT_ANY, .cb = dl_callback};

	ret = app_lrw_init();
	if (ret) {
		LOG_ERR("Call `app_lrw_init` failed: %d", ret);
		goto error;
	}

	lorawan_register_downlink_callback(&downlink_cb);
	lorawan_register_dr_changed_callback(lorwan_datarate_changed);

	join_cfg.mode = LORAWAN_ACT_OTAA;
	join_cfg.dev_eui = dev_eui;
	join_cfg.otaa.join_eui = join_eui;
	join_cfg.otaa.app_key = app_key;
	join_cfg.otaa.nwk_key = app_key;
	join_cfg.otaa.dev_nonce = 0u;

	LOG_INF("Joining network over OTAA");
	ret = lorawan_join(&join_cfg);
	if (ret < 0) {
		LOG_WRN("lorawan_join_network failed: %d", ret);
	}
#endif

	for (;;) {
		LOG_INF("Alive");

		gpio_pin_set_dt(&led_g, 1);
		k_sleep(K_MSEC(50));
		gpio_pin_set_dt(&led_g, 0);

		float temperature, humidity;
		ret = app_sht40_read(&temperature, &humidity);
		if (ret < 0) {
			LOG_ERR("Call `app_sht40_read` failed: %d", ret);
			goto error;
		}

		float accel_x, accel_y, accel_z, orientation;
		ret = app_accel_read(&accel_x, &accel_y, &accel_z, &orientation);
		if (ret < 0) {
			LOG_ERR("Call `app_accel_read` failed: %d", ret);
			goto error;
		}

#if defined(CONFIG_W1)
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
#endif

		k_sleep(K_SECONDS(1));
	}

	return 0;

error:

	for (;;) {
		LOG_ERR("Error loop");

		ret = gpio_pin_toggle_dt(&led_r);
		if (ret < 0) {
			LOG_ERR("Call `gpio_pin_toggle_dt` failed: %d", ret);
			return ret;
		}

		k_sleep(K_MSEC(50));
	}

	return 1;

error2:

	for (;;) {
		LOG_ERR("Error loop");

		ret = gpio_pin_toggle_dt(&led_r);
		if (ret < 0) {
			LOG_ERR("Call `gpio_pin_toggle_dt` failed: %d", ret);
			return ret;
		}

		k_sleep(K_MSEC(3250));
	}

	return 1;

error3:

	for (;;) {
		LOG_ERR("Error loop");

		for (int i = 0; i < error_code; i++) {
			gpio_pin_set_dt(&led_y, 1);
			k_sleep(K_MSEC(200));
			gpio_pin_set_dt(&led_y, 0);
			k_sleep(K_MSEC(200));
		}

		k_sleep(K_MSEC(1250));
	}

	return 1;
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
