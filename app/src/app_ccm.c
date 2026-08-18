/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * AES-128 CCM (RFC 3610) for the encrypted NFC channel — see app_ccm.h. The CCM
 * layer is block-cipher agnostic; the single forward-AES primitive
 * aes128_ecb_encrypt() is provided by one of two backends selected at build time:
 *
 *   CONFIG_APP_CCM_HW_AES=y : the STM32WL on-die AES peripheral, driven at the
 *                             register level (no HAL, no Zephyr crypto API). CCM
 *                             needs only the forward cipher for both encrypt and
 *                             decrypt, so ECB-encrypt is the only primitive.
 *   otherwise               : the LoRaMac soft-SE AES (already in flash), used as
 *                             a fallback and by the host unit tests.
 */

#include "app_ccm.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define AES_BLOCK 16

/* ---- Block-cipher backend: AES-128 ECB single-block forward cipher ---------- */

#if defined(CONFIG_APP_CCM_HW_AES)

#include <soc.h>
#include <stm32wlxx_ll_bus.h>

/* AES data type = 8-bit (byte swapping): with this mode the peripheral consumes /
 * produces DINR/DOUTR as a native little-endian 32-bit load/store of the byte
 * buffer, matching FIPS-197 byte order (mirrors the ST HAL's `*(uint32_t*)ptr`
 * access). Key registers are big-endian word order and are NOT swap-sensitive. */
static void aes128_ecb_encrypt(const uint8_t key[AES_BLOCK], const uint8_t in[AES_BLOCK],
			       uint8_t out[AES_BLOCK])
{
	LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_AES);

	AES->CR = 0U;                /* disable + reset config (ECB, encrypt, 128-bit) */
	AES->CR = AES_CR_DATATYPE_1; /* DATATYPE = 8-bit byte swap; CHMOD=ECB, MODE=enc */

	AES->KEYR3 = sys_get_be32(&key[0]);
	AES->KEYR2 = sys_get_be32(&key[4]);
	AES->KEYR1 = sys_get_be32(&key[8]);
	AES->KEYR0 = sys_get_be32(&key[12]);

	AES->CR |= AES_CR_EN;

	AES->DINR = sys_get_le32(&in[0]);
	AES->DINR = sys_get_le32(&in[4]);
	AES->DINR = sys_get_le32(&in[8]);
	AES->DINR = sys_get_le32(&in[12]);

	while (!(AES->SR & AES_SR_CCF)) {
		/* single-block AES completes in ~15 cycles; no timeout needed */
	}

	sys_put_le32(AES->DOUTR, &out[0]);
	sys_put_le32(AES->DOUTR, &out[4]);
	sys_put_le32(AES->DOUTR, &out[8]);
	sys_put_le32(AES->DOUTR, &out[12]);

	AES->CR |= AES_CR_CCFC; /* clear the computation-complete flag */
	AES->CR = 0U;           /* disable; register state is not retained across Stop */
}

#else /* software fallback / host tests */

#include "aes.h"

static void aes128_ecb_encrypt(const uint8_t key[AES_BLOCK], const uint8_t in[AES_BLOCK],
			       uint8_t out[AES_BLOCK])
{
	aes_context ctx;

	aes_set_key(key, AES_BLOCK, &ctx);
	aes_encrypt(in, out, &ctx);
}

#endif

/* ---- RFC 3610 CCM layer (backend-independent) ------------------------------- */

static K_MUTEX_DEFINE(m_lock);

static void xor16(uint8_t dst[AES_BLOCK], const uint8_t src[AES_BLOCK])
{
	for (int i = 0; i < AES_BLOCK; i++) {
		dst[i] ^= src[i];
	}
}

/* One CBC-MAC step: X <- E(K, X ^ block). */
static void cbc_step(const uint8_t key[AES_BLOCK], uint8_t x[AES_BLOCK],
		     const uint8_t block[AES_BLOCK])
{
	uint8_t t[AES_BLOCK];

	memcpy(t, x, AES_BLOCK);
	xor16(t, block);
	aes128_ecb_encrypt(key, t, x);
}

/* Fold the length-prefixed AAD then the message into the CBC-MAC state `x`, each
 * region independently zero-padded to a block boundary (RFC 3610 §2.2). Only the
 * short AAD encoding (0 < aad_len < 0xFF00, 2-byte big-endian length) is handled. */
static void cbc_mac(const uint8_t key[AES_BLOCK], uint8_t x[AES_BLOCK], const uint8_t *aad,
		    size_t aad_len, const uint8_t *msg, size_t msg_len)
{
	uint8_t block[AES_BLOCK];
	size_t bl;
	size_t off;

	/* AAD region: [len(2, BE)] || aad, zero-padded. */
	memset(block, 0, sizeof(block));
	block[0] = (uint8_t)(aad_len >> 8);
	block[1] = (uint8_t)aad_len;
	bl = 2;
	for (off = 0; off < aad_len; off++) {
		block[bl++] = aad[off];
		if (bl == AES_BLOCK) {
			cbc_step(key, x, block);
			memset(block, 0, sizeof(block));
			bl = 0;
		}
	}
	if (bl != 0) {
		cbc_step(key, x, block); /* tail already zero-padded by memset */
	}

	/* Message region: msg, zero-padded. */
	for (off = 0; off + AES_BLOCK <= msg_len; off += AES_BLOCK) {
		cbc_step(key, x, &msg[off]);
	}
	if (off < msg_len) {
		memset(block, 0, sizeof(block));
		memcpy(block, &msg[off], msg_len - off);
		cbc_step(key, x, block);
	}
}

/* Build counter block A_i and encrypt it: S_i = E(K, A_i). */
static void ctr_keystream(const uint8_t key[AES_BLOCK], const uint8_t *nonce, size_t nonce_len,
			  uint32_t counter, uint8_t s[AES_BLOCK])
{
	uint8_t a[AES_BLOCK];
	size_t L = 15 - nonce_len;

	memset(a, 0, sizeof(a));
	a[0] = (uint8_t)(L - 1); /* flags: only the length field, Adata/M bits are 0 */
	memcpy(&a[1], nonce, nonce_len);
	for (size_t i = 0; i < L && i < 4; i++) {
		a[15 - i] = (uint8_t)(counter >> (8 * i));
	}
	aes128_ecb_encrypt(key, a, s);
}

/* XOR the message with the CTR keystream starting at A_1 (A_0 masks the tag). */
static void ctr_crypt(const uint8_t key[AES_BLOCK], const uint8_t *nonce, size_t nonce_len,
		      const uint8_t *in, uint8_t *out, size_t len)
{
	uint8_t s[AES_BLOCK];
	uint32_t counter = 1;

	for (size_t off = 0; off < len; off += AES_BLOCK, counter++) {
		size_t n = MIN((size_t)AES_BLOCK, len - off);

		ctr_keystream(key, nonce, nonce_len, counter, s);
		for (size_t i = 0; i < n; i++) {
			out[off + i] = in[off + i] ^ s[i];
		}
	}
}

/* Compute the CCM authentication tag over (aad, msg) into `tag` (tag_len bytes). */
static void ccm_tag(const uint8_t key[AES_BLOCK], const uint8_t *nonce, size_t nonce_len,
		    const uint8_t *aad, size_t aad_len, const uint8_t *msg, size_t msg_len,
		    uint8_t *tag, size_t tag_len)
{
	uint8_t x[AES_BLOCK];
	uint8_t s0[AES_BLOCK];
	uint8_t b0[AES_BLOCK];
	size_t L = 15 - nonce_len;

	/* B0: flags = 64*Adata + 8*((M-2)/2) + (L-1), then nonce, then msg_len (L, BE). */
	memset(b0, 0, sizeof(b0));
	b0[0] = (uint8_t)(0x40 | (((tag_len - 2) / 2) << 3) | (L - 1));
	memcpy(&b0[1], nonce, nonce_len);
	for (size_t i = 0; i < L && i < 4; i++) {
		b0[15 - i] = (uint8_t)(msg_len >> (8 * i));
	}
	aes128_ecb_encrypt(key, b0, x);

	cbc_mac(key, x, aad, aad_len, msg, msg_len);

	/* T = MAC ^ S_0 (S_0 = keystream block for counter 0). */
	ctr_keystream(key, nonce, nonce_len, 0, s0);
	for (size_t i = 0; i < tag_len; i++) {
		tag[i] = x[i] ^ s0[i];
	}
}

static bool params_ok(size_t nonce_len, size_t aad_len, size_t tag_len)
{
	return nonce_len >= 7 && nonce_len <= 13 && aad_len >= 1 && aad_len <= 0xFEFF &&
	       tag_len >= 4 && tag_len <= 16 && (tag_len % 2) == 0;
}

int app_ccm_encrypt_and_tag(const uint8_t key[16], const uint8_t *nonce, size_t nonce_len,
			    const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len,
			    uint8_t *ct, uint8_t *tag, size_t tag_len)
{
	if (!params_ok(nonce_len, aad_len, tag_len)) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	/* Tag is computed over the plaintext, so authenticate before we overwrite it
	 * (safe even when ct aliases pt). */
	ccm_tag(key, nonce, nonce_len, aad, aad_len, pt, pt_len, tag, tag_len);
	ctr_crypt(key, nonce, nonce_len, pt, ct, pt_len);

	k_mutex_unlock(&m_lock);
	return 0;
}

int app_ccm_auth_decrypt(const uint8_t key[16], const uint8_t *nonce, size_t nonce_len,
			 const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len,
			 const uint8_t *tag, size_t tag_len, uint8_t *pt)
{
	uint8_t expected[16];
	uint8_t diff = 0;

	if (!params_ok(nonce_len, aad_len, tag_len)) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);

	/* Recover plaintext, then recompute the tag over it and compare. */
	ctr_crypt(key, nonce, nonce_len, ct, pt, ct_len);
	ccm_tag(key, nonce, nonce_len, aad, aad_len, pt, ct_len, expected, tag_len);

	for (size_t i = 0; i < tag_len; i++) {
		diff |= expected[i] ^ tag[i]; /* constant-time compare */
	}

	k_mutex_unlock(&m_lock);

	if (diff != 0) {
		memset(pt, 0, ct_len); /* never expose unauthenticated plaintext */
		return -EBADMSG;
	}
	return 0;
}
