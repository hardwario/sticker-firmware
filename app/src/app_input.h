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
	bool input_a_notify_act;
	bool input_a_notify_deact;
	bool input_b_notify_act;
	bool input_b_notify_deact;
};

int app_input_init(void);
int app_input_get_data(struct app_input_data *data);
void app_input_clear_notify_flags(struct app_input_data *data);
bool app_input_check_notify_event(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_INPUT_H_ */
