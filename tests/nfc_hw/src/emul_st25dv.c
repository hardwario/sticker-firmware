/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c_emul model of the ST25DV NFC tag chip app_nfc.c talks to directly (no
 * separate Zephyr driver exists for it — app_nfc.c does raw i2c_write_read()/
 * i2c_write() itself, see read_mem()/write_mem()/read_reg()/write_reg()).
 *
 * One physical chip answers on TWO I2C addresses: E0 (0x53, EEPROM memory +
 * dynamic registers, reg >= 0x2000) and E1 (0x57, static system registers,
 * reg < 0x2000) — app_nfc.c's reg_dev_addr() picks the address purely from the
 * register value. Both addresses are registered as separate devicetree nodes
 * (each needs its own struct emul per the i2c_emul framework), but they share
 * ONE global model here (`m_model`, file-scope static) instead of per-instance
 * data, since there is exactly one chip being emulated regardless of how many
 * addresses represent it.
 */

#define DT_DRV_COMPAT hardwario_st25dv_emul

#include "emul_st25dv.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>

#include <errno.h>
#include <string.h>

#define ST25DV_I2C_ADDR_E0 0x53
#define ST25DV_I2C_ADDR_E1 0x57

/* Dynamic registers (E0, reg >= 0x2000) app_nfc.c actually reads/writes. */
#define ST25DV_GPO_CTRL_DYN_REG 0x2000
#define ST25DV_EH_CTRL_DYN      0x2002
#define ST25DV_IT_STS_DYN       0x2005
#define ST25DV_FIELD_ON         0x04
#define ST25DV_IT_RF_WRITE      0x80

/* Static registers (E1, reg < 0x2000). */
#define ST25DV_GPO_REG     0x0000
#define ST25DV_I2C_PWD_REG 0x0900

struct st25dv_model {
	uint8_t mem[ST25DV_EMUL_MEM_SIZE];
	uint8_t gpo_ctrl_dyn;
	uint8_t eh_ctrl_dyn;
	uint8_t it_sts_dyn;
	uint8_t gpo_reg;
	int write_fail_remaining;
};

static struct st25dv_model m_model;

void st25dv_emul_reset(void)
{
	memset(&m_model, 0, sizeof(m_model));
}

void st25dv_emul_mem_get(uint8_t *out, size_t offset, size_t len)
{
	__ASSERT_NO_MSG(offset + len <= sizeof(m_model.mem));
	memcpy(out, &m_model.mem[offset], len);
}

void st25dv_emul_mem_set(const uint8_t *data, size_t offset, size_t len)
{
	__ASSERT_NO_MSG(offset + len <= sizeof(m_model.mem));
	memcpy(&m_model.mem[offset], data, len);
}

void st25dv_emul_set_field_on(bool on)
{
	if (on) {
		m_model.eh_ctrl_dyn |= ST25DV_FIELD_ON;
	} else {
		m_model.eh_ctrl_dyn &= ~ST25DV_FIELD_ON;
	}
}

void st25dv_emul_inject_write_fail(int count)
{
	m_model.write_fail_remaining = count;
}

/* One dynamic/static register byte, addressed generically — every register
 * app_nfc.c touches today is a single byte wide (GPO_REG, GPO_CTRL_DYN_REG,
 * EH_CTRL_DYN, IT_STS_DYN). Extend here if a future fix reads a new one. */
static uint8_t *reg_slot(uint16_t reg)
{
	switch (reg) {
	case ST25DV_GPO_CTRL_DYN_REG:
		return &m_model.gpo_ctrl_dyn;
	case ST25DV_EH_CTRL_DYN:
		return &m_model.eh_ctrl_dyn;
	case ST25DV_IT_STS_DYN:
		return &m_model.it_sts_dyn;
	case ST25DV_GPO_REG:
		return &m_model.gpo_reg;
	default:
		return NULL;
	}
}

/* E0 (memory + dynamic regs) treats reg < 0x2000 as an EEPROM offset and
 * reg >= 0x2000 as a dynamic register; E1 (static regs) is ALWAYS a register
 * lookup regardless of the reg value — reg 0x0000 on E1 (GPO_REG) must not be
 * confused with EEPROM offset 0, which only exists on E0. */
static int st25dv_do_write(int addr, uint16_t reg, const uint8_t *data, size_t len)
{
	if (addr == ST25DV_I2C_ADDR_E1 && reg == ST25DV_I2C_PWD_REG) {
		/* nfc_present_password(): accept unconditionally, no security
		 * modeling needed for these tests. */
		return 0;
	}

	if (addr == ST25DV_I2C_ADDR_E0 && reg < 0x2000) {
		/* EEPROM memory write (write_mem()'s actual tag-content path). */
		if (m_model.write_fail_remaining > 0) {
			m_model.write_fail_remaining--;
			return -EIO;
		}
		if (reg + len > sizeof(m_model.mem)) {
			return -EIO;
		}
		memcpy(&m_model.mem[reg], data, len);
		/* A real tag would set IT_RF_WRITE only for an RF-side write; an
		 * I2C-side write (the device writing to itself) does not set it.
		 * Nothing to do here for that bit. */
		return 0;
	}

	/* Register write (write_reg()): E0 dynamic (reg >= 0x2000) or E1 static. */
	uint8_t *slot = reg_slot(reg);

	if (!slot || len != 1) {
		return -EIO;
	}
	*slot = data[0];
	return 0;
}

static int st25dv_do_read(int addr, uint16_t reg, uint8_t *out, size_t len)
{
	if (addr == ST25DV_I2C_ADDR_E0 && reg < 0x2000) {
		if (reg + len > sizeof(m_model.mem)) {
			return -EIO;
		}
		memcpy(out, &m_model.mem[reg], len);
		return 0;
	}

	uint8_t *slot = reg_slot(reg);

	if (!slot || len != 1) {
		return -EIO;
	}
	out[0] = *slot;
	return 0;
}

/* app_nfc.c's wire shape:
 *   write:                      msgs[0] = [reg_hi, reg_lo, data...]   (1 msg)
 *   read (write+read restart):  msgs[0] = [reg_hi, reg_lo], msgs[1] = read buf (2 msgs)
 */
static int st25dv_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	ARG_UNUSED(target);

	if (num_msgs < 1 || msgs[0].len < 2) {
		return -EIO;
	}

	uint16_t reg = ((uint16_t)msgs[0].buf[0] << 8) | msgs[0].buf[1];

	if (num_msgs == 1) {
		return st25dv_do_write(addr, reg, &msgs[0].buf[2], msgs[0].len - 2);
	}
	if (num_msgs == 2) {
		return st25dv_do_read(addr, reg, msgs[1].buf, msgs[1].len);
	}
	return -EIO;
}

static const struct i2c_emul_api st25dv_emul_api = {
	.transfer = st25dv_transfer,
};

static int st25dv_emul_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);
	return 0;
}

/* Trivial device front-end: nothing in app_nfc.c ever calls DEVICE_DT_GET() on
 * the ST25DV node itself (it only talks to the i2c1 BUS device) — this exists
 * purely because EMUL_DT_DEFINE requires a matching real device at the same
 * node (`.dev = DEVICE_DT_GET(node_id)`). */
static int st25dv_dev_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define ST25DV_EMUL_DEFINE(n)                                                                      \
	DEVICE_DT_INST_DEFINE(n, st25dv_dev_init, NULL, NULL, NULL, POST_KERNEL,                   \
			      CONFIG_I2C_INIT_PRIORITY, NULL);                                     \
	EMUL_DT_INST_DEFINE(n, st25dv_emul_init, NULL, NULL, &st25dv_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(ST25DV_EMUL_DEFINE)
