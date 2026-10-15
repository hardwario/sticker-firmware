/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator: TI OPT3001 on the emulated I2C bus. 16-bit big-endian
 * registers behind a pointer byte; the result register encodes the set lux as
 * exponent/mantissa (lux = 0.01 * 2^E * M). */

#define DT_DRV_COMPAT ti_opt3001

#include "sim.h"

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <math.h>

#define REG_RESULT    0x00
#define REG_CONFIG    0x01
#define REG_LOW_LIMIT 0x02
#define REG_HIGH_LIM  0x03
#define REG_MFR_ID    0x7e
#define REG_DEV_ID    0x7f

#define CONFIG_CRF BIT(7) /* conversion ready */

static float m_lux = 250.0f;
static bool m_present = true;
static uint8_t m_ptr;
static uint16_t m_config = 0xc810; /* power-on default */
static uint16_t m_limits[2] = {0x0000, 0xbfff};
static struct k_spinlock m_lock;

void sim_opt3001_set(float lux)
{
	K_SPINLOCK(&m_lock) {
		m_lux = lux;
	}
}

void sim_opt3001_set_present(bool present)
{
	K_SPINLOCK(&m_lock) {
		m_present = present;
	}
}

void sim_opt3001_get(float *lux, bool *present)
{
	K_SPINLOCK(&m_lock) {
		*lux = m_lux;
		*present = m_present;
	}
}

static uint16_t encode_lux(float lux)
{
	uint32_t m = (uint32_t)roundf(MAX(lux, 0.0f) * 100.0f);
	uint16_t e = 0;

	while (m > 0xfff && e < 11) {
		m >>= 1;
		e++;
	}

	return (e << 12) | MIN(m, 0xfffu);
}

static uint16_t reg_read(uint8_t reg)
{
	float lux;
	bool present;

	switch (reg) {
	case REG_RESULT:
		sim_opt3001_get(&lux, &present);
		return encode_lux(lux);
	case REG_CONFIG:
		return m_config | CONFIG_CRF;
	case REG_LOW_LIMIT:
	case REG_HIGH_LIM:
		return m_limits[reg - REG_LOW_LIMIT];
	case REG_MFR_ID:
		return 0x5449;
	case REG_DEV_ID:
		return 0x3001;
	default:
		return 0;
	}
}

static void reg_write(uint8_t reg, uint16_t value)
{
	if (reg == REG_CONFIG) {
		m_config = value & ~CONFIG_CRF;
	} else if (reg == REG_LOW_LIMIT || reg == REG_HIGH_LIM) {
		m_limits[reg - REG_LOW_LIMIT] = value;
	}
}

static int emul_opt3001_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
				 int addr)
{
	ARG_UNUSED(target);
	ARG_UNUSED(addr);
	float lux;
	bool present;

	sim_opt3001_get(&lux, &present);
	if (!present) {
		return -EIO;
	}

	for (int i = 0; i < num_msgs; i++) {
		struct i2c_msg *msg = &msgs[i];

		if (msg->flags & I2C_MSG_READ) {
			uint16_t v = reg_read(m_ptr);

			for (uint32_t j = 0; j < msg->len; j++) {
				msg->buf[j] = (j & 1) ? (v & 0xff) : (v >> 8);
			}
		} else if (msg->len > 0) {
			m_ptr = msg->buf[0];
			if (msg->len >= 3) {
				reg_write(m_ptr, sys_get_be16(&msg->buf[1]));
			}
		}
	}

	return 0;
}

static const struct i2c_emul_api emul_opt3001_api = {
	.transfer = emul_opt3001_transfer,
};

static int emul_opt3001_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);

	return 0;
}

EMUL_DT_INST_DEFINE(0, emul_opt3001_init, NULL, NULL, &emul_opt3001_api, NULL);
