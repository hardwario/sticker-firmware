/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
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

#define ST25DV_I2C_ADDR_E0 0x53

#define ST25DV_MAX_SEQ_WRITE_BYTES 256
#define ST25DV_PAGE_BYTES          4
#define ST25DV_TW_MS_PER_PAGE      5

static const struct device *const m_i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const struct gpio_dt_spec m_lpd = GPIO_DT_SPEC_GET(DT_NODELABEL(lpd), gpios);

/* Raw ST25DV EEPROM access (mirrors app/src/app_nfc.c). */

static int read_mem(uint16_t reg, void *buf, size_t len)
{
	uint8_t addr[2];

	sys_put_be16(reg, addr);
	return i2c_write_read(m_i2c, ST25DV_I2C_ADDR_E0, addr, sizeof(addr), buf, len);
}

static int write_mem(uint16_t reg, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t remaining = len;

	while (remaining) {
		size_t within = ST25DV_MAX_SEQ_WRITE_BYTES -
				(reg & (ST25DV_MAX_SEQ_WRITE_BYTES - 1));
		size_t chunk = MIN(remaining, within);
		uint8_t frame[2 + ST25DV_MAX_SEQ_WRITE_BYTES];
		int ret;

		sys_put_be16(reg, frame);
		memcpy(&frame[2], p, chunk);

		ret = i2c_write(m_i2c, frame, 2 + chunk, ST25DV_I2C_ADDR_E0);
		if (ret) {
			return ret;
		}

		/* EEPROM page programming time. */
		size_t off_in_page = reg & (ST25DV_PAGE_BYTES - 1);
		size_t pages = DIV_ROUND_UP(off_in_page + chunk, ST25DV_PAGE_BYTES);
		k_msleep(pages * ST25DV_TW_MS_PER_PAGE);

		reg += chunk;
		p += chunk;
		remaining -= chunk;
	}

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

	/* Power the ST25DV (lpd active drives it on; see app_nfc.c). */
	ret = gpio_pin_configure_dt(&m_lpd, GPIO_OUTPUT_ACTIVE);
	if (ret) {
		return ret;
	}
	ret = gpio_pin_set_dt(&m_lpd, 0);
	if (ret) {
		return ret;
	}
	k_msleep(150);
	return 0;
}

int mb_wait_request(uint8_t *frame, size_t cap, size_t *len, int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	uint8_t flag;
	int ret;

	/* Poll PH_FLAG. */
	do {
		ret = read_mem(NFC_MB_PH_FLAG_OFF, &flag, 1);
		if (ret) {
			return ret;
		}
		if (flag == NFC_MB_FLAG_SET) {
			break;
		}
		k_msleep(5);
	} while (k_uptime_get() < deadline);

	if (flag != NFC_MB_FLAG_SET) {
		return -ETIMEDOUT;
	}

	/* Read frame header (type/seq/len), then payload. */
	uint8_t hdr[NFC_REQ_HDR_LEN];

	ret = read_mem(NFC_MB_REQ_OFF, hdr, sizeof(hdr));
	if (ret) {
		return ret;
	}

	size_t dlen = hdr[3];
	size_t total = NFC_REQ_HDR_LEN + dlen;

	if (total > cap || total > NFC_MB_REQ_LEN) {
		return -EMSGSIZE;
	}

	memcpy(frame, hdr, NFC_REQ_HDR_LEN);
	if (dlen) {
		ret = read_mem(NFC_MB_REQ_OFF + NFC_REQ_HDR_LEN, frame + NFC_REQ_HDR_LEN,
			       dlen);
		if (ret) {
			return ret;
		}
	}

	*len = total;
	return 0;
}

int mb_send_response(const uint8_t *rsp, size_t len)
{
	uint8_t flag;
	int ret;

	if (len > NFC_MB_RSP_LEN) {
		return -EMSGSIZE;
	}

	ret = write_mem(NFC_MB_RSP_OFF, rsp, len);
	if (ret) {
		return ret;
	}

	flag = NFC_MB_FLAG_SET;
	ret = write_mem(NFC_MB_MC_FLAG_OFF, &flag, 1);
	if (ret) {
		return ret;
	}

	flag = NFC_MB_FLAG_CLEAR;
	return write_mem(NFC_MB_PH_FLAG_OFF, &flag, 1);
}
