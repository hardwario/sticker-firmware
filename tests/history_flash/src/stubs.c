/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clock stub for app_history (APP_HISTORY_HAVE_CLOCK). Tests toggle test_clock_*.
 */

#include "app_radio.h"
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

/* app_radio's flash/exchange gate: nothing is on air here. */
void app_radio_flash_hold(void)
{
}

void app_radio_flash_release(void)
{
}
