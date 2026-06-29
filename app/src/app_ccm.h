/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CCM_H_
#define APP_CCM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thin AES-CCM wrapper over mbedtls_ccm (no PSA), the same primitive app_nfc.c
 * uses for the NFC channel. CCM provides CTR-mode confidentiality + a CBC-MAC
 * authentication tag in one pass, which is what the P2P link needs (#118/#119):
 * raw LoRa has no MIC/encryption of its own. Stateless — the caller owns the
 * key, the (key,nonce) uniqueness discipline and the wire framing.
 *
 * nonce_len must be 7..13; tag_len one of {4,6,8,10,12,14,16}. The plaintext and
 * ciphertext are the same length (CTR mode); `aad` is authenticated but not
 * encrypted (pass the cleartext frame header so it is covered by the tag). */

int app_ccm_encrypt(const uint8_t *key, size_t key_len, const uint8_t *nonce, size_t nonce_len,
		    const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len,
		    uint8_t *ct, uint8_t *tag, size_t tag_len);

int app_ccm_decrypt(const uint8_t *key, size_t key_len, const uint8_t *nonce, size_t nonce_len,
		    const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len,
		    const uint8_t *tag, size_t tag_len, uint8_t *pt);

#ifdef __cplusplus
}
#endif

#endif /* APP_CCM_H_ */
