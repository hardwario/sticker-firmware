/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_gpio_count.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_gpio_count, LOG_LEVEL_DBG);

int app_gpio_count_apply(const struct app_gpio_count_chan *chan, bool is_active)
{
	bool was_active = *chan->stored_active;

	*chan->stored_active = is_active;

	if (!was_active && is_active) {
		if (*chan->counter_en) {
			(*chan->count)++;
		}
		LOG_DBG("%s activated, count: %u", chan->name, *chan->count);
		return 1;
	}

	if (was_active && !is_active) {
		LOG_DBG("%s deactivated", chan->name);
		return -1;
	}

	return 0;
}
