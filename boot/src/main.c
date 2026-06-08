/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * STICKER NFC bootloader (variant B — erase-in-place).
 * Boot decision + jump, and the DFU receive loop over the ST25DV mailbox.
 * See doc/nfc-update-protocol.md. WIP scaffold — not yet HW-tested.
 */

#include "flash_writer.h"
#include "st25dv_mb.h"
#include "verify.h"

#include <sticker/nfc_proto.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include <cmsis_core.h>

#include <string.h>

#define DFU_SESSION_TIMEOUT_MS 30000

/* Force-DFU gesture: both hall magnets held at reset (active-low). */
static const struct gpio_dt_spec m_hall_l = GPIO_DT_SPEC_GET(DT_NODELABEL(hall_l), gpios);
static const struct gpio_dt_spec m_hall_r = GPIO_DT_SPEC_GET(DT_NODELABEL(hall_r), gpios);

static bool dfu_forced(void)
{
	if (!gpio_is_ready_dt(&m_hall_l) || !gpio_is_ready_dt(&m_hall_r)) {
		return false;
	}
	(void)gpio_pin_configure_dt(&m_hall_l, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&m_hall_r, GPIO_INPUT);
	k_busy_wait(50);
	return gpio_pin_get_dt(&m_hall_l) == 1 && gpio_pin_get_dt(&m_hall_r) == 1;
}

static bool slot_is_bootable(void)
{
	struct sfu_meta meta;

	if (meta_read(&meta) != 0) {
		return false;
	}
	return meta.magic == SFU_META_MAGIC && meta.valid == SFU_META_MAGIC;
}

static void jump_to_app(uint32_t base)
{
	uint32_t *vt = (uint32_t *)base;
	uint32_t sp = vt[0];
	uint32_t pc = vt[1];

	__disable_irq();
	SysTick->CTRL = 0;
	SysTick->VAL = 0;
	SCB->VTOR = base;
	__DSB();
	__ISB();
	__set_MSP(sp);
	((void (*)(void))pc)();

	CODE_UNREACHABLE;
}

/* ---- DFU receive loop ----------------------------------------------- */

static struct sfu_header m_hdr;
static uint8_t m_sig[SFU_SIGNATURE_LEN];
static bool m_started;

static size_t build_status(uint8_t *out, uint8_t status, uint16_t ctx)
{
	out[0] = status;
	out[1] = ctx & 0xFF;
	out[2] = (ctx >> 8) & 0xFF;
	return 3;
}

static uint16_t handle_start(const uint8_t *data, size_t len)
{
	if (len < SFU_PREAMBLE_LEN) {
		return NFC_ST_ERR_MAGIC;
	}
	memcpy(&m_hdr, data, SFU_HEADER_LEN);
	memcpy(m_sig, data + SFU_HEADER_LEN, SFU_SIGNATURE_LEN);

	if (!verify_header(&m_hdr, fw_slot0_size())) {
		return (m_hdr.payload_len > fw_slot0_size()) ? NFC_ST_ERR_SIZE
							     : NFC_ST_ERR_MAGIC;
	}
	if (fw_erase_slot() != 0) {
		return NFC_ST_ERR_FLASH;
	}
	m_started = true;
	return NFC_ST_READY;
}

static uint16_t handle_data(uint16_t seq, const uint8_t *data, size_t len)
{
	if (!m_started) {
		return NFC_ST_ERR_STATE;
	}
	uint32_t off = (uint32_t)seq * NFC_MAX_DATA;

	if (fw_write(off, data, len) != 0) {
		return NFC_ST_ERR_FLASH;
	}
	return NFC_ST_ACK;
}

static uint16_t handle_finish(void)
{
	uint32_t crc = 0;

	if (!m_started) {
		return NFC_ST_ERR_STATE;
	}
	if (fw_slot_crc32(m_hdr.payload_len, &crc) != 0) {
		return NFC_ST_ERR_FLASH;
	}
	if (crc != m_hdr.payload_crc32) {
		return NFC_ST_ERR_VERIFY;
	}
	if (!verify_signature(&m_hdr, m_sig)) {
		return NFC_ST_ERR_VERIFY;
	}

	struct sfu_meta meta = {
		.magic = SFU_META_MAGIC,
		.payload_len = m_hdr.payload_len,
		.payload_crc32 = m_hdr.payload_crc32,
		.valid = SFU_META_MAGIC,
	};
	if (meta_write(&meta) != 0) {
		return NFC_ST_ERR_FLASH;
	}
	return NFC_ST_OK;
}

static void dfu_loop(void)
{
	uint8_t frame[NFC_MB_REQ_LEN];
	uint8_t rsp[NFC_MB_RSP_LEN];

	for (;;) {
		size_t len = 0;
		int ret = mb_wait_request(frame, sizeof(frame), &len, DFU_SESSION_TIMEOUT_MS);

		if (ret == -ETIMEDOUT) {
			/* No activity — stay in DFU-wait, keep recoverable. */
			continue;
		}
		if (ret != 0 || len < NFC_REQ_HDR_LEN) {
			continue;
		}

		uint8_t type = frame[0];
		uint16_t seq = frame[1] | (frame[2] << 8);
		uint8_t dlen = frame[3];
		const uint8_t *data = frame + NFC_REQ_HDR_LEN;
		uint16_t status;
		uint16_t ctx = 0;

		switch (type) {
		case NFC_CMD_PING:
			status = NFC_ST_READY;
			break;
		case NFC_CMD_START:
			status = handle_start(data, dlen);
			break;
		case NFC_CMD_DATA:
			status = handle_data(seq, data, dlen);
			ctx = seq;
			break;
		case NFC_CMD_FINISH:
			status = handle_finish();
			break;
		case NFC_CMD_ABORT:
			m_started = false;
			status = NFC_ST_READY;
			break;
		default:
			status = NFC_ST_ERR_STATE;
			break;
		}

		size_t rlen = build_status(rsp, status, ctx);
		(void)mb_send_response(rsp, rlen);

		if (status == NFC_ST_OK) {
			k_msleep(50);
			sys_reboot(SYS_REBOOT_COLD);
		}
	}
}

int main(void)
{
	printk("STICKER NFC bootloader\n");

	bool bootable = slot_is_bootable();
	bool forced = dfu_forced();

	if (bootable && !forced) {
		jump_to_app(fw_slot0_base());
	}

	printk("Entering DFU mode (bootable=%d forced=%d)\n", bootable, forced);

	if (mb_init() != 0) {
		printk("ST25DV init failed\n");
		/* Without NFC we cannot recover; spin so a debugger can attach. */
		for (;;) {
			k_msleep(1000);
		}
	}

	dfu_loop();
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
