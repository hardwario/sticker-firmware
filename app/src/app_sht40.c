/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_sht40.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>

LOG_MODULE_REGISTER(app_sht40, LOG_LEVEL_DBG);

int app_sht40_read(float *temperature, float *humidity)
{
	int ret;

	struct sensor_value val;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(sht40));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = sensor_sample_fetch(dev);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_sample_fetch", ret);
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_channel_get", ret);
		return ret;
	}

	float temperature_ = sensor_value_to_float(&val);

	LOG_DBG("Temperature: %.2f C", (double)temperature_);

	if (temperature) {
		*temperature = temperature_;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &val);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_channel_get", ret);
		return ret;
	}

	float humidity_ = sensor_value_to_float(&val);

	LOG_DBG("Humidity: %.1f %%", (double)humidity_);

	if (humidity) {
		*humidity = humidity_;
	}

	return 0;
}

int app_sht40_read_serial(uint32_t *serial_number)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(sht40));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	/* Get I2C bus and address from devicetree */
	const struct device *i2c_dev = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(sht40)));
	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C device not ready");
		return -ENODEV;
	}

	uint16_t addr = DT_REG_ADDR(DT_NODELABEL(sht40));

	/* SHT4x (SHT40/SHT43) Read Serial Number command: 0x89
	 * Note: For SHT30/SHT33 use 2-byte command 0x3780 instead
	 */
	uint8_t cmd = 0x89;
	uint8_t data[6];

	ret = i2c_write(i2c_dev, &cmd, 1, addr);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("i2c_write", ret);
		return ret;
	}

	/* Wait for measurement (1ms should be enough for serial read) */
	k_sleep(K_MSEC(1));

	ret = i2c_read(i2c_dev, data, sizeof(data), addr);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("i2c_read", ret);
		return ret;
	}

	/* Serial number is in data[0:1] and data[3:4], with CRC in data[2] and data[5] */
	if (serial_number) {
		*serial_number = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
				 ((uint32_t)data[3] << 8) | (uint32_t)data[4];
	}

	LOG_DBG("SHT40 Serial: 0x%08X", *serial_number);

	return 0;
}
