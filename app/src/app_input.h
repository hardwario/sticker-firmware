/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_INPUT_H_
#define APP_INPUT_H_

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct app_input_data {
	bool input_a_is_active;
	bool input_b_is_active;
	uint32_t input_a_count;
	uint32_t input_b_count;
};

int app_input_init(void);
int app_input_get_data(struct app_input_data *data);
void app_input_reset_counts(void);
void app_input_reset_count(bool input_a, bool input_b);

/* Seed the totalizers (used to restore persisted counts on boot). */
void app_input_set_counts(uint32_t input_a, uint32_t input_b);

#ifdef __cplusplus
}
#endif

#endif /* APP_INPUT_H_ */
