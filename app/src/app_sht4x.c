/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_sht4x.h"
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
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_sht4x, LOG_LEVEL_DBG);

/* Application-level plausibility gate (#202), mirroring the DS18B20 fix (#180).
 * The Zephyr driver CRC-checks the I2C transfer, so a missing sensor errors out
 * — but a stuck/faulty part can return a finite out-of-range value that would
 * feed a false onboard-temp/humidity alarm. Reject readings outside the SHT4x
 * spec window (slightly widened for measurement margin) so the sample is treated
 * as a failed read rather than a valid sample. */
#define SHT4X_TEMP_MIN (-45.0f)
#define SHT4X_TEMP_MAX 130.0f
#define SHT4X_HUM_MIN  0.0f
#define SHT4X_HUM_MAX  100.0f

static uint8_t sht_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0xff;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int j = 0; j < 8; j++) {
			if (crc & 0x80) {
				crc = (crc << 1) ^ 0x31;
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

int app_sht4x_read(float *temperature, float *humidity)
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

	LOG_DBG("Temperature: %s%d.%02d C", APP_FP2(temperature_));

	if (temperature_ < SHT4X_TEMP_MIN || temperature_ > SHT4X_TEMP_MAX) {
		LOG_WRN("Implausible temperature %s%d.%02d C rejected", APP_FP2(temperature_));
		return -ERANGE;
	}

	if (temperature) {
		*temperature = temperature_;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &val);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("sensor_channel_get", ret);
		return ret;
	}

	float humidity_ = sensor_value_to_float(&val);

	LOG_DBG("Humidity: %s%d.%01d %%", APP_FP1(humidity_));

	if (humidity_ < SHT4X_HUM_MIN || humidity_ > SHT4X_HUM_MAX) {
		LOG_WRN("Implausible humidity %s%d.%01d %% rejected", APP_FP1(humidity_));
		return -ERANGE;
	}

	if (humidity) {
		*humidity = humidity_;
	}

	return 0;
}

int app_sht4x_read_serial(uint32_t *serial_number)
{
	int ret;

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

	if (sht_crc8(&data[0], 2) != data[2] || sht_crc8(&data[3], 2) != data[5]) {
		LOG_ERR("CRC mismatch");
		return -EIO;
	}

	/* Serial number is in data[0:1] and data[3:4], with CRC in data[2] and data[5] */
	if (serial_number) {
		*serial_number = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
				 ((uint32_t)data[3] << 8) | (uint32_t)data[4];
		LOG_DBG("SHT40 Serial: %u", *serial_number);
	}

	return 0;
}
