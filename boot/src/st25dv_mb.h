/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ST25DV FTM (Fast-Transfer-Mode) mailbox transport for the NFC bootloader.
 * See doc/nfc-update-protocol.md §4. Mirrors app/src/app_nfc.c.
 */

#ifndef STICKER_BOOT_ST25DV_MB_H
#define STICKER_BOOT_ST25DV_MB_H

#include <stddef.h>
#include <stdint.h>

/* Power the ST25DV (lpd) and prepare the I2C bus. */
int mb_init(void);

/*
 * Block until the phone has written a request into the mailbox (MB_CTRL_Dyn
 * RF_PUT set), then copy it into `frame` (capacity NFC_MB_REQ_LEN). On success
 * *len is the frame length. Returns 0, or -ETIMEDOUT, or a negative I2C error.
 */
int mb_wait_request(uint8_t *frame, size_t cap, size_t *len, int timeout_ms);

/* Write a response into the mailbox (arms HOST_PUT_MSG for the phone to read). */
int mb_send_response(const uint8_t *rsp, size_t len);

#endif /* STICKER_BOOT_ST25DV_MB_H */
