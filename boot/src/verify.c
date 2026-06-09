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
