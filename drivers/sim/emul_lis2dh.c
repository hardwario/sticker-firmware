/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* STICKER simulator: ST LIS2DH12 accelerometer on the emulated I2C bus.
 *
 * A register file (sub-address bit 7 = auto-increment) whose OUT_X..OUT_Z
 * return the set acceleration in the scale the Zephyr driver expects for the
 * configured full scale. Both interrupt generators are evaluated when the
 * acceleration changes (a new sample; the chip compares only at its ODR, so
 * the app's arm sequence - CFG, then HPF + REFERENCE - sees no event):
 *   - INT1_CFG/INT2_CFG high/low events per axis, OR or AND (AOI), against
 *     INTx_THS in full-scale dependent LSBs; duration and 6D are ignored;
 *   - with the high-pass filter on a generator's path (CTRL2 HP_IA1/HP_IA2)
 *     it sees the change since the last REFERENCE read (the app snaps the
 *     reference when it arms motion detection); the filter then settles;
 *   - routed to its pin (CTRL3 I1_IA1, CTRL6 I2_IA2) the event raises
 *     irq-gpios[0] (INT1) / irq-gpios[1] (INT2); a latched one (CTRL5 LIR)
 *     holds the pin until INTx_SRC is read, otherwise the pin follows the
 *     condition (a filtered event is one pulse). */

#define DT_DRV_COMPAT st_lis2dh

#include "sim.h"

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <math.h>
#include <string.h>

#define REG_WHO_AM_I  0x0f
#define REG_CTRL1     0x20
#define REG_CTRL2     0x21
#define REG_CTRL3     0x22
#define REG_CTRL4     0x23
#define REG_CTRL5     0x24
#define REG_CTRL6     0x25
#define REG_REFERENCE 0x26
#define REG_STATUS    0x27
#define REG_OUT_X_L   0x28
#define REG_INT1_CFG  0x30
#define REG_INT1_SRC  0x31
#define REG_INT1_THS  0x32
#define REG_INT2_CFG  0x34
#define REG_INT2_SRC  0x35
#define REG_INT2_THS  0x36
#define REG_COUNT     0x40

#define WHO_AM_I     0x33
#define AUTO_INC     BIT(7)
#define STATUS_ZYXDA 0x0f
#define INT_CFG_AOI  BIT(7)
#define INT_CFG_6D   BIT(6)
#define INT_SRC_IA   BIT(6)
#define STANDARD_G   9.80665f

struct generator {
	uint8_t cfg, src, ths;    /* register addresses */
	uint8_t hp_bit;           /* CTRL2 */
	uint8_t route_reg, route; /* CTRL3 I1_IA1 / CTRL6 I2_IA2 */
	uint8_t lir;              /* CTRL5 */
	int pin;                  /* irq-gpios index */
};

static const struct generator m_gen[2] = {
	{REG_INT1_CFG, REG_INT1_SRC, REG_INT1_THS, BIT(0), REG_CTRL3, BIT(6), BIT(3), 0},
	{REG_INT2_CFG, REG_INT2_SRC, REG_INT2_THS, BIT(1), REG_CTRL6, BIT(5), BIT(1), 1},
};

static const struct gpio_dt_spec m_irq[2] = {
	GPIO_DT_SPEC_GET_BY_IDX(DT_DRV_INST(0), irq_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(DT_DRV_INST(0), irq_gpios, 1),
};

/* Full-scale dependent: Zephyr driver scale (low-power sensitivity x 100,
 * µm/s^2 per 12-bit LSB after its own formula) and INTx_THS LSB in mg. */
static const uint32_t m_sensitivity[4] = {1600, 3200, 6400, 19200};
static const float m_ths_mg[4] = {16.0f, 32.0f, 62.0f, 186.0f};

static float m_accel[3] = {0.0f, 0.0f, STANDARD_G}; /* m/s^2, lying flat */
static float m_ref[3];
static bool m_present = true;
static uint8_t m_regs[REG_COUNT];
static uint8_t m_ptr;
static int m_pin_level[2];
static uint32_t m_events[2];
static struct k_spinlock m_lock;

static int full_scale(void)
{
	return (m_regs[REG_CTRL4] >> 4) & 0x3;
}

static void fill_output(void)
{
	uint32_t scale = (uint32_t)(((uint64_t)9806650 * m_sensitivity[full_scale()]) >> 14) / 100;

	for (int i = 0; i < 3; i++) {
		long raw = lroundf(m_accel[i] * 1e6f / (float)scale);
		int16_t out = (int16_t)(CLAMP(raw, -2048, 2047) * 16);

		m_regs[REG_OUT_X_L + 2 * i] = (uint8_t)out;
		m_regs[REG_OUT_X_L + 2 * i + 1] = (uint8_t)((uint16_t)out >> 8);
	}
	m_regs[REG_STATUS] = (m_regs[REG_CTRL1] & 0x07) ? STATUS_ZYXDA : 0;
}

/* Evaluate one generator; returns the level its pin should have, or -1 when
 * the pin is not touched. */
static int evaluate(int g)
{
	const struct generator *gen = &m_gen[g];
	uint8_t cfg = m_regs[gen->cfg];
	uint8_t enabled = cfg & 0x3f;
	bool hpf = m_regs[REG_CTRL2] & gen->hp_bit;
	bool latched = m_regs[REG_CTRL5] & gen->lir;
	bool routed = m_regs[gen->route_reg] & gen->route;
	float ths = m_regs[gen->ths] * m_ths_mg[full_scale()] * STANDARD_G / 1000.0f;
	uint8_t hit = 0;

	for (int i = 0; i < 3; i++) {
		float v = fabsf(hpf ? m_accel[i] - m_ref[i] : m_accel[i]);

		hit |= (v > ths ? BIT(2 * i + 1) : BIT(2 * i)) & enabled;
	}
	if (hpf) {
		memcpy(m_ref, m_accel, sizeof(m_ref)); /* the filter settles */
	}

	bool active = enabled && ((cfg & INT_CFG_AOI) ? hit == enabled : hit != 0);

	if (active && !(latched && (m_regs[gen->src] & INT_SRC_IA))) {
		m_regs[gen->src] = INT_SRC_IA | hit;
		m_events[g]++;
	} else if (!active && !latched) {
		m_regs[gen->src] = 0;
	}

	if (!routed) {
		return -1;
	}
	if (latched) {
		return (m_regs[gen->src] & INT_SRC_IA) ? 1 : 0;
	}

	return active ? (hpf ? 2 /* pulse */ : 1) : 0;
}

static void evaluate_all(int level[2])
{
	for (int g = 0; g < 2; g++) {
		level[g] = evaluate(g);
	}
}

/* Drive the INT pins, outside the lock (the port runs the callbacks). */
static void drive_pins(const int level[2])
{
	for (int g = 0; g < 2; g++) {
		const struct gpio_dt_spec *pin = &m_irq[m_gen[g].pin];

		if (level[g] == 2) {
			(void)sim_gpio_input_set(pin->port, pin->pin, 1);
			(void)sim_gpio_input_set(pin->port, pin->pin, 0);
			m_pin_level[g] = 0;
		} else if (level[g] >= 0 && level[g] != m_pin_level[g]) {
			(void)sim_gpio_input_set(pin->port, pin->pin, level[g]);
			m_pin_level[g] = level[g];
		}
	}
}

void sim_lis2dh_set(float x, float y, float z)
{
	int level[2];

	K_SPINLOCK(&m_lock) {
		m_accel[0] = x;
		m_accel[1] = y;
		m_accel[2] = z;
		fill_output();
		evaluate_all(level);
	}
	drive_pins(level);
}

void sim_lis2dh_set_present(bool present)
{
	K_SPINLOCK(&m_lock) {
		m_present = present;
	}
}

void sim_lis2dh_get(float accel[3], bool *present, uint32_t events[2])
{
	K_SPINLOCK(&m_lock) {
		memcpy(accel, m_accel, sizeof(m_accel));
		*present = m_present;
		events[0] = m_events[0];
		events[1] = m_events[1];
	}
}

static uint8_t read_reg(uint8_t reg, int level[2])
{
	uint8_t val = m_regs[reg];

	if (reg == REG_REFERENCE) {
		memcpy(m_ref, m_accel, sizeof(m_ref));
	}
	for (int g = 0; g < 2; g++) {
		if (reg == m_gen[g].src && (m_regs[REG_CTRL5] & m_gen[g].lir)) {
			m_regs[reg] = 0;
			if (m_regs[m_gen[g].route_reg] & m_gen[g].route) {
				level[g] = 0;
			}
		}
	}

	return val;
}

static void write_reg(uint8_t reg, uint8_t val)
{
	if (reg == REG_WHO_AM_I || reg == REG_STATUS || reg == REG_INT1_SRC ||
	    reg == REG_INT2_SRC || (reg >= REG_OUT_X_L && reg < REG_OUT_X_L + 6)) {
		return;
	}
	m_regs[reg] = val;
}

static int emul_lis2dh_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs,
				int addr)
{
	ARG_UNUSED(target);
	ARG_UNUSED(addr);
	int level[2] = {-1, -1};
	bool wrote = false;
	int ret = 0;

	K_SPINLOCK(&m_lock) {
		if (!m_present) {
			ret = -EIO;
			K_SPINLOCK_BREAK;
		}
		for (int i = 0; i < num_msgs; i++) {
			struct i2c_msg *msg = &msgs[i];

			if (msg->flags & I2C_MSG_READ) {
				for (uint32_t j = 0; j < msg->len; j++) {
					msg->buf[j] = read_reg(m_ptr & 0x3f, level);
					m_ptr = (m_ptr & AUTO_INC) ? m_ptr + 1 : m_ptr;
				}
			} else if (msg->len > 0) {
				m_ptr = msg->buf[0];
				for (uint32_t j = 1; j < msg->len; j++) {
					write_reg(m_ptr & 0x3f, msg->buf[j]);
					m_ptr = (m_ptr & AUTO_INC) ? m_ptr + 1 : m_ptr;
					wrote = true;
				}
			}
		}
		if (wrote) {
			fill_output(); /* the full scale may have changed */
		}
	}
	drive_pins(level);

	return ret;
}

static const struct i2c_emul_api emul_lis2dh_api = {
	.transfer = emul_lis2dh_transfer,
};

static int emul_lis2dh_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);

	memset(m_regs, 0, sizeof(m_regs));
	m_regs[REG_WHO_AM_I] = WHO_AM_I;
	m_regs[REG_CTRL1] = 0x07;
	fill_output();

	return 0;
}

EMUL_DT_INST_DEFINE(0, emul_lis2dh_init, NULL, NULL, &emul_lis2dh_api, NULL);
