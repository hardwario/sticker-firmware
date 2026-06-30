/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ST25DV FTM (Fast-Transfer-Mode) mailbox transport for the NFC bootloader.
 * Mirrors the HW-proven application path in app/src/app_nfc.c: a single 256-byte
 * volatile RAM buffer is shared half-duplex between the RF (phone) and I2C (MCU)
 * sides. No EEPROM wear, no 5 ms page-programming wait — a whole frame moves in
 * one I2C transaction. See doc/nfc-update-protocol.md §4.
 */

#include "st25dv_mb.h"

#include <sticker/nfc_proto.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <errno.h>
#include <string.h>

/* ST25DV I2C device addresses: E0 (0x53) = user memory + dynamic registers
 * (>=0x2000); E1 (0x57) = static system config (<0x2000) + password session. */
#define ST25DV_I2C_ADDR_E0 0x53
#define ST25DV_I2C_ADDR_E1 0x57

#define ST25DV_I2C_PWD_REG 0x0900 /* present-password register (E1) */

/* Fast-Transfer-Mode mailbox registers (see app_nfc.c for the full notes):
 * MB_MODE (static, E1 0x000D) bit0 must be 1 to allow the mailbox at all;
 * MB_CTRL_Dyn (dynamic, E0 0x2006) MB_EN turns it on, RF_PUT/HOST_PUT report
 * who wrote last; MB_LEN_Dyn (E0 0x2007) holds (message length - 1); the message
 * bytes live in MAILBOX_RAM (E0 0x2008..0x2107). */
#define ST25DV_MB_MODE_REG      0x000D
#define ST25DV_MB_MODE_EN       0x01
#define ST25DV_MB_CTRL_DYN      0x2006
#define ST25DV_MB_CTRL_MB_EN    0x01 /* bit0: mailbox enable */
#define ST25DV_MB_CTRL_HOST_PUT 0x02 /* bit1: host (I2C) wrote a message */
#define ST25DV_MB_CTRL_RF_PUT   0x04 /* bit2: RF (phone) wrote a message */
#define ST25DV_MB_LEN_DYN       0x2007
#define ST25DV_MB_RAM           0x2008

#define ST25DV_TW_MS_PER_PAGE 5 /* EEPROM page programming time */

static const struct device *const m_i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const struct gpio_dt_spec m_lpd = GPIO_DT_SPEC_GET(DT_NODELABEL(lpd), gpios);

/* Dynamic registers (>=0x2000) live on E0; the static system config on E1. */
static inline uint8_t reg_dev_addr(uint16_t reg)
{
	return (reg >= 0x2000) ? ST25DV_I2C_ADDR_E0 : ST25DV_I2C_ADDR_E1;
}

static int read_reg(uint16_t reg, void *buf, size_t len)
{
	uint8_t addr[2];

	sys_put_be16(reg, addr);
	return i2c_write_read(m_i2c, reg_dev_addr(reg), addr, sizeof(addr), buf, len);
}

static int write_reg(uint16_t reg, const void *buf, size_t len)
{
	uint8_t frame[2 + 16];

	if (len > 16) {
		return -EINVAL;
	}
	sys_put_be16(reg, frame);
	memcpy(&frame[2], buf, len);
	return i2c_write(m_i2c, frame, 2 + len, reg_dev_addr(reg));
}

/* Read mailbox RAM (E0, up to 256 bytes in one transaction). */
static int read_mem(uint16_t reg, void *buf, size_t len)
{
	uint8_t addr[2];

	sys_put_be16(reg, addr);
	return i2c_write_read(m_i2c, ST25DV_I2C_ADDR_E0, addr, sizeof(addr), buf, len);
}

/* Open the I2C security session by presenting an 8-byte password (required to
 * write the static MB_MODE bit). Frame: addr(2) + pwd(8) + 0x09 + pwd(8). */
static int present_password(const uint8_t pwd[8])
{
	uint8_t frame[2 + 8 + 1 + 8];

	sys_put_be16(ST25DV_I2C_PWD_REG, frame);
	memcpy(&frame[2], pwd, 8);
	frame[10] = 0x09;
	memcpy(&frame[11], pwd, 8);
	return i2c_write(m_i2c, frame, sizeof(frame), ST25DV_I2C_ADDR_E1);
}

/* Write the whole mailbox in one I2C transaction (volatile RAM, no program
 * wait). Writing MAILBOX_RAM arms HOST_PUT_MSG so the RF side can read it. */
static int mb_write(const uint8_t *data, size_t len)
{
	uint8_t frame[2 + NFC_MB_RAM_SIZE];

	if (len == 0 || len > NFC_MB_RAM_SIZE) {
		return -EINVAL;
	}
	sys_put_be16(ST25DV_MB_RAM, frame);
	memcpy(&frame[2], data, len);
	return i2c_write(m_i2c, frame, 2 + len, ST25DV_I2C_ADDR_E0);
}

/* Enable the mailbox: set the static MB_MODE allow-bit (needs the I2C password)
 * then the dynamic MB_EN. The default factory password is all-zero. */
static int mb_enable(void)
{
	static const uint8_t default_pwd[8] = {0};
	uint8_t mode = 0;
	uint8_t ctrl;
	int ret;

	ret = present_password(default_pwd);
	if (ret) {
		return ret;
	}
	k_msleep(15);

	ret = read_reg(ST25DV_MB_MODE_REG, &mode, 1);
	if (ret) {
		return ret;
	}
	if (!(mode & ST25DV_MB_MODE_EN)) {
		mode |= ST25DV_MB_MODE_EN;
		ret = write_reg(ST25DV_MB_MODE_REG, &mode, 1);
		if (ret) {
			return ret;
		}
		k_msleep(ST25DV_TW_MS_PER_PAGE + 5);
	}

	ctrl = ST25DV_MB_CTRL_MB_EN;
	ret = write_reg(ST25DV_MB_CTRL_DYN, &ctrl, 1);
	if (ret) {
		return ret;
	}
	k_msleep(1);
	return 0;
}

int mb_init(void)
{
	int ret;

	if (!device_is_ready(m_i2c)) {
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&m_lpd)) {
		return -ENODEV;
	}

	/* Power the ST25DV (lpd active drives it on; see app_nfc.c) and keep it on. */
	ret = gpio_pin_configure_dt(&m_lpd, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		return ret;
	}
	ret = gpio_pin_set_dt(&m_lpd, 0);
	if (ret) {
		return ret;
	}
	k_msleep(150);

	return mb_enable();
}

int mb_wait_request(uint8_t *frame, size_t cap, size_t *len, int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	uint8_t ctrl = 0;
	uint8_t lenm1;
	int ret;

	/* Poll MB_CTRL_Dyn for RF_PUT (the phone wrote a request). */
	do {
		ret = read_reg(ST25DV_MB_CTRL_DYN, &ctrl, 1);
		if (ret) {
			return ret;
		}
		if (ctrl & ST25DV_MB_CTRL_RF_PUT) {
			break;
		}
		k_msleep(10);
	} while (k_uptime_get() < deadline);

	if (!(ctrl & ST25DV_MB_CTRL_RF_PUT)) {
		return -ETIMEDOUT;
	}

	/* MB_LEN_Dyn holds (length - 1) of the message just written by RF. */
	ret = read_reg(ST25DV_MB_LEN_DYN, &lenm1, 1);
	if (ret) {
		return ret;
	}

	size_t total = (size_t)lenm1 + 1;

	if (total > cap || total > NFC_MB_RAM_SIZE) {
		return -EMSGSIZE;
	}

	ret = read_mem(ST25DV_MB_RAM, frame, total);
	if (ret) {
		return ret;
	}

	*len = total;
	return 0;
}

int mb_send_response(const uint8_t *rsp, size_t len)
{
	if (len == 0 || len > NFC_MB_RAM_SIZE) {
		return -EMSGSIZE;
	}
	/* Writing the mailbox arms HOST_PUT_MSG; the phone polls and reads it. */
	return mb_write(rsp, len);
}
