/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Symmetric AES-CCM authentication for NFC firmware update frames.
 * All-zero key => unkeyed (factory): frames pass through as plaintext.
 */

#ifndef STICKER_BOOT_AUTH_H
#define STICKER_BOOT_AUTH_H

#include <sticker/nfc_proto.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Load the device key (from sfu_meta). Sets unkeyed mode if key is all-zero. */
void auth_set_key(const uint8_t key[NFC_KEY_LEN], uint32_t serial);

/* True if a non-zero key is loaded (frames must be CCM-authenticated). */
bool auth_is_keyed(void);

/* Per-session nonce diversifier from CMD_START. */
void auth_set_session(uint32_t session);

/* Currently loaded key/serial (for the bootloader to persist into sfu_meta). */
const uint8_t *auth_key(void);
uint32_t auth_serial(void);

/*
 * Decrypt+verify one frame at sequence `seq`. When keyed, `in` is
 * ciphertext||tag (in_len includes the tag) and `out` receives in_len-tag
 * plaintext bytes. When unkeyed, the frame is copied verbatim. Returns 0 on
 * success (tag verified), negative on auth failure or bad size.
 * `out` must have capacity for at least in_len bytes.
 */
int auth_decrypt(uint32_t seq, const uint8_t *in, size_t in_len, uint8_t *out,
		 size_t *out_len);

#endif /* STICKER_BOOT_AUTH_H */
