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

/* Known-answer vector for the public single-block AES-ECB wrapper
 * (app_ccm_ecb_encrypt_block()) -- a pure RFC/FIPS-197 primitive KAT,
 * independent of any particular caller. Vector computed independently via
 * PyCryptodome's AES.new(key, AES.MODE_ECB).encrypt(block) — same KEY as the
 * golden vectors above; `block` is an arbitrary but fixed 16 B input (kept
 * unchanged from when this doubled as a P2P join_key KAT -- join_key no
 * longer exists as a concept in this codebase at all: #118 phase 2 first
 * moved the P2P transport off this bare ECB PRF onto AES-CMAC (see
 * test_cmac_rfc4493_vectors/test_cmac_psa_cross below), and #118 phase 2's
 * later revision (proximos-v2 MR!7 §7) removed the join_key intermediate
 * entirely in favor of deriving session_key directly from app_key). Only
 * `app_ccm_ecb_encrypt_block()` itself -- the ECB primitive -- is under
 * test here; it has no bearing on P2P key derivation any more. */
ZTEST(ccm, test_ecb_encrypt_block_known_answer)
{
	static const uint8_t block[16] = {'H', 'I', 'O', '-', 'P',  '2',  'P',  '-',
					  'J', 'O', 'I', 'N', 0x12, 0x34, 0x56, 0x78};
	static const uint8_t expected[16] = {0xec, 0xf6, 0xd6, 0xe6, 0x03, 0x6b, 0x54, 0xe2,
					     0xba, 0x69, 0xb4, 0x3b, 0xd5, 0x16, 0x18, 0xa9};
	uint8_t out[16];

	zassert_equal(app_ccm_ecb_encrypt_block(KEY, block, out), 0, "ecb_encrypt_block failed");
	zassert_mem_equal(out, expected, sizeof(expected), "ECB single-block KAT mismatch");
}

/* #118 phase 2 (doc/p2p.md §4, crypto review): RFC 4493 §4 official AES-128
 * CMAC test vectors (key = 2b7e151628aed2a6abf7158809cf4f3c), covering the
 * empty message, exactly-one-block, and multi-block-with-partial-tail cases
 * -- pins app_ccm_cmac() against the standard, independent of this repo. */
ZTEST(ccm, test_cmac_rfc4493_vectors)
{
	static const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
					0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
	static const uint8_t msg[64] = {
		0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73,
		0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7,
		0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4,
		0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
		0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
	static const uint8_t mac_0[16] = {0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
					  0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46};
	static const uint8_t mac_16[16] = {0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
					   0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c};
	static const uint8_t mac_40[16] = {0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
					   0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27};
	static const uint8_t mac_64[16] = {0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92,
					   0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe};
	uint8_t out[16];

	zassert_ok(app_ccm_cmac(key, msg, 0, out));
	zassert_mem_equal(out, mac_0, 16, "CMAC Mlen=0 mismatch");

	zassert_ok(app_ccm_cmac(key, msg, 16, out));
	zassert_mem_equal(out, mac_16, 16, "CMAC Mlen=16 mismatch");

	zassert_ok(app_ccm_cmac(key, msg, 40, out));
	zassert_mem_equal(out, mac_40, 16, "CMAC Mlen=40 mismatch");

	zassert_ok(app_ccm_cmac(key, msg, 64, out));
	zassert_mem_equal(out, mac_64, 16, "CMAC Mlen=64 mismatch");
}

/* #118 phase 2 revision (proximos-v2 MR!7 §7): known-answer vector for the
 * P2P transport's session_key = AES128-CMAC(app_key, "HIO-P2P-SES" || 0x01
 * || dev_nonce(4 BE) || central_nonce(4 BE) || serial_number(4 BE) ||
 * zero-pad to 32 B) -- pins app_p2p.c's derive_session_key() construction
 * (root key is now the device's LoRaWAN AppKey directly, no join_key
 * intermediate). Vector computed independently via PyCryptodome's
 * CMAC.new(key, ciphermod=AES) and cross-checked against app/decoder/
 * p2p.js's deriveSessionKey() unit test (app/decoder/p2p.test.js), so C, JS
 * and the independent Python oracle all agree on the same 16 bytes. */
ZTEST(ccm, test_session_key_known_answer)
{
	static const uint8_t block[32] = {
		'H',  'I',  'O',  '-',  'P',  '2',  'P',  '-',  'S',  'E',  'S',
		0x01, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x12, 0x34,
		0x56, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	static const uint8_t expected[16] = {0x80, 0x88, 0x7b, 0x1e, 0x99, 0xb6, 0x1b, 0x0b,
					     0x19, 0xf4, 0x2e, 0x45, 0x7f, 0xeb, 0x40, 0x6f};
	uint8_t out[16];

	zassert_ok(app_ccm_cmac(KEY, block, sizeof(block), out));
	zassert_mem_equal(out, expected, sizeof(expected), "session_key KAT mismatch");
}

/* #118 phase 2 revision (proximos-v2 MR!7 §7): known-answer vectors for the
 * JoinRequest/JoinAccept plain-CMAC handshake tags -- tag = AES128-CMAC(
 * app_key, label || header || body), pins app_p2p.c's send_join_request()/
 * recv_join_accept() tag construction (a NEW proposal introduced with this
 * revision, NOT AES-CCM -- see app_p2p.c's P2P_JOIN_TAG_LABEL comment).
 * Vectors computed independently via PyCryptodome and cross-checked against
 * app/decoder/p2p.js's joinTag() unit test (app/decoder/p2p.test.js). */
ZTEST(ccm, test_join_tag_known_answer)
{
	/* JoinRequest: label "HIO-P2P-JOIN" (12 B) || header (net_id=0,
	 * dev_addr=0, frame_type=0xF0, counter=7) || body (product_type=1,
	 * proto_version=1, serial=0x12345678, fw=1.2.3, reserved=0). */
	static const uint8_t msg_req[33] = {
		'H',  'I',  'O',  '-',  'P',  '2',  'P',  '-',  'J',  'O',  'I',
		'N',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x00,
		0x07, 0x01, 0x01, 0x12, 0x34, 0x56, 0x78, 0x01, 0x02, 0x03, 0x00,
	};
	static const uint8_t expected_req[16] = {0xfd, 0x02, 0x84, 0x3b, 0x75, 0x6c, 0x81, 0xa9,
						 0x97, 0x88, 0x2c, 0x28, 0xed, 0xaf, 0x28, 0xf8};
	/* JoinAccept: label "HIO-P2P-ACC" (11 B) || header (net_id=0,
	 * dev_addr=0, frame_type=0xF1, counter=7) || body (net_id=100,
	 * dev_addr=5, central_nonce=0x22222222, rx1_delay_s=1, reserved=0). */
	static const uint8_t msg_acc[37] = {
		'H',  'I',  'O',  '-',  'P',  '2',  'P',  '-',  'A',  'C',  'C',  0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xf1, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x64,
		0x00, 0x05, 0x22, 0x22, 0x22, 0x22, 0x01, 0x00, 0x00, 0x00, 0x00,
	};
	static const uint8_t expected_acc[16] = {0xca, 0x91, 0xd3, 0x0e, 0x19, 0x5f, 0x4f, 0x38,
						 0xbf, 0x2a, 0xf3, 0x8f, 0xc4, 0xea, 0xd0, 0xe0};
	uint8_t out[16];

	zassert_ok(app_ccm_cmac(KEY, msg_req, sizeof(msg_req), out));
	zassert_mem_equal(out, expected_req, sizeof(expected_req), "JoinRequest tag KAT mismatch");

	zassert_ok(app_ccm_cmac(KEY, msg_acc, sizeof(msg_acc), out));
	zassert_mem_equal(out, expected_acc, sizeof(expected_acc), "JoinAccept tag KAT mismatch");
}

/* Cross-check app_ccm_cmac() against PSA's AES-CMAC (a certified RFC 4493
 * implementation) over varying message lengths, straddling every padding
 * case (empty, partial block, exact block, multi-block partial tail). */
ZTEST(ccm, test_cmac_psa_cross)
{
	static const size_t lens[] = {0, 1, 15, 16, 17, 31, 32, 33, 63};
	uint8_t msg[64];

	for (size_t i = 0; i < sizeof(msg); i++) {
		msg[i] = (uint8_t)(0x50 + i);
	}

	for (size_t li = 0; li < ARRAY_SIZE(lens); li++) {
		size_t len = lens[li];
		uint8_t ours[16];

		zassert_ok(app_ccm_cmac(KEY, msg, len, ours), "cmac failed len=%zu", len);

		psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

		psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
		psa_set_key_algorithm(&attr, PSA_ALG_CMAC);
		psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
		psa_set_key_bits(&attr, 128);

		psa_key_id_t id;

		zassert_equal(psa_import_key(&attr, KEY, 16, &id), PSA_SUCCESS);

		uint8_t oracle[16];
		size_t olen = 0;

		zassert_equal(
			psa_mac_compute(id, PSA_ALG_CMAC, msg, len, oracle, sizeof(oracle), &olen),
			PSA_SUCCESS, "psa_mac_compute len=%zu", len);
		psa_destroy_key(id);

		zassert_equal(olen, 16, "PSA CMAC length != 16");
		zassert_mem_equal(ours, oracle, 16, "CMAC != PSA oracle, len=%zu", len);
	}
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
