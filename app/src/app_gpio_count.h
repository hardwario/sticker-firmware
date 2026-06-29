/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_GPIO_COUNT_H_
#define APP_GPIO_COUNT_H_

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One edge-counted GPIO channel (a hall switch or a digital input). The pointers
 * target the owning module's mutex-protected state and config, so the shared
 * apply helper can do the edge/count/log work identically for both app_hall and
 * app_input without duplicating it (#220.C). */
struct app_gpio_count_chan {
	const char *name;       /* log label, e.g. "Left hall switch" */
	const bool *counter_en; /* &g_app_config.<x>_counter: count rising edges when set */
	bool *stored_active;    /* persistent is-active state in the owner's data struct */
	uint32_t *count;        /* persistent rising-edge count in the owner's data struct */
};

/* Apply a fresh sample for one channel. MUST be called holding the owner's data
 * mutex. Updates *stored_active and, on a rising edge with counting enabled,
 * increments *count; logs the transition. Returns the edge so the caller can fire
 * app_alarm_event() AFTER releasing its data lock (keeps the data lock off the
 * alarm/uplink path): +1 rising, -1 falling, 0 no change. */
int app_gpio_count_apply(const struct app_gpio_count_chan *chan, bool is_active);

#ifdef __cplusplus
}
#endif

#endif /* APP_GPIO_COUNT_H_ */
