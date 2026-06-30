/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Image header validation for the NFC bootloader. Authenticity is handled
 * separately by the symmetric AES-CCM path (auth.[ch]).
 */

#ifndef STICKER_BOOT_VERIFY_H
#define STICKER_BOOT_VERIFY_H

#include <sticker/nfc_proto.h>

#include <stdbool.h>
#include <stdint.h>

/* Validate header magic/version/size against the slot. */
bool verify_header(const struct sfu_header *hdr, uint32_t slot_size);

#endif /* STICKER_BOOT_VERIFY_H */
