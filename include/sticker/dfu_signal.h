/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DFU-request handoff between the application and the NFC bootloader.
 *
 * The application sets DFU_SIGNAL_MAGIC at a fixed SRAM address and issues a
 * cold reboot to ask the bootloader to enter NFC DFU mode (see
 * doc/nfc-update-protocol.md). The bootloader reads the word once and clears
 * it.
 *
 * The word lives in the top 32 bytes of SRAM, reserved out of `sram0` in
 * boards/sticker/sticker.dts so neither image's linker allocates it. SRAM is
 * not cleared by a software reset, so the value survives the reboot; it IS lost
 * on power loss, so an aborted/forgotten request never wedges the device in DFU
 * (a power cycle boots the application normally).
 */

#ifndef STICKER_DFU_SIGNAL_H_
#define STICKER_DFU_SIGNAL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Top of the 64 KB SRAM minus the 32 reserved bytes (board DTS keeps sram0 32 B
 * short). Must match the reservation in boards/sticker/sticker.dts. */
#define DFU_SIGNAL_ADDR  0x2000FFE0u
#define DFU_SIGNAL_MAGIC 0x44465521u /* "DFU!" */

/* Request DFU on the next boot. Call immediately before sys_reboot(). */
static inline void dfu_signal_request(void)
{
	*(volatile uint32_t *)DFU_SIGNAL_ADDR = DFU_SIGNAL_MAGIC;
}

/* Read the request flag and clear it (one-shot). Returns non-zero if DFU was
 * requested. The bootloader calls this once during the boot decision. */
static inline int dfu_signal_check_and_clear(void)
{
	volatile uint32_t *p = (volatile uint32_t *)DFU_SIGNAL_ADDR;
	int requested = (*p == DFU_SIGNAL_MAGIC);

	*p = 0;
	return requested;
}

#ifdef __cplusplus
}
#endif

#endif /* STICKER_DFU_SIGNAL_H_ */
