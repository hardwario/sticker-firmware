/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_ccm.h"
#include "app_log.h"

#include <zephyr/logging/log.h>

#include <mbedtls/ccm.h>

#include <errno.h>

LOG_MODULE_REGISTER(app_ccm, LOG_LEVEL_WRN);

static int ccm_setkey(mbedtls_ccm_context *ctx, const uint8_t *key, size_t key_len)
{
	mbedtls_ccm_init(ctx);

	int ret = mbedtls_ccm_setkey(ctx, MBEDTLS_CIPHER_ID_AES, key, 8 * key_len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("mbedtls_ccm_setkey", ret);
		mbedtls_ccm_free(ctx);
		return -EIO;
	}

	return 0;
}

int app_ccm_encrypt(const uint8_t *key, size_t key_len, const uint8_t *nonce, size_t nonce_len,
		    const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len,
		    uint8_t *ct, uint8_t *tag, size_t tag_len)
{
	if (!key || !nonce || !ct || !tag) {
		return -EINVAL;
	}

	mbedtls_ccm_context ctx;
	int ret = ccm_setkey(&ctx, key, key_len);
	if (ret) {
		return ret;
	}

	ret = mbedtls_ccm_encrypt_and_tag(&ctx, pt_len, nonce, nonce_len, aad, aad_len, pt, ct, tag,
					  tag_len);
	mbedtls_ccm_free(&ctx);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("mbedtls_ccm_encrypt_and_tag", ret);
		return -EIO;
	}

	return 0;
}

int app_ccm_decrypt(const uint8_t *key, size_t key_len, const uint8_t *nonce, size_t nonce_len,
		    const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len,
		    const uint8_t *tag, size_t tag_len, uint8_t *pt)
{
	if (!key || !nonce || !ct || !tag || !pt) {
		return -EINVAL;
	}

	mbedtls_ccm_context ctx;
	int ret = ccm_setkey(&ctx, key, key_len);
	if (ret) {
		return ret;
	}

	/* Returns MBEDTLS_ERR_CCM_AUTH_FAILED if the tag does not verify. */
	ret = mbedtls_ccm_auth_decrypt(&ctx, ct_len, nonce, nonce_len, aad, aad_len, ct, pt, tag,
				       tag_len);
	mbedtls_ccm_free(&ctx);

	if (ret) {
		LOG_WRN("AES-CCM auth-decrypt failed: -0x%04x", (unsigned int)-ret);
		return -EBADMSG;
	}

	return 0;
}
