/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * native_sim suite linking the REAL app_nfc.c against an emulated ST25DV (see
 * emul_st25dv.c) — issue #361. First two tests establish that the harness
 * itself works; the rest cover the regression scenarios it exists for: #340
 * M3/M15 (claim-window arm persists only after a confirmed tag write, PR
 * #358) and the vendor-transport clm_consume() gating fix (also PR #358).
 */

#include "app_nfc.h"
#include "app_config.h"

#include <zephyr/ztest.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "emul_st25dv.h"

/* Mirrors app_nfc.c's private `enum clm_state` (app_nfc_clm_state_get()
 * returns the raw uint8_t — no public enum to include). */
#define TEST_CLM_UNSET    0
#define TEST_CLM_PENDING  1
#define TEST_CLM_CONSUMED 2

static void nfc_hw_before(void *fixture)
{
	ARG_UNUSED(fixture);
	st25dv_emul_reset();
	memset(&g_app_config, 0, sizeof(g_app_config));
	/* app_nfc.c's clm state (m_clm_state) is a private static that survives
	 * across tests in the same ztest binary — CONFIG_SETTINGS_NONE makes
	 * app_nfc_init()'s settings_load_subtree("clm") a no-op, so it does NOT
	 * reset to CLM_UNSET on its own. Force it back explicitly. */
	app_nfc_clm_reset();
}

/* Minimal NDEF framer mirroring app_nfc.c's build_ndef_record() (single
 * external-type record, short form since every payload here is < 0xFF bytes):
 * CC(4) + Message-TLV(type 0x03, 1-byte length) + record + Terminator TLV. */
static size_t write_single_record_ndef(uint8_t *out, size_t out_size, const char *type,
				       const uint8_t *payload, size_t payload_len)
{
	size_t type_len = strlen(type);
	size_t msg_len = 1 + 1 + 1 + type_len + payload_len; /* flags+typelen+len+type+payload */
	size_t total = 4 + 1 + 1 + msg_len + 1;

	__ASSERT_NO_MSG(total <= out_size);
	__ASSERT_NO_MSG(payload_len <= 0xFF);

	size_t i = 0;

	out[i++] = 0xE1;                     /* CC0 */
	out[i++] = 0x40;                     /* CC1 */
	out[i++] = ST25DV_EMUL_MEM_SIZE / 8; /* CC2 */
	out[i++] = 0x01;                     /* CC3 */
	out[i++] = 0x03;                     /* NDEF Message TLV */
	out[i++] = (uint8_t)msg_len;
	out[i++] = 0x80 | 0x40 | 0x10 | 0x04; /* MB|ME|SR|TNF=external */
	out[i++] = (uint8_t)type_len;
	out[i++] = (uint8_t)payload_len;
	memcpy(&out[i], type, type_len);
	i += type_len;
	memcpy(&out[i], payload, payload_len);
	i += payload_len;
	out[i++] = 0xFE; /* Terminator TLV */

	return i;
}

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = 0;

	for (; hex[0] && hex[1] && n < cap; hex += 2, n++) {
		char b[3] = {hex[0], hex[1], 0};

		out[n] = (uint8_t)strtoul(b, NULL, 16);
	}
	return n;
}

ZTEST(nfc_hw, test_init_succeeds_on_empty_tag)
{
	zassert_equal(app_nfc_init(), 0, "app_nfc_init failed against the emulated ST25DV");
}

ZTEST(nfc_hw, test_check_writes_info_record_on_empty_tag)
{
	zassert_equal(app_nfc_init(), 0, "app_nfc_init failed");

	uint8_t mem_before[ST25DV_EMUL_MEM_SIZE];

	st25dv_emul_mem_get(mem_before, 0, sizeof(mem_before));
	bool all_zero = true;

	for (size_t i = 0; i < sizeof(mem_before); i++) {
		if (mem_before[i]) {
			all_zero = false;
			break;
		}
	}
	zassert_true(all_zero, "test precondition: tag should start empty");

	zassert_equal(app_nfc_check(), 0, "app_nfc_check failed on an empty tag");

	uint8_t mem_after[ST25DV_EMUL_MEM_SIZE];

	st25dv_emul_mem_get(mem_after, 0, sizeof(mem_after));
	bool wrote_something = false;

	for (size_t i = 0; i < sizeof(mem_after); i++) {
		if (mem_after[i]) {
			wrote_something = true;
			break;
		}
	}
	zassert_true(wrote_something,
		     "app_nfc_check() should have written the resting info record to the tag");
}

/* #340 M3/M15: the claim-window arm (CLM_UNSET -> CLM_PENDING) must persist
 * only once the resting NDEF write that lays the clm record down on the tag
 * actually succeeds — a failed write must leave clm UNSET (retry next poll),
 * never PENDING (which the old code did unconditionally, before the write,
 * and which then permanently latches CONSUMED on the next poll that finds no
 * clm record — see PR #358, `a499f43`). */
ZTEST(nfc_hw, test_clm_arm_reverts_on_write_failure_commits_on_success)
{
	memset(g_app_config.claim_token, 0xAB, sizeof(g_app_config.claim_token));

	zassert_equal(app_nfc_init(), 0, "app_nfc_init failed");
	zassert_equal(app_nfc_clm_state_get(), TEST_CLM_UNSET, "clm should start UNSET");

	/* Cycle 1: the resting-NDEF write that would confirm the arm fails.
	 * write_mem() retries an I2C error internally (ST25DV_I2C_RETRIES=20)
	 * before giving up, so a single injected failure is silently absorbed —
	 * inject enough to exhaust every retry within this one write_mem() call. */
	st25dv_emul_inject_write_fail(25);
	int ret = app_nfc_check();

	zassert_equal(ret, -EIO, "app_nfc_check should surface the injected write failure (got %d)",
		      ret);
	zassert_equal(app_nfc_clm_state_get(), TEST_CLM_UNSET,
		      "a failed arm-confirming write must not leave clm PENDING (#340 M3/M15)");

	/* Cycle 2: no injected failure this time — the same arm attempt succeeds. */
	zassert_equal(app_nfc_check(), 0, "app_nfc_check should succeed once the write lands");
	zassert_equal(app_nfc_clm_state_get(), TEST_CLM_PENDING,
		      "clm should be PENDING once the resting NDEF write is confirmed");
}

/* Golden CCM vectors from tests/nfc_crypto (same contract, kept in lockstep
 * there): wire = header(serial=0,counter=1, 8B BE) || AES-CCM(plaintext).
 * REQ_WIRE/VND_REQ_WIRE decrypt to a valid Command under KEY/VND_KEY
 * respectively — what they DO isn't relevant here, only that app_cmd_handle()
 * accepts them (ret==0), which is what lets handle_encrypted_cmd() reach the
 * clm_consume() call this test is gating on transport. */
#define KEY_HEX     "000102030405060708090a0b0c0d0e0f"
#define VND_KEY_HEX "101112131415161718191a1b1c1d1e1f"
#define REQ_WIRE    "00000000000000019e91d455b31b7eb34212a122abc064170eed1ed238"
#define VND_REQ_WIRE                                                                               \
	"0000000000000001ee1ffd295e8201868ec3520156a2fec3ff856bcbdd327b929c9487fe1fddb3487c5ccc"   \
	"7473eafc5d"

static void arm_clm_pending(void)
{
	memset(g_app_config.claim_token, 0xAB, sizeof(g_app_config.claim_token));
	zassert_equal(app_nfc_init(), 0, "app_nfc_init failed");
	zassert_equal(app_nfc_check(), 0, "arming poll (empty tag) failed");
	zassert_equal(app_nfc_clm_state_get(), TEST_CLM_PENDING, "test precondition: clm armed");
}

ZTEST(nfc_hw, test_secret_key_command_consumes_clm)
{
	arm_clm_pending();

	unhex(KEY_HEX, g_app_config.secret_key, sizeof(g_app_config.secret_key));
	g_app_config.serial_number = 0;
	g_app_config.nonce_counter = 0;

	uint8_t wire[64];
	size_t wire_len = unhex(REQ_WIRE, wire, sizeof(wire));
	uint8_t tag[ST25DV_EMUL_MEM_SIZE] = {0};

	(void)write_single_record_ndef(tag, sizeof(tag), "hio.stck:cmd", wire, wire_len);
	/* Write the whole 512 B buffer (not just the record's own length) so any
	 * leftover bytes from arm_clm_pending()'s earlier resting-NDEF write don't
	 * linger past this record's Terminator TLV — a real phone write is a full
	 * offset-0 overwrite too (see reference_nfc_cmd_no_queue). */
	st25dv_emul_mem_set(tag, 0, sizeof(tag));

	zassert_equal(app_nfc_check(), 0, "app_nfc_check should accept the secret_key command");
	zassert_equal(app_nfc_clm_state_get(), TEST_CLM_CONSUMED,
		      "a valid hio.stck:cmd decrypt must consume the claim window");
}

ZTEST(nfc_hw, test_vendor_token_command_does_not_consume_clm)
{
	arm_clm_pending();

	unhex(VND_KEY_HEX, g_app_config.vendor_token, sizeof(g_app_config.vendor_token));
	g_app_config.serial_number = 0;
	g_app_config.nonce_counter = 0;

	uint8_t wire[64];
	size_t wire_len = unhex(VND_REQ_WIRE, wire, sizeof(wire));
	uint8_t tag[ST25DV_EMUL_MEM_SIZE] = {0};

	(void)write_single_record_ndef(tag, sizeof(tag), "hio.stck:vnd", wire, wire_len);
	st25dv_emul_mem_set(tag, 0, sizeof(tag));

	zassert_equal(app_nfc_check(), 0, "app_nfc_check should accept the vendor_token command");
	zassert_equal(app_nfc_clm_state_get(), TEST_CLM_PENDING,
		      "a vendor_token decrypt must NOT consume the claim window meant for the "
		      "device owner (#316, PR #358 a499f43)");
}

/* #340 L1: the boot-staged path (app_nfc_check() from main(), no RF field and
 * therefore no m_awake_timer session) has no other backstop to clear the
 * "processing" blink nfc_led_processing() starts -- a hard response-write
 * failure must stop it itself (nfc_write_response()'s error paths), or it
 * blinks forever. */
ZTEST(nfc_hw, test_response_write_failure_stops_the_processing_blink)
{
	arm_clm_pending();

	unhex(KEY_HEX, g_app_config.secret_key, sizeof(g_app_config.secret_key));
	g_app_config.serial_number = 0;
	g_app_config.nonce_counter = 0;

	/* A distinct counter (not REQ_WIRE's 1, shared by the two command tests
	 * above) -- m_resp_cache_* in app_nfc.c is a file-scope static that
	 * outlives a single test, so reusing counter=1 here risks a same-counter
	 * "retransmission" replay against whichever of those tests runs first,
	 * instead of a fresh decrypt. Same plaintext/key as REQ_WIRE, counter=99,
	 * sealed with the same seal() helper as sticker_nfc_frame.py. */
	const char *write_fail_req_wire =
		"0000000000000063e656f7390a22d21e38553a1635cab62af5dd4f7d2f";
	uint8_t wire[64];
	size_t wire_len = unhex(write_fail_req_wire, wire, sizeof(wire));
	uint8_t tag[ST25DV_EMUL_MEM_SIZE] = {0};

	(void)write_single_record_ndef(tag, sizeof(tag), "hio.stck:cmd", wire, wire_len);
	st25dv_emul_mem_set(tag, 0, sizeof(tag));

	/* Exhaust write_mem()'s internal I2C retries so the reply write hard-fails
	 * (same injection count as test_clm_arm_reverts_on_write_failure_commits_on_success). */
	st25dv_emul_inject_write_fail(25);

	int ret = app_nfc_check();

	zassert_not_equal(ret, 0,
			  "app_nfc_check should surface the injected response-write failure");
	zassert_false(app_nfc_led_blink_active(),
		      "a hard response-write failure must stop the processing blink timer "
		      "(#340 L1) -- nothing else will on the boot-staged, no-RF-session path");
}

ZTEST_SUITE(nfc_hw, NULL, NULL, nfc_hw_before, NULL, NULL);
