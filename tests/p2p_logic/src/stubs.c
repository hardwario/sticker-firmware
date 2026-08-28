/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal stubs so app_p2p.c links on native_sim: the global config it reads
 * and the app_compose entry points it calls. The pure-logic tests never drive
 * a real send/compose, so these can be trivial. app_ccm.c is the REAL source
 * (linked in CMakeLists) -- the frame codec tests need genuine CCM.
 */

#include "app_compose.h"
#include "app_config.h"

#include <zephyr/toolchain.h>

#include <stddef.h>

struct app_config g_app_config;

int app_compose_budget(uint8_t *buf, size_t size, size_t *len, bool *more, uint8_t budget)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(size);
	ARG_UNUSED(budget);
	if (len) {
		*len = 0;
	}
	if (more) {
		*more = false;
	}
	return 0;
}

void app_compose_reset(void)
{
}
