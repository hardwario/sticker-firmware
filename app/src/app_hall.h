/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_HALL_H_
#define APP_HALL_H_

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct app_hall_data {
	bool left_is_active;
	bool right_is_active;
	uint32_t left_count;
	uint32_t right_count;
};

int app_hall_init(void);
int app_hall_get_data(struct app_hall_data *data);
void app_hall_reset_counts(void);
void app_hall_reset_count(bool left, bool right);

/* Seed the totalizers (used to restore persisted counts on boot). */
void app_hall_set_counts(uint32_t left, uint32_t right);

#ifdef __cplusplus
}
#endif

#endif /* APP_HALL_H_ */
