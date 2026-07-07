/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for app/src/app_ccm.c — the RFC 3610 AES-CCM that replaced mbedTLS
 * on the NFC channel (#261). Two independent oracles pin correctness:
 *
 *   1. The existing nfc_crypto golden wire vectors (serial‖counter‖CCM) — proves
 *      app_ccm is byte-identical to the mbedTLS it replaced, so the phone-side
 *      contract (Manager-App) is unchanged.
 *   2. A cross-check against the PSA AES-CCM (a certified RFC 3610 implementation)
 *      over a matrix of nonce / AAD / plaintext / tag-length parameters, incl. the
 *      NFC params (9-byte nonce, 16-byte tag) and RFC 3610 params (13-byte nonce,
 *      8-byte tag). PSA here is a test oracle only — the firmware ships no mbedTLS.
 *
 * These tests exercise app_ccm's software (soft-SE AES) backend; the STM32WL HW
 * AES backend is target-only and validated by the HW E2E round-trip.
 */

#include "app_ccm.h"

#include <psa/crypto.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t KEY[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
				0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = 0;

	for (; hex[0] && hex[1] && n < cap; hex += 2, n++) {
		char b[3] = {hex[0], hex[1], 0};

		out[n] = (uint8_t)strtoul(b, NULL, 16);
	}
	return n;
}

/* PSA AES-CCM encrypt oracle: out = ciphertext ‖ tag(tag_len). */
static int psa_ccm_encrypt(const uint8_t *key, size_t key_len, const uint8_t *nonce,
			   size_t nonce_len, const uint8_t *aad, size_t aad_len, const uint8_t *pt,
			   size_t pt_len, size_t tag_len, uint8_t *out, size_t out_cap,
			   size_t *out_len)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, tag_len));
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 8 * key_len);

	psa_key_id_t id;

	if (psa_import_key(&attr, key, key_len, &id) != PSA_SUCCESS) {
		return -EIO;
	}

	psa_status_t s =
		psa_aead_encrypt(id, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, tag_len), nonce,
				 nonce_len, aad, aad_len, pt, pt_len, out, out_cap, out_len);
	psa_destroy_key(id);
	return s == PSA_SUCCESS ? 0 : -EIO;
}

/* --- Golden wire vectors (must match tests/nfc_crypto) --- */
/* header = serial(0) ‖ counter(1) = 00 00 00 00 00 00 00 01, used as both the
 * 8-byte AAD and the first 8 bytes of the 9-byte nonce; dir byte completes it. */
#define HDR_HEX   "0000000000000001"
#define DIR_REQ   0x00
#define DIR_RSP   0x01
#define REQ_PLAIN "010801220a"
#define REQ_WIRE  "00000000000000019e91d455b31b7eb34212a122abc064170eed1ed238"
#define RSP_PLAIN "0108011200"
#define RSP_WIRE  "0000000000000001d67a6dd3cce76a8bae20443086737ea584e195dfa0"

/* app_ccm must reproduce the exact golden wire the mbedTLS build emitted. */
static void check_golden(uint8_t dir, const char *plain_hex, const char *wire_hex)
{
	uint8_t hdr[8], pt[64], want[128];
	uint8_t nonce[9], ct[64], tag[16], wire[128];

	unhex(HDR_HEX, hdr, sizeof(hdr));
	size_t pt_len = unhex(plain_hex, pt, sizeof(pt));
	size_t want_len = unhex(wire_hex, want, sizeof(want));

	memcpy(nonce, hdr, 8);
	nonce[8] = dir;

	zassert_ok(app_ccm_encrypt_and_tag(KEY, nonce, 9, hdr, 8, pt, pt_len, ct, tag, 16));

	memcpy(wire, hdr, 8);
	memcpy(&wire[8], ct, pt_len);
	memcpy(&wire[8 + pt_len], tag, 16);

	zassert_equal(8 + pt_len + 16, want_len, "wire length differs from golden");
	zassert_mem_equal(wire, want, want_len, "wire bytes differ from golden vector");
}

ZTEST(ccm, test_golden_request)
{
	check_golden(DIR_REQ, REQ_PLAIN, REQ_WIRE);
}

ZTEST(ccm, test_golden_response)
{
	check_golden(DIR_RSP, RSP_PLAIN, RSP_WIRE);
}

/* Decrypting the golden request wire must recover the plaintext and accept the tag. */
ZTEST(ccm, test_golden_decrypt)
{
	uint8_t wire[128], hdr[8], want[64], nonce[9], pt[64];
	size_t wlen = unhex(REQ_WIRE, wire, sizeof(wire));
	size_t want_len = unhex(REQ_PLAIN, want, sizeof(want));
	size_t ct_len = wlen - 8 - 16;

	unhex(HDR_HEX, hdr, sizeof(hdr));
	memcpy(nonce, hdr, 8);
	nonce[8] = DIR_REQ;

	zassert_ok(app_ccm_auth_decrypt(KEY, nonce, 9, wire, 8, &wire[8], ct_len, &wire[8 + ct_len],
					16, pt));
	zassert_equal(ct_len, want_len);
	zassert_mem_equal(pt, want, want_len, "decrypt recovered wrong plaintext");
}

/* Cross-check app_ccm's ciphertext+tag against PSA (RFC 3610 oracle) across a
 * parameter matrix, and confirm app_ccm round-trips its own output. */
ZTEST(ccm, test_psa_cross_and_roundtrip)
{
	static const uint8_t nonce_full[13] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
					       0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC};
	static const size_t nonce_lens[] = {7, 9, 11, 13};
	static const size_t tag_lens[] = {4, 8, 16};
	static const size_t aad_lens[] = {1, 8, 17};
	static const size_t pt_lens[] = {0, 1, 15, 16, 17, 40};

	uint8_t pt[64], aad[20];

	for (size_t i = 0; i < sizeof(pt); i++) {
		pt[i] = (uint8_t)(0x30 + i);
	}
	for (size_t i = 0; i < sizeof(aad); i++) {
		aad[i] = (uint8_t)(0x10 + i);
	}

	for (size_t ni = 0; ni < ARRAY_SIZE(nonce_lens); ni++) {
		for (size_t ti = 0; ti < ARRAY_SIZE(tag_lens); ti++) {
			for (size_t ai = 0; ai < ARRAY_SIZE(aad_lens); ai++) {
				for (size_t pi = 0; pi < ARRAY_SIZE(pt_lens); pi++) {
					size_t nlen = nonce_lens[ni];
					size_t tlen = tag_lens[ti];
					size_t alen = aad_lens[ai];
					size_t plen = pt_lens[pi];

					uint8_t ct[64], tag[16], oracle[80], rt[64];
					size_t olen = 0;

					int ec = app_ccm_encrypt_and_tag(KEY, nonce_full, nlen, aad,
									 alen, pt, plen, ct, tag,
									 tlen);
					zassert_ok(ec, "encrypt n=%zu t=%zu a=%zu p=%zu", nlen,
						   tlen, alen, plen);

					zassert_ok(psa_ccm_encrypt(KEY, 16, nonce_full, nlen, aad,
								   alen, pt, plen, tlen, oracle,
								   sizeof(oracle), &olen));
					zassert_equal(olen, plen + tlen);
					zassert_mem_equal(ct, oracle, plen,
							  "ciphertext != PSA oracle");
					zassert_mem_equal(tag, &oracle[plen], tlen,
							  "tag != PSA oracle");

					/* app_ccm must accept its own output and reject a
					 * tampered tag. */
					zassert_ok(app_ccm_auth_decrypt(KEY, nonce_full, nlen, aad,
									alen, ct, plen, tag, tlen,
									rt));
					zassert_mem_equal(rt, pt, plen, "roundtrip mismatch");

					tag[0] ^= 0x80;
					zassert_equal(app_ccm_auth_decrypt(KEY, nonce_full, nlen,
									   aad, alen, ct, plen, tag,
									   tlen, rt),
						      -EBADMSG, "tampered tag accepted");
				}
			}
		}
	}
}

/* A single flipped ciphertext byte must fail authentication and zero the output. */
ZTEST(ccm, test_ciphertext_tamper)
{
	uint8_t nonce[9] = {1, 2, 3, 4, 5, 6, 7, 8, DIR_REQ};
	uint8_t aad[8] = {0}, pt[24], ct[24], tag[16], rt[24];

	for (size_t i = 0; i < sizeof(pt); i++) {
		pt[i] = (uint8_t)i;
	}

	zassert_ok(app_ccm_encrypt_and_tag(KEY, nonce, 9, aad, sizeof(aad), pt, sizeof(pt), ct, tag,
					   16));
	ct[5] ^= 0x01;
	zassert_equal(
		app_ccm_auth_decrypt(KEY, nonce, 9, aad, sizeof(aad), ct, sizeof(ct), tag, 16, rt),
		-EBADMSG, "tampered ciphertext accepted");

	uint8_t zero[24] = {0};

	zassert_mem_equal(rt, zero, sizeof(zero), "plaintext not cleared on auth failure");
}

/* Parameter guards. */
ZTEST(ccm, test_param_validation)
{
	uint8_t nonce[13] = {0}, aad[8] = {0}, pt[8] = {0}, ct[8], tag[16];

	zassert_equal(app_ccm_encrypt_and_tag(KEY, nonce, 6, aad, 8, pt, 8, ct, tag, 16), -EINVAL,
		      "accepted nonce_len < 7");
	zassert_equal(app_ccm_encrypt_and_tag(KEY, nonce, 14, aad, 8, pt, 8, ct, tag, 16), -EINVAL,
		      "accepted nonce_len > 13");
	zassert_equal(app_ccm_encrypt_and_tag(KEY, nonce, 9, aad, 0, pt, 8, ct, tag, 16), -EINVAL,
		      "accepted aad_len 0");
	zassert_equal(app_ccm_encrypt_and_tag(KEY, nonce, 9, aad, 8, pt, 8, ct, tag, 3), -EINVAL,
		      "accepted odd/too-small tag");
}

static void *setup(void)
{
	zassert_equal(psa_crypto_init(), PSA_SUCCESS, "psa_crypto_init failed");
	return NULL;
}

ZTEST_SUITE(ccm, NULL, setup, NULL, NULL, NULL);
