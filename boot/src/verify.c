/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "verify.h"

#include <string.h>

bool verify_header(const struct sfu_header *hdr, uint32_t slot_size)
{
	if (hdr->magic[0] != SFU_MAGIC0 || hdr->magic[1] != SFU_MAGIC1 ||
	    hdr->magic[2] != SFU_MAGIC2 || hdr->magic[3] != SFU_MAGIC3) {
		return false;
	}
	if (hdr->hdr_version != SFU_HDR_VERSION) {
		return false;
	}
	if (hdr->payload_len == 0 || hdr->payload_len > slot_size) {
		return false;
	}
	return true;
}

bool verify_signature(const struct sfu_header *hdr,
		      const uint8_t signature[SFU_SIGNATURE_LEN])
{
	ARG_UNUSED(hdr);
	ARG_UNUSED(signature);

#if defined(CONFIG_BOOT_ALLOW_UNSIGNED)
	/* DEV/debug only: skip authenticity, rely on CRC for integrity. */
	return true;
#else
	/*
	 * TODO(Phase 3): compute SHA-512 over header || payload and verify the
	 * Ed25519 signature against the baked-in public key (tinycrypt/uECC
	 * from bootloader/mcuboot/ext or a compact ed25519). Fail closed until
	 * then so production builds never accept an unverified image.
	 */
	return false;
#endif
}
