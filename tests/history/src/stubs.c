/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clock stub for app_history (APP_HISTORY_HAVE_CLOCK). Tests toggle test_clock_*.
 */

#include <stdbool.h>
#include <stdint.h>

bool test_clock_has;
uint32_t test_clock_unix;

int app_clock_get_unix(uint32_t *unix_s)
{
	if (!test_clock_has) {
		return -1;
	}
	*unix_s = test_clock_unix;
	return 0;
}
