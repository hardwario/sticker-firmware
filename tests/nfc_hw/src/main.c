/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * First slice of a native_sim suite linking the REAL app_nfc.c against an
 * emulated ST25DV (see emul_st25dv.c) — issue #361. Establishes that the
 * harness itself works (init succeeds, a poll cycle runs end to end) before
 * building the M3/M15/vendor-gating regression scenarios on top of it.
 */

#include "app_nfc.h"
#include "app_config.h"

#include <zephyr/ztest.h>

#include <string.h>

#include "emul_st25dv.h"

static void nfc_hw_before(void *fixture)
{
	ARG_UNUSED(fixture);
	st25dv_emul_reset();
	memset(&g_app_config, 0, sizeof(g_app_config));
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

ZTEST_SUITE(nfc_hw, NULL, NULL, nfc_hw_before, NULL, NULL);
