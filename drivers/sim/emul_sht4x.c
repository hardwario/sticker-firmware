/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator: Sensirion SHT4x on the emulated I2C bus. A command byte
 * selects what the next read returns: a measurement (any measure command),
 * the serial number (0x89); soft reset (0x94) has no data. Words are big
 * endian with the Sensirion CRC-8 (poly 0x31, init 0xFF). */

#define DT_DRV_COMPAT sensirion_sht4x

#include "sim.h"

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <math.h>
#include <string.h>

#define SHT4X_CMD_SERIAL 0x89
#define SHT4X_CMD_RESET  0x94
#define SHT4X_SERIAL     0x12345678u

static float m_temperature = 23.0f;
static float m_humidity = 45.0f;
static bool m_present = true;
static uint8_t m_cmd;
static struct k_spinlock m_lock;

void sim_sht4x_set(float temperature, float humidity)
{
	K_SPINLOCK(&m_lock) {
		m_temperature = temperature;
		m_humidity = humidity;
	}
}

void sim_sht4x_set_present(bool present)
{
	K_SPINLOCK(&m_lock) {
		m_present = present;
	}
}

void sim_sht4x_get(float *temperature, float *humidity, bool *present)
{
	K_SPINLOCK(&m_lock) {
		*temperature = m_temperature;
		*humidity = m_humidity;
		*present = m_present;
	}
}

static void put_word(uint8_t *buf, uint16_t word)
{
	buf[0] = word >> 8;
	buf[1] = word & 0xff;
	buf[2] = crc8(buf, 2, 0x31, 0xff, false);
}

static uint16_t to_ticks(float value, float offset, float span)
{
	float ticks = roundf((value - offset) * 65535.0f / span);

	return (uint16_t)CLAMP(ticks, 0.0f, 65535.0f);
}

static void fill_read(uint8_t *buf, uint32_t len)
{
	uint8_t data[6] = {0};
	float t, rh;
	bool present;

	sim_sht4x_get(&t, &rh, &present);

	if (m_cmd == SHT4X_CMD_SERIAL) {
		put_word(&data[0], SHT4X_SERIAL >> 16);
		put_word(&data[3], SHT4X_SERIAL & 0xffff);
	} else {
		/* T = -45 + 175 * S / 65535, RH = -6 + 125 * S / 65535 */
		put_word(&data[0], to_ticks(t, -45.0f, 175.0f));
		put_word(&data[3], to_ticks(rh, -6.0f, 125.0f));
	}

	memcpy(buf, data, MIN(len, sizeof(data)));
}

static int emul_sht4x_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
			       int addr)
{
	ARG_UNUSED(target);
	ARG_UNUSED(addr);
	bool present;

	K_SPINLOCK(&m_lock) {
		present = m_present;
	}
	if (!present) {
		return -EIO;
	}

	for (int i = 0; i < num_msgs; i++) {
		if (msgs[i].flags & I2C_MSG_READ) {
			fill_read(msgs[i].buf, msgs[i].len);
		} else if (msgs[i].len > 0) {
			m_cmd = msgs[i].buf[0];
		}
	}

	return 0;
}

static const struct i2c_emul_api emul_sht4x_api = {
	.transfer = emul_sht4x_transfer,
};

static int emul_sht4x_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);

	return 0;
}

EMUL_DT_INST_DEFINE(0, emul_sht4x_init, NULL, NULL, &emul_sht4x_api, NULL);
