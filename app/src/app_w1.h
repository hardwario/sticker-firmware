/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_W1_H_
#define APP_W1_H_

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/drivers/w1.h>
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct app_w1 {
	sys_slist_t scan_list;
	bool is_ds28e17_present;
};

int app_w1_acquire(struct app_w1 *w1, const struct device *dev);
int app_w1_release(struct app_w1 *w1, const struct device *dev);
int app_w1_scan(struct app_w1 *w1, const struct device *dev,
		int (*user_cb)(struct w1_rom rom, void *user_data), void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* APP_W1_H_ */
