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

ZTEST_SUITE(nfc_hw, NULL, NULL, nfc_hw_before, NULL, NULL);
