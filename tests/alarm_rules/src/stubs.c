/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal environment for app_alarm_rules host tests: the config store (the
 * alarm_N slot fields live here) and a no-op settings_save().
 */

#include "app_config.h"

static struct app_config m_test_config;

struct app_config *app_config(void)
{
	return &m_test_config;
}

int settings_save(void)
{
	return 0;
}
