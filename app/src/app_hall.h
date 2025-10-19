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
	bool left_notify_act;
	bool left_notify_deact;
	bool right_notify_act;
	bool right_notify_deact;
};

int app_hall_init(void);
int app_hall_get_data(struct app_hall_data *data);
void app_hall_clear_notify_flags(struct app_hall_data *data);
bool app_hall_check_notify_event(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_HALL_H_ */
