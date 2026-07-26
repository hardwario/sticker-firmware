/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests pinning the encrypted-NFC wire contract implemented in
 * app/src/app_nfc.c (decrypt()/encrypt()) and mirrored by the phone side
 * (gitlab tester/manager-app#27). The fix for #179 (AES-CCM nonce reuse) made
 * the nonce direction-separated and added the header as AAD; if either side
 * drifts from this construction, these golden vectors fail.
 *
 * Contract (must match the NFC_NONCE_* macros + AAD argument in app_nfc.c):
 *   wire   = serial(4,BE) || counter(4,BE) || AES-CCM(ciphertext || tag16)
 *   nonce  = serial(4,BE) || counter(4,BE) || direction(1)   (9 bytes)
 *   dir    = 0x00 request (phone->device) / 0x01 response (device->phone)
 *   AAD    = the 8-byte header (serial || counter)
 *   key    = AES-128 (device secret_key)
 */

#include <psa/crypto.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Mirror of app_nfc.c. */
#define NFC_NONCE_LEN          9
#define NFC_NONCE_DIR_REQUEST  0x00
#define NFC_NONCE_DIR_RESPONSE 0x01

/* Mirror of app_nfc.c NFC_NONCE_MAX_SKIP (#266, N-2) — keep in lockstep. */
#define NFC_NONCE_MAX_SKIP 1024

/* Mirror of decrypt()'s anti-replay acceptance decision: accept a received
 * counter only in (current, current + NFC_NONCE_MAX_SKIP]. The subtraction is
 * overflow-safe because the first clause proves received > current. */
static bool nonce_accept(uint32_t current, uint32_t received)
{
	if (received <= current) {
		return false; /* replay / stale */
	}
	if (received - current > NFC_NONCE_MAX_SKIP) {
		return false; /* implausibly far ahead — would risk a lockout */
	}
	return true;
}

/* Published TEST key only (not a device secret). */
static const uint8_t KEY[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
				0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

/* Published TEST vendor_token key (#316) — distinct from KEY, same 16 bytes; the
 * vendor channel (hio.stck:vnd) uses the identical framing keyed by vendor_token. */
static const uint8_t VND_KEY[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
				    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

/* serial = 0, counter = 1  ->  header = AAD = 00 00 00 00 00 00 00 01 */
static const uint8_t HDR[8] = {0, 0, 0, 0, 0, 0, 0, 1};

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = 0;
	for (; hex[0] && hex[1] && n < cap; hex += 2, n++) {
		char b[3] = {hex[0], hex[1], 0};
		out[n] = (uint8_t)strtoul(b, NULL, 16);
	}
	return n;
}

/* Golden vectors generated with the contract above (see manager-app#27). */
#define REQ_PLAIN "010801220a"
#define REQ_WIRE  "00000000000000019e91d455b31b7eb34212a122abc064170eed1ed238"
#define RSP_PLAIN "0108011200"
#define RSP_WIRE  "0000000000000001d67a6dd3cce76a8bae20443086737ea584e195dfa0"

/* #316 vendor channel (hio.stck:vnd), sealed under VND_KEY (dir=REQUEST): the
 * plaintext is APP_PROTO_VERSION + Command{ seq=1, vendor_reset{ key = 16x0xAB } }
 * (field 25, SetSecretKey shape). Same nonce/AAD/tag construction as REQ_WIRE. */
#define VND_REQ_PLAIN "010801ca01120a10abababababababababababababababab"
/* clang-format off */
#define VND_REQ_WIRE "0000000000000001ee1ffd315e8201868ec3520156a2fec3ff856bcbdd327b92f14df3765923ed59ac63824553481d58"
/* clang-format on */

/* Seal `pt` into the wire frame [header(8) || ciphertext+tag]; mirrors encrypt(). */
static int ccm_seal(const uint8_t *key_bytes, uint8_t dir, const uint8_t *pt, size_t pt_len,
		    uint8_t *wire, size_t cap, size_t *wire_len)
{
	uint8_t nonce[NFC_NONCE_LEN];
	memcpy(nonce, HDR, 8);
	nonce[8] = dir;

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_CCM);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 128);

	psa_key_id_t key;
	if (psa_import_key(&attr, key_bytes, sizeof(KEY), &key) != PSA_SUCCESS) {
		return -EIO;
	}

	memcpy(wire, HDR, 8);
	size_t ct = 0;
	psa_status_t s = psa_aead_encrypt(key, PSA_ALG_CCM, nonce, sizeof(nonce), /* AAD */ HDR, 8,
					  pt, pt_len, wire + 8, cap - 8, &ct);
	psa_destroy_key(key);
	if (s != PSA_SUCCESS) {
		return -EIO;
	}
	*wire_len = 8 + ct;
	return 0;
}

/* Open a wire frame with the given direction byte and AAD; mirrors decrypt().
 * `aad` is passed explicitly so a test can feed a wrong AAD to prove binding. */
static psa_status_t ccm_open(const uint8_t *wire, size_t wire_len, uint8_t dir, const uint8_t *aad,
			     uint8_t *pt, size_t cap, size_t *pt_len)
{
	uint8_t nonce[NFC_NONCE_LEN];
	memcpy(nonce, wire, 8); /* header from the wire */
	nonce[8] = dir;

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_CCM);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 128);

	psa_key_id_t key;
	if (psa_import_key(&attr, KEY, sizeof(KEY), &key) != PSA_SUCCESS) {
		return PSA_ERROR_GENERIC_ERROR;
	}

	psa_status_t s = psa_aead_decrypt(key, PSA_ALG_CCM, nonce, sizeof(nonce), aad, 8, wire + 8,
					  wire_len - 8, pt, cap, pt_len);
	psa_destroy_key(key);
	return s;
}

/* Encrypting the documented plaintext must reproduce the exact golden wire bytes
 * — this is what guards both the FW and the phone against contract drift. */
ZTEST(nfc_crypto, test_request_vector)
{
	uint8_t pt[64], want[64], wire[128];
	size_t pt_len = unhex(REQ_PLAIN, pt, sizeof(pt));
	size_t want_len = unhex(REQ_WIRE, want, sizeof(want));
	size_t wire_len = 0;

	zassert_ok(ccm_seal(KEY, NFC_NONCE_DIR_REQUEST, pt, pt_len, wire, sizeof(wire), &wire_len));
	zassert_equal(wire_len, want_len, "request wire length %zu != %zu", wire_len, want_len);
	zassert_mem_equal(wire, want, want_len, "request wire bytes differ from golden vector");
}

ZTEST(nfc_crypto, test_response_vector)
{
	uint8_t pt[64], want[64], wire[128];
	size_t pt_len = unhex(RSP_PLAIN, pt, sizeof(pt));
	size_t want_len = unhex(RSP_WIRE, want, sizeof(want));
	size_t wire_len = 0;

	zassert_ok(
		ccm_seal(KEY, NFC_NONCE_DIR_RESPONSE, pt, pt_len, wire, sizeof(wire), &wire_len));
	zassert_equal(wire_len, want_len, "response wire length %zu != %zu", wire_len, want_len);
	zassert_mem_equal(wire, want, want_len, "response wire bytes differ from golden vector");
}

/* #316: the vendor channel (hio.stck:vnd) uses the identical CCM framing keyed by
 * vendor_token instead of secret_key. Sealing a real vendor_reset Command under a
 * distinct vendor key must reproduce the golden wire bytes — pins the vnd contract
 * for the Manager-App the same way test_request_vector pins hio.stck:cmd. */
ZTEST(nfc_crypto, test_vendor_channel_vector)
{
	uint8_t pt[64], want[64], wire[128];
	size_t pt_len = unhex(VND_REQ_PLAIN, pt, sizeof(pt));
	size_t want_len = unhex(VND_REQ_WIRE, want, sizeof(want));
	size_t wire_len = 0;

	zassert_ok(ccm_seal(VND_KEY, NFC_NONCE_DIR_REQUEST, pt, pt_len, wire, sizeof(wire),
			    &wire_len));
	zassert_equal(wire_len, want_len, "vendor wire length %zu != %zu", wire_len, want_len);
	zassert_mem_equal(wire, want, want_len, "vendor wire bytes differ from golden vector");
}

ZTEST(nfc_crypto, test_roundtrip)
{
	uint8_t wire[128], pt[64], want[64];
	size_t out_len = 0;

	size_t wlen = unhex(RSP_WIRE, wire, sizeof(wire));
	size_t want_len = unhex(RSP_PLAIN, want, sizeof(want));
	zassert_equal(ccm_open(wire, wlen, NFC_NONCE_DIR_RESPONSE, HDR, pt, sizeof(pt), &out_len),
		      PSA_SUCCESS, "response did not decrypt");
	zassert_equal(out_len, want_len);
	zassert_mem_equal(pt, want, want_len);
}

/* The direction byte is the actual fix for #179: a response must NOT decrypt
 * under the request-direction nonce (otherwise request and response would share
 * a keystream). */
ZTEST(nfc_crypto, test_direction_separation)
{
	uint8_t wire[128], pt[64];
	size_t out_len = 0;
	size_t wlen = unhex(RSP_WIRE, wire, sizeof(wire));

	zassert_not_equal(
		ccm_open(wire, wlen, NFC_NONCE_DIR_REQUEST, HDR, pt, sizeof(pt), &out_len),
		PSA_SUCCESS, "response decrypted with REQUEST-direction nonce (nonce reuse!)");
}

/* The header is fed as AAD: a wrong AAD must fail the tag even with the correct
 * nonce/key, so serial/counter are cryptographically bound. */
ZTEST(nfc_crypto, test_aad_binding)
{
	uint8_t wire[128], pt[64], bad_aad[8];
	size_t out_len = 0;
	size_t wlen = unhex(RSP_WIRE, wire, sizeof(wire));

	memcpy(bad_aad, HDR, 8);
	bad_aad[7] ^= 0x01; /* tamper the counter byte in the AAD only */

	zassert_not_equal(
		ccm_open(wire, wlen, NFC_NONCE_DIR_RESPONSE, bad_aad, pt, sizeof(pt), &out_len),
		PSA_SUCCESS, "decrypt accepted a tampered AAD");
}

/* Anti-replay window (#266, N-2). Without an upper bound a single accepted
 * near-UINT32_MAX counter would permanently brick the channel; decrypt() now
 * accepts only (current, current + NFC_NONCE_MAX_SKIP]. */
ZTEST(nfc_crypto, test_nonce_window)
{
	/* Replay / stale — must be rejected. */
	zassert_false(nonce_accept(10, 10), "equal counter accepted (replay)");
	zassert_false(nonce_accept(10, 9), "lower counter accepted (replay)");
	zassert_false(nonce_accept(10, 0), "zero counter accepted (replay)");

	/* The normal Manager-App path is always current + 1. */
	zassert_true(nonce_accept(10, 11), "current + 1 rejected");
	zassert_true(nonce_accept(0, 1), "first command (0 -> 1) rejected");

	/* Small forward gaps within the window stay accepted. */
	zassert_true(nonce_accept(10, 10 + NFC_NONCE_MAX_SKIP), "current + MAX_SKIP rejected");

	/* One past the window is the reject-far boundary. */
	zassert_false(nonce_accept(10, 10 + NFC_NONCE_MAX_SKIP + 1),
		      "current + MAX_SKIP + 1 accepted (lockout risk)");

	/* The brick vector: a jump to near UINT32_MAX must be refused. */
	zassert_false(nonce_accept(10, UINT32_MAX), "near-UINT32_MAX jump accepted (brick)");
	zassert_false(nonce_accept(0, UINT32_MAX - 1), "far jump from zero accepted (brick)");

	/* Overflow safety: current near the top still accepts current + 1 without
	 * the (current + MAX_SKIP) sum wrapping. */
	zassert_true(nonce_accept(UINT32_MAX - 1, UINT32_MAX), "top-of-range current + 1 rejected");
}

static void *setup(void)
{
	zassert_equal(psa_crypto_init(), PSA_SUCCESS, "psa_crypto_init failed");
	return NULL;
}

ZTEST_SUITE(nfc_crypto, NULL, setup, NULL, NULL, NULL);
