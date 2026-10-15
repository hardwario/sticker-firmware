/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator: NXP MPL3115A2 barometer on the emulated I2C bus. A
 * register file behind an auto-incrementing pointer; a one-shot (CTRL_REG1
 * OST) latches the set pressure, or the altitude derived from it when ALT is
 * set, and the temperature into OUT_P/OUT_T and raises the data-ready flags. */

#define DT_DRV_COMPAT nxp_mpl3115a2

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
#include <string.h>

#define REG_STATUS    0x00
#define REG_OUT_P_MSB 0x01
#define REG_WHO_AM_I  0x0c
#define REG_CTRL_REG1 0x26
#define REG_COUNT     0x2e

#define WHO_AM_I    0xc4
#define CTRL1_ALT   BIT(7)
#define CTRL1_OST   BIT(1)
#define STATUS_PTDR 0x0e /* TDR | PDR | PTDR */

static float m_pressure = 1013.25f; /* hPa */
static float m_temperature = 21.0f;
static bool m_present = true;
static uint8_t m_regs[REG_COUNT];
static uint8_t m_ptr;
static struct k_spinlock m_lock;

void sim_mpl3115a2_set(float pressure_hpa, float temperature)
{
	K_SPINLOCK(&m_lock) {
		m_pressure = pressure_hpa;
		m_temperature = temperature;
	}
}

void sim_mpl3115a2_set_present(bool present)
{
	K_SPINLOCK(&m_lock) {
		m_present = present;
	}
}

void sim_mpl3115a2_get(float *pressure_hpa, float *temperature, bool *present)
{
	K_SPINLOCK(&m_lock) {
		*pressure_hpa = m_pressure;
		*temperature = m_temperature;
		*present = m_present;
	}
}

/* One-shot conversion: OUT_P is Q18.2 Pa or Q16.4 m, OUT_T Q8.4 °C, both
 * left-justified in the 3 / 2 output bytes. */
static void convert(bool altimeter)
{
	float pa = m_pressure * 100.0f;
	int32_t out_p;

	if (altimeter) {
		/* Datasheet 9.1.3 with the default sea-level pressure (BAR_IN). */
		float alt = 44330.77f * (1.0f - powf(pa / 101326.0f, 0.1902632f));

		out_p = (int32_t)lroundf(alt * 16.0f) << 4;
	} else {
		out_p = (int32_t)lroundf(CLAMP(pa, 0.0f, 262143.75f) * 4.0f) << 4;
	}

	int16_t out_t = (int16_t)lroundf(CLAMP(m_temperature, -128.0f, 127.9f) * 16.0f) << 4;

	sys_put_be24((uint32_t)out_p & 0xffffff, &m_regs[REG_OUT_P_MSB]);
	sys_put_be16((uint16_t)out_t, &m_regs[REG_OUT_P_MSB + 3]);
	m_regs[REG_STATUS] = STATUS_PTDR;
}

static void write_reg(uint8_t reg, uint8_t val)
{
	if (reg >= REG_COUNT || reg == REG_WHO_AM_I) {
		return;
	}
	if (reg == REG_CTRL_REG1 && (val & CTRL1_OST)) {
		convert(val & CTRL1_ALT);
		val &= ~CTRL1_OST; /* clears itself when the conversion is done */
	}
	m_regs[reg] = val;
}

static uint8_t read_reg(uint8_t reg)
{
	uint8_t val = reg < REG_COUNT ? m_regs[reg] : 0;

	/* Reading OUT_P_MSB acknowledges the data-ready flags. */
	if (reg == REG_OUT_P_MSB) {
		m_regs[REG_STATUS] = 0;
	}

	return val;
}

static int emul_mpl3115a2_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
				   int addr)
{
	ARG_UNUSED(target);
	ARG_UNUSED(addr);
	int ret = 0;

	K_SPINLOCK(&m_lock) {
		if (!m_present) {
			ret = -EIO;
			K_SPINLOCK_BREAK;
		}
		for (int i = 0; i < num_msgs; i++) {
			if (msgs[i].flags & I2C_MSG_READ) {
				for (uint32_t j = 0; j < msgs[i].len; j++) {
					msgs[i].buf[j] = read_reg(m_ptr++);
				}
			} else if (msgs[i].len > 0) {
				m_ptr = msgs[i].buf[0];
				for (uint32_t j = 1; j < msgs[i].len; j++) {
					write_reg(m_ptr++, msgs[i].buf[j]);
				}
			}
		}
	}

	return ret;
}

static const struct i2c_emul_api emul_mpl3115a2_api = {
	.transfer = emul_mpl3115a2_transfer,
};

static int emul_mpl3115a2_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);

	memset(m_regs, 0, sizeof(m_regs));
	m_regs[REG_WHO_AM_I] = WHO_AM_I;

	return 0;
}

EMUL_DT_INST_DEFINE(0, emul_mpl3115a2_init, NULL, NULL, &emul_mpl3115a2_api, NULL);
