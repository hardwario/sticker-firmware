/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ed25519 public key that the NFC bootloader trusts for firmware image
 * signatures (see doc/nfc-update-protocol.md). Only an image signed by the
 * matching private key is accepted when CONFIG_BOOT_REQUIRE_SIGNATURE is set.
 *
 * !! DEVELOPMENT KEY !! Replace with the production key (and keep its private
 * half in an HSM / CI secret) before shipping. The matching private key was
 * generated alongside this and must NOT live in the repo.
 */

#ifndef STICKER_BOOT_FW_SIGNING_KEY_H
#define STICKER_BOOT_FW_SIGNING_KEY_H

#include <stdint.h>

/* Ed25519 raw public key (32 bytes). */
static const uint8_t FW_SIGNING_PUBLIC_KEY[32] = {
	0x32, 0xea, 0xc1, 0x0c, 0xed, 0x81, 0x47, 0x42, 0x32, 0x00, 
	0x30, 0xfc, 0xeb, 0xef, 0x83, 0x88, 0x82, 0x5c, 0x50, 0x58, 
	0x0f, 0x64, 0x8c, 0xaa, 0x5f, 0xed, 0x96, 0xf2, 0xee, 0x3b, 
	0x39, 0x2e, 
};

#endif /* STICKER_BOOT_FW_SIGNING_KEY_H */
