/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * STICKER NFC bootloader (variant B — erase-in-place).
 * Boot decision + jump, and the DFU receive loop over the ST25DV mailbox.
 * See doc/nfc-update-protocol.md. WIP scaffold — not yet HW-tested.
 */

#include "auth.h"
#include "flash_writer.h"
#include "st25dv_mb.h"
#include "verify.h"

#include <sticker/dfu_signal.h>
#include <sticker/nfc_proto.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include <cmsis_core.h>

#include <string.h>

#define DFU_SESSION_TIMEOUT_MS 30000

/*
 * A vector table at slot0 looks like a runnable image when the initial stack
 * pointer lands in SRAM and the reset vector points (thumb) into the slot.
 * Lets a JLink-provisioned app (no sfu_meta yet) boot; a blank/erased slot
 * (0xFFFFFFFF) fails the check and drops into DFU.
 */
static bool slot0_looks_valid(void)
{
	uint32_t vt[2];
	const uint32_t base = fw_slot0_base();

	if (fw_read(0, (uint8_t *)vt, sizeof(vt)) != 0) {
		return false;
	}

	const uint32_t sram_lo = CONFIG_SRAM_BASE_ADDRESS;
	const uint32_t sram_hi = sram_lo + (CONFIG_SRAM_SIZE * 1024);
	const uint32_t sp = vt[0];
	const uint32_t pc = vt[1];

	if (sp < sram_lo || sp > sram_hi) {
		return false;
	}
	if ((pc & 1) == 0) { /* thumb bit */
		return false;
	}
	return pc >= base && pc < base + fw_slot0_size();
}

static bool slot_is_bootable(void)
{
	struct sfu_meta meta;

	if (meta_read(&meta) == 0 && meta.magic == SFU_META_MAGIC &&
	    meta.valid == SFU_META_MAGIC) {
		return true;
	}

	/* No valid metadata (e.g. factory JLink flash) — boot if slot0 holds a
	 * plausible image rather than wedging in DFU. */
	return slot0_looks_valid();
}

static void jump_to_app(uint32_t base)
{
	uint32_t *vt = (uint32_t *)base;
	uint32_t sp = vt[0];
	uint32_t pc = vt[1];

	__disable_irq();
	SysTick->CTRL = 0;
	SysTick->VAL = 0;
	/* Disable the MPU: the bootloader (a Zephyr app) leaves MPU regions sized for
	 * its own small RAM, which deny the app's early .data/.bss init (it touches
	 * RAM beyond the bootloader's regions before it reconfigures the MPU itself) —
	 * seen as a MemManage data-access violation. The app re-enables + reprograms
	 * the MPU during kernel init. */
	MPU->CTRL = 0;
	__DSB();
	SCB->VTOR = base;
	__DSB();
	__ISB();
	__set_MSP(sp);
	/* The bootloader is a Zephyr application, so this runs on a thread stack via
	 * PSP (CONTROL.SPSEL=1). Switch back to the main stack (SPSEL=0, privileged)
	 * before entering the app — otherwise the app keeps running on the
	 * bootloader's small thread stack and faults (MemManage stack violation). */
	__set_CONTROL(0);
	__ISB();
	((void (*)(void))pc)();

	CODE_UNREACHABLE;
}

/* ---- DFU receive loop ----------------------------------------------- */

static struct sfu_header m_hdr;
static bool m_started;

static size_t build_status(uint8_t *out, uint8_t status, uint16_t ctx)
{
	out[0] = status;
	out[1] = ctx & 0xFF;
	out[2] = (ctx >> 8) & 0xFF;
	return 3;
}

/* START data = session(4) || CCM(header) [|| tag]  (plaintext header if unkeyed). */
static uint16_t handle_start(const uint8_t *data, size_t len)
{
	if (len < 4) {
		return NFC_ST_ERR_MAGIC;
	}
	auth_set_session(sys_get_le32(data));

	uint8_t hdrbuf[SFU_HEADER_LEN + NFC_CCM_TAG_LEN];
	size_t hl = 0;

	if (auth_decrypt(0, data + 4, len - 4, hdrbuf, &hl) != 0) {
		return NFC_ST_ERR_VERIFY;
	}
	if (hl != SFU_HEADER_LEN) {
		return NFC_ST_ERR_MAGIC;
	}
	memcpy(&m_hdr, hdrbuf, SFU_HEADER_LEN);

	if (!verify_header(&m_hdr, fw_slot0_size())) {
		return (m_hdr.payload_len > fw_slot0_size()) ? NFC_ST_ERR_SIZE
							     : NFC_ST_ERR_MAGIC;
	}
	/* Arm lazy per-page erase instead of a blocking ~1.7 s full-slot erase:
	 * CMD_START must reply quickly or the phone's mailbox poll window re-sends
	 * START and the handshake livelocks. Pages are erased on first write. */
	fw_begin_incremental();
	m_started = true;
	return NFC_ST_READY;
}

/* DATA data = CCM(chunk) [|| tag]; plaintext chunk is NFC_MAX_PLAINTEXT-sized. */
static uint16_t handle_data(uint16_t seq, const uint8_t *data, size_t len)
{
	if (!m_started) {
		return NFC_ST_ERR_STATE;
	}

	uint8_t pt[NFC_MAX_PLAINTEXT];
	size_t pl = 0;

	if (auth_decrypt((uint32_t)seq + 1, data, len, pt, &pl) != 0) {
		return NFC_ST_ERR_VERIFY;
	}

	uint32_t off = (uint32_t)seq * NFC_MAX_PLAINTEXT;

	if (fw_write(off, pt, pl) != 0) {
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
	int ret = fw_slot_crc32(m_hdr.payload_len, &crc);

	if (ret != 0) {
		printk("DFU: finish crc read failed (%d)\n", ret);
		return NFC_ST_ERR_FLASH;
	}
	printk("DFU: finish crc=0x%08x want=0x%08x len=%u\n", crc, m_hdr.payload_crc32,
	       m_hdr.payload_len);
	if (crc != m_hdr.payload_crc32) {
		return NFC_ST_ERR_VERIFY;
	}

	struct sfu_meta meta = {
		.magic = SFU_META_MAGIC,
		.payload_len = m_hdr.payload_len,
		.payload_crc32 = m_hdr.payload_crc32,
		.valid = SFU_META_MAGIC,
		.serial = auth_serial(),
	};
	memcpy(meta.secret_key, auth_key(), NFC_KEY_LEN);

	ret = meta_write(&meta);
	if (ret != 0) {
		printk("DFU: meta_write failed (%d)\n", ret);
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
		/* Echo the request seq in ctx so the phone can tell a fresh reply from a
		 * stale one left in the half-duplex mailbox (PING/START/FINISH all reply
		 * with an otherwise-identical status byte). DATA overrides ctx only on
		 * RETRY (expected_seq); on ACK ctx == seq, same as this default. */
		uint16_t ctx = seq;

		printk("DFU: rx type=0x%02x seq=%u len=%u\n", type, seq, dlen);

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
		printk("DFU: tx status=0x%02x ctx=%u\n", status, ctx);

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
	/* DFU requested by the app over NFC (enter_dfu command): it set a magic in
	 * retained RAM and cold-rebooted. One-shot — cleared on read so a power
	 * cycle boots the app normally. */
	bool forced = dfu_signal_check_and_clear();

	if (bootable && !forced) {
		jump_to_app(fw_slot0_base());
	}

	printk("Entering DFU mode (bootable=%d forced=%d)\n", bootable, forced);

	/* Load the device key (from sfu_meta, provisioned by the app). A blank
	 * record / all-zero key => unkeyed: accept plaintext frames (factory). */
	struct sfu_meta meta;
	static const uint8_t zero_key[NFC_KEY_LEN] = {0};

	if (meta_read(&meta) == 0 && meta.magic == SFU_META_MAGIC) {
		auth_set_key(meta.secret_key, meta.serial);
	} else {
		auth_set_key(zero_key, 0);
	}
	printk("Auth: %s\n", auth_is_keyed() ? "keyed" : "unkeyed (factory)");

	if (mb_init() != 0) {
		printk("ST25DV init failed\n");
		/* Without NFC we cannot recover; spin so a debugger can attach. */
		for (;;) {
			k_msleep(1000);
		}
	}

	printk("DFU: mailbox up, waiting for frames\n");
	dfu_loop();
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
