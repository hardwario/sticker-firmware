/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Image integrity & authenticity checks for the NFC bootloader.
 * WIP scaffold — signature verification is a stub pending Phase 3.
 */

#ifndef STICKER_BOOT_VERIFY_H
#define STICKER_BOOT_VERIFY_H

#include <sticker/nfc_proto.h>

#include <stdbool.h>
#include <stdint.h>

/* Validate header magic/version/size against the slot. */
bool verify_header(const struct sfu_header *hdr, uint32_t slot_size);

/*
 * Verify the Ed25519 signature over header || payload using the baked-in
 * public key. `payload_crc` is the CRC already computed from the written
 * slot (used to feed the hash without re-reading where possible).
 *
 * TODO(Phase 3): wire a real Ed25519 implementation. Until then this returns
 * true only when CONFIG_BOOT_ALLOW_UNSIGNED is set (DEV/debug builds).
 */
bool verify_signature(const struct sfu_header *hdr,
		      const uint8_t signature[SFU_SIGNATURE_LEN]);

#endif /* STICKER_BOOT_VERIFY_H */
