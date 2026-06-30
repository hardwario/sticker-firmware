/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "auth.h"

#include <zephyr/sys/byteorder.h>

#include <psa/crypto.h>

#include <string.h>

#define CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, NFC_CCM_TAG_LEN)

static uint8_t m_key[NFC_KEY_LEN];
static uint32_t m_serial;
static uint32_t m_session;
static bool m_keyed;
static psa_key_id_t m_key_id;
static bool m_key_ready;

static bool all_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] != 0) {
			return false;
		}
	}
	return true;
}

void auth_set_key(const uint8_t key[NFC_KEY_LEN], uint32_t serial)
{
	memcpy(m_key, key, NFC_KEY_LEN);
	m_serial = serial;
	m_keyed = !all_zero(m_key, NFC_KEY_LEN);
	m_key_ready = false; /* imported lazily on first decrypt */
}

bool auth_is_keyed(void)
{
	return m_keyed;
}

void auth_set_session(uint32_t session)
{
	m_session = session;
}

const uint8_t *auth_key(void)
{
	return m_key;
}

uint32_t auth_serial(void)
{
	return m_serial;
}

static void build_nonce(uint8_t nonce[NFC_CCM_NONCE_LEN], uint32_t seq)
{
	sys_put_le32(m_serial, &nonce[0]);
	sys_put_le32(m_session, &nonce[4]);
	sys_put_le32(seq, &nonce[8]);
	nonce[12] = 0;
}

static int ensure_key(void)
{
	if (m_key_ready) {
		return 0;
	}
	if (psa_crypto_init() != PSA_SUCCESS) {
		return -1;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, CCM_ALG);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, PSA_BYTES_TO_BITS(NFC_KEY_LEN));

	if (psa_import_key(&attr, m_key, NFC_KEY_LEN, &m_key_id) != PSA_SUCCESS) {
		return -1;
	}
	m_key_ready = true;
	return 0;
}

int auth_decrypt(uint32_t seq, const uint8_t *in, size_t in_len, uint8_t *out,
		 size_t *out_len)
{
	if (!m_keyed) {
		memcpy(out, in, in_len);
		*out_len = in_len;
		return 0;
	}

	if (in_len < NFC_CCM_TAG_LEN || ensure_key() != 0) {
		return -1;
	}

	uint8_t nonce[NFC_CCM_NONCE_LEN];

	build_nonce(nonce, seq);

	size_t olen = 0;
	psa_status_t st = psa_aead_decrypt(m_key_id, CCM_ALG, nonce, NFC_CCM_NONCE_LEN,
					   NULL, 0, in, in_len, out, in_len, &olen);

	if (st != PSA_SUCCESS) {
		return -1;
	}
	*out_len = olen;
	return 0;
}
