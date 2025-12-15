/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_machine_probe.h"
#include "app_log.h"
#include "app_w1.h"

/* STICKER includes */
#include <sticker/drivers/w1/ds28e17.h>

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/w1.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_machine_probe, LOG_LEVEL_DBG);

#define TMP112_I2C_ADDR  0x48
#define TMP112_INIT_TIME K_MSEC(10)
#define TMP112_CONV_TIME K_MSEC(50)

#define SHT30_I2C_ADDR  0x45
#define SHT30_INIT_TIME K_MSEC(100)
#define SHT30_CONV_TIME K_MSEC(100)

/* Machine Probe SHT sensor addresses (auto-detected at runtime) */
#define SHT43_I2C_ADDR 0x44
#define SHT33_I2C_ADDR 0x45

#define OPT3001_I2C_ADDR  0x44
#define OPT3001_INIT_TIME K_MSEC(10)
#define OPT3001_CONV_TIME K_MSEC(2000)

#define SI7210_I2C_ADDR 0x32

#define LIS2DH12_I2C_ADDR 0x19

#define LIS2DH12_CTRL_REG1     0x20
#define LIS2DH12_CTRL_REG2     0x21
#define LIS2DH12_CTRL_REG3     0x22
#define LIS2DH12_CTRL_REG4     0x23
#define LIS2DH12_CTRL_REG5     0x24
#define LIS2DH12_REFERENCE     0x26
#define LIS2DH12_STATUS_REG    0x27
#define LIS2DH12_INT1_CFG      0x30
#define LIS2DH12_INT1_SRC      0x31
#define LIS2DH12_INT1_THS      0x32
#define LIS2DH12_INT1_DURATION 0x33

struct sensor {
	uint64_t serial_number;
	const struct device *dev;
};

static K_MUTEX_DEFINE(m_lock);

static struct app_w1 m_w1;

static struct sensor m_sensors[] = {
	{.dev = DEVICE_DT_GET(DT_NODELABEL(machine_probe_0))},
	{.dev = DEVICE_DT_GET(DT_NODELABEL(machine_probe_1))},
};

static int m_count;

static int tmp112_init(const struct device *dev)
{
	int ret;

	uint8_t write_buf[3];

	write_buf[0] = 0x01;
	write_buf[1] = 0x01;
	write_buf[2] = 0x80;

	ret = ds28e17_i2c_write(dev, TMP112_I2C_ADDR, write_buf, 3);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	return 0;
}

static int tmp112_convert(const struct device *dev)
{
	int ret;

	uint8_t write_buf[2];

	write_buf[0] = 0x01;
	write_buf[1] = 0x81;

	ret = ds28e17_i2c_write(dev, TMP112_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	return 0;
}

static int tmp112_read(const struct device *dev, float *temperature)
{
	int ret;

	uint8_t write_buf[1];
	uint8_t read_buf[2];

	write_buf[0] = 0x01;

	ret = ds28e17_i2c_write_read(dev, TMP112_I2C_ADDR, write_buf, 1, read_buf, 1);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write_read", ret);
		return ret;
	}

	if ((read_buf[0] & 0x81) != 0x81) {
		LOG_ERR("Conversion not done");
		return -EBUSY;
	}

	write_buf[0] = 0x00;

	ret = ds28e17_i2c_write_read(dev, TMP112_I2C_ADDR, write_buf, 1, read_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write_read", ret);
		return ret;
	}

	if (temperature) {
		*temperature = (sys_get_be16(&read_buf[0]) >> 4) * 0.0625f;
	}

	return 0;
}

static int sht30_init(const struct device *dev)
{
	int ret;

	uint8_t write_buf[2];

	write_buf[0] = 0x30;
	write_buf[1] = 0xa2;

	ret = ds28e17_i2c_write(dev, SHT30_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	return 0;
}

static int sht30_convert(const struct device *dev)
{
	int ret;

	uint8_t write_buf[2];

	write_buf[0] = 0x24;
	write_buf[1] = 0x00;

	ret = ds28e17_i2c_write(dev, SHT30_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	return 0;
}

static int sht30_read(const struct device *dev, float *temperature, float *humidity)
{
	int ret;

	uint8_t read_buf[6];

	ret = ds28e17_i2c_read(dev, SHT30_I2C_ADDR, read_buf, 6);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_read", ret);
		return ret;
	}

	if (temperature) {
		*temperature = -45.f + 175.f * (float)sys_get_be16(&read_buf[0]) / 65535.f;
	}

	if (humidity) {
		*humidity = 100.f * (float)sys_get_be16(&read_buf[3]) / 65535.f;
	}

	return 0;
}

static int sht_read_serial(const struct device *dev, uint32_t *serial_number)
{
	int ret;

	uint8_t write_buf[2];
	uint8_t read_buf[6];

	/* Try SHT43 first (address 0x44, command 0x89) */
	write_buf[0] = 0x89;
	ret = ds28e17_i2c_write(dev, SHT43_I2C_ADDR, write_buf, 1);
	if (ret == 0) {
		k_sleep(K_MSEC(1));
		ret = ds28e17_i2c_read(dev, SHT43_I2C_ADDR, read_buf, 6);
		if (ret == 0) {
			LOG_DBG("SHT43 detected");
			goto parse_serial;
		}
	}

	/* Fallback to SHT30/SHT33 (address 0x45, command 0x3780) */
	write_buf[0] = 0x37;
	write_buf[1] = 0x80;
	ret = ds28e17_i2c_write(dev, SHT33_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	k_sleep(K_MSEC(1));

	ret = ds28e17_i2c_read(dev, SHT33_I2C_ADDR, read_buf, 6);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_read", ret);
		return ret;
	}

	LOG_DBG("SHT33 detected");

parse_serial:
	if (serial_number) {
		/* Serial is in bytes 0-1 and 3-4, with CRC in bytes 2 and 5 */
		*serial_number = ((uint32_t)read_buf[0] << 24) | ((uint32_t)read_buf[1] << 16) |
				 ((uint32_t)read_buf[3] << 8) | (uint32_t)read_buf[4];
	}

	return 0;
}

static int opt3001_init(const struct device *dev)
{
	int ret;

	uint8_t write_buf[3];

	write_buf[0] = 0x01;
	write_buf[1] = 0xc8;
	write_buf[2] = 0x10;

	ret = ds28e17_i2c_write(dev, OPT3001_I2C_ADDR, write_buf, 3);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	return 0;
}

static int opt3001_convert(const struct device *dev)
{
	int ret;

	uint8_t write_buf[3];

	write_buf[0] = 0x01;
	write_buf[1] = 0xca;
	write_buf[2] = 0x10;

	ret = ds28e17_i2c_write(dev, OPT3001_I2C_ADDR, write_buf, 3);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	return 0;
}

static int opt3001_read(const struct device *dev, float *illuminance)
{
	int ret;

	uint8_t write_buf[1];
	uint8_t read_buf[2];

	write_buf[0] = 0x01;

	ret = ds28e17_i2c_write_read(dev, OPT3001_I2C_ADDR, write_buf, 1, read_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write_read", ret);
		return ret;
	}

	if ((read_buf[0] & 0x06) != 0x00 || (read_buf[0] & 0x80) != 0x80) {
		LOG_ERR("Unexpected response");
		return -EIO;
	}

	write_buf[0] = 0x00;

	ret = ds28e17_i2c_write_read(dev, OPT3001_I2C_ADDR, write_buf, 1, read_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write_read", ret);
		return ret;
	}

	if (illuminance) {
		uint16_t reg = sys_get_be16(&read_buf[0]);
		*illuminance = ((1 << (reg >> 12)) * (reg & 0xfff)) * 0.01f;
	}

	return 0;
}

static int si7210_read(const struct device *dev, float *magnetic_field)
{
	int ret;

	uint8_t write_buf[2];
	uint8_t read_buf[1];

	ret = ds28e17_i2c_read(dev, SI7210_I2C_ADDR, read_buf, 1);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_read", ret);
		return ret;
	}

	write_buf[0] = 0xc4;
	write_buf[1] = 0x04;

	ret = ds28e17_i2c_write(dev, SI7210_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	write_buf[0] = 0xc1;

	ret = ds28e17_i2c_write_read(dev, SI7210_I2C_ADDR, write_buf, 1, read_buf, 1);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write_read", ret);
		return ret;
	}

	uint8_t reg_dspsigm = read_buf[0];

	write_buf[0] = 0xc2;

	ret = ds28e17_i2c_write_read(dev, SI7210_I2C_ADDR, write_buf, 1, read_buf, 1);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write_read", ret);
		return ret;
	}

	uint8_t reg_dspsigl = read_buf[0];

	write_buf[0] = 0xc4;
	write_buf[1] = 0x01;

	ret = ds28e17_i2c_write(dev, SI7210_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	if (magnetic_field) {
		*magnetic_field = ((256 * (reg_dspsigm & 0x7f) + reg_dspsigl) - 16384) * 0.00125f;
	}

	return 0;
}

static int lis2dh12_init(const struct device *dev)
{
	int ret;

	uint8_t write_buf[2];

#define LIS2DH12_WRITE_REG(reg, val)                                                               \
	do {                                                                                       \
		write_buf[0] = reg;                                                                \
		write_buf[1] = val;                                                                \
		ret = ds28e17_i2c_write(dev, LIS2DH12_I2C_ADDR, write_buf, 2);                     \
		if (ret) {                                                                         \
			LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);                         \
			return ret;                                                                \
		}                                                                                  \
	} while (0)

	/* Reboot memory content */
	LIS2DH12_WRITE_REG(LIS2DH12_CTRL_REG5, 0x80);

	k_sleep(K_MSEC(10));

	/* Enable block data update and set scale to +/-4g */
	LIS2DH12_WRITE_REG(LIS2DH12_CTRL_REG4, 0x90);

	/* High-pass filter enabled for INT1 */
	LIS2DH12_WRITE_REG(LIS2DH12_CTRL_REG2, 0x01);

	/* Enable IA1 interrupt on INT1 pin */
	LIS2DH12_WRITE_REG(LIS2DH12_CTRL_REG3, 0x40);

	/* Latch INT1 interrupt request */
	LIS2DH12_WRITE_REG(LIS2DH12_CTRL_REG5, 0x08);

	/* 10 Hz, normal mode, X/Y/Z enabled */
	LIS2DH12_WRITE_REG(LIS2DH12_CTRL_REG1, 0x27);

#undef LIS2DH12_WRITE_REG

	k_sleep(K_MSEC(50));

	return 0;
}

static int lis2dh12_read(const struct device *dev, float *accel_x, float *accel_y, float *accel_z)
{
	int ret;

	uint8_t write_buf[1];
	uint8_t read_buf[7];

	write_buf[0] = 0x80 | LIS2DH12_STATUS_REG;

	ret = ds28e17_i2c_write_read(dev, LIS2DH12_I2C_ADDR, write_buf, 1, read_buf, 7);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write_read", ret);
		return ret;
	}

	if (!(read_buf[0] & BIT(3))) {
		return -ENODATA;
	}

	int16_t sample_x = sys_get_le16(&read_buf[1]);
	int16_t sample_y = sys_get_le16(&read_buf[3]);
	int16_t sample_z = sys_get_le16(&read_buf[5]);

	if (accel_x) {
		*accel_x = sample_x / 8192.f * 9.80665f;
	}

	if (accel_y) {
		*accel_y = sample_y / 8192.f * 9.80665f;
	}

	if (accel_z) {
		*accel_z = sample_z / 8192.f * 9.80665f;
	}

	return 0;
}

static int lis2dh12_enable_alert(const struct device *dev, int threshold, int duration)
{
	int ret;

	uint8_t write_buf[2];
	uint8_t read_buf[1];

	write_buf[0] = LIS2DH12_INT1_THS;
	write_buf[1] = threshold & BIT_MASK(7);

	ret = ds28e17_i2c_write(dev, LIS2DH12_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	write_buf[0] = LIS2DH12_INT1_DURATION;
	write_buf[1] = duration & BIT_MASK(7);

	ret = ds28e17_i2c_write(dev, LIS2DH12_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	write_buf[0] = LIS2DH12_REFERENCE;

	ret = ds28e17_i2c_write_read(dev, LIS2DH12_I2C_ADDR, write_buf, 1, read_buf, 1);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write_read", ret);
		return ret;
	}

	write_buf[0] = LIS2DH12_INT1_CFG;
	write_buf[1] = 0x2a;

	ret = ds28e17_i2c_write(dev, LIS2DH12_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	return 0;
}

static int lis2dh12_disable_alert(const struct device *dev)
{
	int ret;

	uint8_t write_buf[2];

	write_buf[0] = LIS2DH12_INT1_CFG;
	write_buf[1] = 0x00;

	ret = ds28e17_i2c_write(dev, LIS2DH12_I2C_ADDR, write_buf, 2);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write", ret);
		return ret;
	}

	return 0;
}

static int lis2dh12_get_interrupt(const struct device *dev, bool *is_active)
{
	int ret;

	uint8_t write_buf[1];
	uint8_t read_buf[1];

	write_buf[0] = LIS2DH12_INT1_SRC;

	ret = ds28e17_i2c_write_read(dev, LIS2DH12_I2C_ADDR, write_buf, 1, read_buf, 1);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_i2c_write_read", ret);
		return ret;
	}

	*is_active = read_buf[0] & BIT(6) ? true : false;

	return 0;
}

static int scan_callback(struct w1_rom rom, void *user_data)
{
	int ret;

	if (rom.family != 0x19) {
		return 0;
	}

	uint64_t serial_number = sys_get_le48(rom.serial);

	if (m_count >= ARRAY_SIZE(m_sensors)) {
		LOG_WRN("No more space for additional device: %llu", serial_number);
		return 0;
	}

	if (!device_is_ready(m_sensors[m_count].dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	struct w1_slave_config config = {.rom = rom};
	ret = ds28e17_set_w1_config(m_sensors[m_count].dev, config);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_set_w1_config", ret);
		return ret;
	}

	ret = ds28e17_write_config(m_sensors[m_count].dev, DS28E17_I2C_SPEED_100_KHZ);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("ds28e17_write_config", ret);
		return ret;
	}

	ret = lis2dh12_init(m_sensors[m_count].dev);
	if (ret) {
		LOG_DBG("Skipping serial number: %llu", serial_number);
		return 0;
	}

	m_sensors[m_count++].serial_number = serial_number;

	LOG_DBG("Registered serial number: %llu", serial_number);

	return 0;
}

int app_machine_probe_scan(void)
{
	int ret;
	int res = 0;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	static const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));

	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		k_mutex_unlock(&m_lock);
		return -ENODEV;
	}

	ret = app_w1_acquire(&m_w1, dev);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_w1_acquire", ret);
		res = ret;
		goto error;
	}

	m_count = 0;

	ret = app_w1_scan(&m_w1, dev, scan_callback, NULL);
	if (ret < 0) {
		LOG_ERR_CALL_FAILED_INT("app_w1_scan", ret);
		res = ret;
		goto error;
	}

error:
	ret = app_w1_release(&m_w1, dev);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_w1_release", ret);
		res = res ? res : ret;
	}

	k_mutex_unlock(&m_lock);

	return res;
}

int app_machine_probe_get_count(void)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	int count = m_count;
	k_mutex_unlock(&m_lock);

	return count;
}

#define COMM_PROLOGUE                                                                              \
	int ret;                                                                                   \
	int res = 0;                                                                               \
	if (k_is_in_isr()) {                                                                       \
		return -EWOULDBLOCK;                                                               \
	}                                                                                          \
	k_mutex_lock(&m_lock, K_FOREVER);                                                          \
	if (index < 0 || index >= m_count || !m_count) {                                           \
		k_mutex_unlock(&m_lock);                                                           \
		return -ERANGE;                                                                    \
	}                                                                                          \
	static const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));                     \
	if (!device_is_ready(dev)) {                                                               \
		LOG_ERR("Device not ready");                                                       \
		k_mutex_unlock(&m_lock);                                                           \
		return -ENODEV;                                                                    \
	}                                                                                          \
	if (serial_number) {                                                                       \
		*serial_number = m_sensors[index].serial_number;                                   \
	}                                                                                          \
	ret = app_w1_acquire(&m_w1, dev);                                                          \
	if (ret) {                                                                                 \
		LOG_ERR_CALL_FAILED_INT("app_w1_acquire", ret);                                    \
		res = ret;                                                                         \
		goto error;                                                                        \
	}                                                                                          \
	if (!device_is_ready(m_sensors[index].dev)) {                                              \
		LOG_ERR("Device not ready");                                                       \
		res = -ENODEV;                                                                     \
		goto error;                                                                        \
	}                                                                                          \
	ret = ds28e17_write_config(m_sensors[index].dev, DS28E17_I2C_SPEED_100_KHZ);               \
	if (ret) {                                                                                 \
		LOG_ERR_CALL_FAILED_INT("ds28e17_write_config", ret);                              \
		res = ret;                                                                         \
		goto error;                                                                        \
	}

#define COMM_EPILOGUE                                                                              \
error:                                                                                             \
	ret = app_w1_release(&m_w1, dev);                                                          \
	if (ret) {                                                                                 \
		LOG_ERR_CALL_FAILED_INT("app_w1_release", ret);                                    \
		res = res ? res : ret;                                                             \
	}                                                                                          \
	k_mutex_unlock(&m_lock);                                                                   \
	return res;

int app_machine_probe_read_thermometer(int index, uint64_t *serial_number, float *temperature)
{
	if (serial_number) {
		*serial_number = UINT64_MAX;
	}

	if (temperature) {
		*temperature = NAN;
	}

	COMM_PROLOGUE

	if (!res) {
		ret = tmp112_init(m_sensors[index].dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("tmp112_init", ret);
			res = ret;
			goto error;
		}
	}

	k_sleep(TMP112_INIT_TIME);

	if (!res) {
		ret = tmp112_convert(m_sensors[index].dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("tmp112_convert", ret);
			res = ret;
			goto error;
		}
	}

	k_sleep(TMP112_CONV_TIME);

	if (!res) {
		ret = tmp112_read(m_sensors[index].dev, temperature);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("tmp112_read", ret);
			res = ret;
			goto error;
		}
	}

	if (!res && temperature) {
		LOG_DBG("Temperature: %.2f C", (double)*temperature);
	}

	COMM_EPILOGUE
}

int app_machine_probe_read_hygrometer(int index, uint64_t *serial_number, float *temperature,
				      float *humidity)
{
	if (serial_number) {
		*serial_number = UINT64_MAX;
	}

	if (temperature) {
		*temperature = NAN;
	}

	if (humidity) {
		*humidity = NAN;
	}

	COMM_PROLOGUE

	if (!res) {
		ret = sht30_init(m_sensors[index].dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("sht30_init", ret);
			res = ret;
			goto error;
		}
	}

	k_sleep(SHT30_INIT_TIME);

	if (!res) {
		ret = sht30_convert(m_sensors[index].dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("sht30_convert", ret);
			res = ret;
			goto error;
		}
	}

	k_sleep(SHT30_CONV_TIME);

	if (!res) {
		ret = sht30_read(m_sensors[index].dev, temperature, humidity);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("sht30_read", ret);
			res = ret;
			goto error;
		}
	}

	if (!res && temperature) {
		LOG_DBG("Temperature: %.2f C", (double)*temperature);
	}

	if (!res && humidity) {
		LOG_DBG("Humidity: %.1f %%", (double)*humidity);
	}

	COMM_EPILOGUE
}

int app_machine_probe_read_hygrometer_serial(int index, uint64_t *serial_number,
					     uint32_t *sht_serial_number)
{
	if (serial_number) {
		*serial_number = UINT64_MAX;
	}

	if (sht_serial_number) {
		*sht_serial_number = 0;
	}

	COMM_PROLOGUE

	if (!res) {
		ret = sht_read_serial(m_sensors[index].dev, sht_serial_number);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("sht_read_serial", ret);
			res = ret;
			goto error;
		}
	}

	if (!res && sht_serial_number) {
		LOG_DBG("SHT Serial: %u", *sht_serial_number);
	}

	COMM_EPILOGUE
}

int app_machine_probe_read_lux_meter(int index, uint64_t *serial_number, float *illuminance)
{
	if (serial_number) {
		*serial_number = UINT64_MAX;
	}

	if (illuminance) {
		*illuminance = NAN;
	}

	COMM_PROLOGUE

	if (!res) {
		ret = opt3001_init(m_sensors[index].dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("opt3001_init", ret);
			res = ret;
			goto error;
		}
	}

	k_sleep(OPT3001_INIT_TIME);

	if (!res) {
		ret = opt3001_convert(m_sensors[index].dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("opt3001_convert", ret);
			res = ret;
			goto error;
		}
	}

	k_sleep(OPT3001_CONV_TIME);

	if (!res) {
		ret = opt3001_read(m_sensors[index].dev, illuminance);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("opt3001_read", ret);
			res = ret;
			goto error;
		}
	}

	if (!res && illuminance) {
		LOG_DBG("Illuminance: %.0f lux", (double)*illuminance);
	}

	COMM_EPILOGUE
}

int app_machine_probe_read_magnetometer(int index, uint64_t *serial_number, float *magnetic_field)
{
	if (serial_number) {
		*serial_number = UINT64_MAX;
	}

	if (magnetic_field) {
		*magnetic_field = NAN;
	}

	COMM_PROLOGUE

	if (!res) {
		ret = si7210_read(m_sensors[index].dev, magnetic_field);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("si7210_read", ret);
			res = ret;
			goto error;
		}
	}

	if (!res && magnetic_field) {
		LOG_DBG("Magnetic field: %.2f mT", (double)*magnetic_field);
	}

	COMM_EPILOGUE
}

int app_machine_probe_read_accelerometer(int index, uint64_t *serial_number, float *accel_x,
					 float *accel_y, float *accel_z, int *orientation)
{
	if (serial_number) {
		*serial_number = UINT64_MAX;
	}

	if (accel_x) {
		*accel_x = NAN;
	}

	if (accel_y) {
		*accel_y = NAN;
	}

	if (accel_z) {
		*accel_z = NAN;
	}

	if (orientation) {
		*orientation = INT_MAX;
	}

	COMM_PROLOGUE

	if (!res) {
		ret = lis2dh12_read(m_sensors[index].dev, accel_x, accel_y, accel_z);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lis2dh12_read", ret);
			res = ret;
			goto error;
		}
	}

	if (!res && accel_x) {
		LOG_DBG("Acceleration in X-axis: %.3f m/s^2", (double)*accel_x);
	}

	if (!res && accel_y) {
		LOG_DBG("Acceleration in Y-axis: %.3f m/s^2", (double)*accel_y);
	}

	if (!res && accel_z) {
		LOG_DBG("Acceleration in Z-axis: %.3f m/s^2", (double)*accel_z);
	}

	COMM_EPILOGUE

	return 0;
}

int app_machine_probe_enable_tilt_alert(int index, uint64_t *serial_number, int threshold,
					int duration)
{
	if (serial_number) {
		*serial_number = UINT64_MAX;
	}

	COMM_PROLOGUE

	if (!res) {
		ret = lis2dh12_enable_alert(m_sensors[index].dev, threshold, duration);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lis2dh12_enable_alert", ret);
			return ret;
		}
	}

	COMM_EPILOGUE

	return 0;
}

int app_machine_probe_disable_tilt_alert(int index, uint64_t *serial_number)
{
	if (serial_number) {
		*serial_number = UINT64_MAX;
	}

	COMM_PROLOGUE

	if (!res) {
		ret = lis2dh12_disable_alert(m_sensors[index].dev);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lis2dh12_disable_alert", ret);
			return ret;
		}
	}

	COMM_EPILOGUE

	return 0;
}

int app_machine_probe_get_tilt_alert(int index, uint64_t *serial_number, bool *is_active)
{
	if (serial_number) {
		*serial_number = UINT64_MAX;
	}

	if (is_active) {
		*is_active = false;
	}

	COMM_PROLOGUE

	if (!res) {
		ret = lis2dh12_get_interrupt(m_sensors[index].dev, is_active);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lis2dh12_get_interrupt", ret);
			return ret;
		}
	}

	if (!res && is_active) {
		LOG_DBG("Alert active: %s", *is_active ? "true" : "false");
	}

	COMM_EPILOGUE

	return 0;
}
