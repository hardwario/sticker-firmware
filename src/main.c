#include "app_ds18b20.h"
#include "app_sht40.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/lorawan/lorawan.h>

/* Standard includes */
#include <stdint.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define LED0_NODE DT_ALIAS(led0)

/* Customize based on network configuration */
#define LORAWAN_DEV_EUI                                                                            \
	{                                                                                          \
		0xDD, 0xEE, 0xAA, 0xDD, 0xBB, 0xEE, 0xEE, 0xFF                                     \
	}
#define LORAWAN_JOIN_EUI                                                                           \
	{                                                                                          \
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00                                     \
	}
#define LORAWAN_APP_KEY                                                                            \
	{                                                                                          \
		0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6, 0xAB, 0xF7, 0x15, 0x88, 0x09,      \
			0xCF, 0x4F, 0x3C                                                           \
	}

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

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

	struct lorawan_join_config join_cfg;
	uint8_t dev_eui[] = LORAWAN_DEV_EUI;
	uint8_t join_eui[] = LORAWAN_JOIN_EUI;
	uint8_t app_key[] = LORAWAN_APP_KEY;

	struct lorawan_downlink_cb downlink_cb = {.port = LW_RECV_PORT_ANY, .cb = dl_callback};

	ret = app_lrw_init();
	if (ret) {
		LOG_ERR("Call `app_lrw_init` failed: %d", ret);
		return ret;
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
		LOG_ERR("lorawan_join_network failed: %d", ret);
		return 0;
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
