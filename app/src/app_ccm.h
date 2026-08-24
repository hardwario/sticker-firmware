/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CCM_H
#define APP_CCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal AES-128 CCM (RFC 3610) for the encrypted NFC channel. This replaces the
 * mbedTLS CCM that app_nfc.c used before, so mbedTLS can be dropped from the build
 * (~6.5 KB flash, #261). The underlying block cipher is either the STM32WL on-die
 * AES peripheral (CONFIG_APP_CCM_HW_AES=y, register-level) or the LoRaMac soft-SE
 * AES already in flash (fallback / host tests). CCM output is fully specified by
 * RFC 3610, so it is byte-identical to mbedTLS regardless of the primitive — the
 * phone-side contract (Manager-App) and the nfc_crypto golden vectors are unchanged.
 *
 * The implementation is deliberately narrowed to what the NFC channel needs, to
 * stay small:
 *   - key is always 128-bit (16 bytes),
 *   - nonce_len in [7, 13]  (NFC uses 9  -> CCM L = 15 - nonce_len = 6),
 *   - tag_len even in [4, 16]  (NFC uses 16),
 *   - aad_len in [1, 0xFEFF]  (RFC 3610 short AAD encoding; NFC uses 8),
 *   - message length fits in 32 bits.
 * Each call is stateless (key reloaded per operation) and serialised by an internal
 * mutex, so it is safe even though NFC command processing is the only caller today.
 */

/* Single-block AES-128 ECB forward encrypt: `out` = AES(`key`, `in`), no CCM
 * framing (no nonce/AAD/tag). Uses the same backend (HW peripheral or soft-SE
 * fallback) and mutex as the CCM calls below. For key derivation only (e.g. the
 * P2P transport's one-block PRF, doc/p2p.md §4) — never for bulk encryption,
 * where CCM's chaining/authentication is required. Always returns 0. */
int app_ccm_ecb_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* AES-128 CMAC (NIST SP 800-38B, equivalently RFC 4493): `out` = CMAC(`key`,
 * `msg`). Same backend + mutex as the calls above. For the P2P transport's
 * join_key/session_key derivation (doc/p2p.md §4) -- a proper PRF with
 * domain-separated subkeys, replacing the bare one-block AES-ECB PRF a crypto
 * review flagged as under-specified (#118 phase 2). `msg_len` may be 0 or
 * span multiple blocks; no length limit beyond what fits in `size_t`. Always
 * returns 0. */
int app_ccm_cmac(const uint8_t key[16], const uint8_t *msg, size_t msg_len, uint8_t out[16]);

/* Encrypt `pt_len` plaintext bytes and produce a `tag_len`-byte tag. `ct` receives
 * `pt_len` ciphertext bytes; `tag` receives `tag_len` bytes. `ct` may alias `pt`.
 * Returns 0, or -EINVAL on a parameter violation. */
int app_ccm_encrypt_and_tag(const uint8_t key[16], const uint8_t *nonce, size_t nonce_len,
			    const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len,
			    uint8_t *ct, uint8_t *tag, size_t tag_len);

/* Decrypt `ct_len` ciphertext bytes and verify `tag` in constant time. `pt` receives
 * `ct_len` plaintext bytes (zeroed on tag mismatch). `pt` may alias `ct`. Returns 0
 * on success, -EBADMSG on tag mismatch, or -EINVAL on a parameter violation. */
int app_ccm_auth_decrypt(const uint8_t key[16], const uint8_t *nonce, size_t nonce_len,
			 const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len,
			 const uint8_t *tag, size_t tag_len, uint8_t *pt);

#ifdef __cplusplus
}
#endif

#endif /* APP_CCM_H */
