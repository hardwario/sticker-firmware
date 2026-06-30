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

	if (meta_read(&meta) == 0 && meta.magic == SFU_META_MAGIC) {
		/* Metadata exists: it is the authority. valid==MAGIC means a committed
		 * image (FINISH succeeded). Anything else (e.g. valid==0) means an
		 * update is in progress or was interrupted — slot0 is partial, so stay
		 * in DFU rather than boot a corrupt image. Recoverable: a fresh update
		 * (or a power cycle mid-write) always lands here, never bricks. */
		return meta.valid == SFU_META_MAGIC;
	}

	/* No metadata at all (blank record = factory JLink flash, never touched by
	 * DFU) — boot if slot0 holds a plausible image rather than wedging in DFU. */
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
/* Session for which slot0 was already fully erased, so a re-sent START (the
 * phone re-writes it each poll window during the ~1.7 s erase) doesn't erase
 * again and livelock. */
static uint32_t m_erased_session;

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
	uint32_t session = sys_get_le32(data);

	auth_set_session(session);

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
	/* Idempotent: the phone re-writes START every poll window, so a START that
	 * repeats the session we already erased just re-acks — no second erase, no
	 * livelock. */
	if (m_started && session == m_erased_session) {
		return NFC_ST_READY;
	}

	/* Mark the slot in-progress BEFORE touching it: write metadata with a valid
	 * magic but valid != MAGIC. From here on, an interruption + power cycle finds
	 * meta present-but-not-committed and stays in DFU (slot_is_bootable), instead
	 * of booting a half-written image. handle_finish overwrites this with the
	 * committed record only after the CRC verifies. */
	struct sfu_meta in_progress = {
		.magic = SFU_META_MAGIC,
		.payload_len = m_hdr.payload_len,
		.payload_crc32 = m_hdr.payload_crc32,
		.valid = 0,
		.serial = auth_serial(),
	};
	memcpy(in_progress.secret_key, auth_key(), NFC_KEY_LEN);
	if (meta_write(&in_progress) != 0) {
		return NFC_ST_ERR_FLASH;
	}

	/* Erase the WHOLE slot now, up front (~1.7 s). This keeps the per-frame DATA
	 * path write-only, so a flash page erase never overlaps the RF field + the
	 * AES-CCM decrypt — that 3-way overlap (NFC + crypto + erase) is what failed
	 * the erase mid-stream on keyed updates. The erase blocks the START reply,
	 * but it runs with no concurrent crypto and the idempotent re-ack above
	 * absorbs the phone's retries. DATA frames then only program. */
	int eret = fw_erase_slot();

	if (eret) {
		printk("DFU: full slot erase FAILED: %d\n", eret);
		return NFC_ST_ERR_FLASH;
	}
	m_erased_session = session;
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

#if defined(CONFIG_BOOT_SELFTEST)
/* Phone-free reproduction of the keyed-DFU flash failure. Loops over slot0
 * pages doing (optionally) an AES-CCM decrypt (the per-frame crypto current
 * load) immediately before each page erase+write — exactly the keyed handle_data
 * sequence — first WITHOUT crypto (control) then WITH, so a PPK2 current trace
 * can show the erase spike that sags the supply, and flash_writer logs whether
 * the erase or the program is what fails. Never returns. */
#define SELFTEST_ROUNDS 100
static void selftest(void)
{
	static const uint8_t test_key[NFC_KEY_LEN] = {
		0x9a, 0x7c, 0x7d, 0x1f, 0xfe, 0xcc, 0xfe, 0xb7,
		0x41, 0x5a, 0xb2, 0x47, 0x9b, 0xe9, 0x31, 0xb8,
	};
	static const uint8_t zero_key[NFC_KEY_LEN] = {0};
	const uint32_t npages = fw_slot0_size() / 2048u;
	static uint8_t buf[NFC_MAX_PLAINTEXT];
	static uint8_t ct[NFC_MAX_PLAINTEXT + NFC_CCM_TAG_LEN];
	static uint8_t pt[NFC_MAX_PLAINTEXT + NFC_CCM_TAG_LEN];
	size_t ptl;

	memset(buf, 0xA5, sizeof(buf));
	memset(ct, 0x5A, sizeof(ct));
	printk("SELFTEST: npages=%u rounds=%d\n", npages, SELFTEST_ROUNDS);

	for (int phase = 0; phase < 2; phase++) {
		bool crypto = (phase == 1);
		int fails = 0;

		auth_set_key(crypto ? test_key : zero_key, 0x80E0058Bu);
		auth_set_session(0x12345678u);
		printk("SELFTEST: PHASE %d (crypto=%d) START\n", phase, crypto);
		k_msleep(2000); /* idle gap = low-current marker in the PPK2 trace */

		for (int r = 0; r < SELFTEST_ROUNDS; r++) {
			fw_begin_incremental();
			for (uint32_t p = 0; p < npages; p++) {
				if (crypto) {
					(void)auth_decrypt(p, ct, sizeof(ct), pt, &ptl);
				}
				if (fw_write(p * 2048u, buf, NFC_MAX_PLAINTEXT) != 0) {
					fails++; /* flash_writer prints erase-vs-write + errno */
				}
			}
			if ((r % 20) == 0) {
				printk("SELFTEST: phase=%d round=%d fails=%d\n", phase, r, fails);
			}
		}
		printk("SELFTEST: PHASE %d DONE fails=%d\n", phase, fails);
	}
	printk("SELFTEST: COMPLETE\n");
	for (;;) {
		k_msleep(1000);
	}
}
#endif /* CONFIG_BOOT_SELFTEST */

int main(void)
{
	printk("STICKER NFC bootloader\n");

#if defined(CONFIG_BOOT_SELFTEST)
	selftest(); /* never returns */
#endif

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
