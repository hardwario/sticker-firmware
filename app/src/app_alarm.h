/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ALARM_H_
#define APP_ALARM_H_

/* Standard includes */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum app_alarm_source {
	APP_ALARM_SOURCE_HALL_LEFT,
	APP_ALARM_SOURCE_HALL_RIGHT,
	APP_ALARM_SOURCE_PIR_MOTION,
	APP_ALARM_SOURCE_INPUT_A,
	APP_ALARM_SOURCE_INPUT_B,
};

typedef void (*app_alarm_event_cb)(enum app_alarm_source source, bool active, void *user_data);

bool app_alarm_is_active(void);
bool app_alarm_poll(void);
void app_alarm_event(enum app_alarm_source source, bool active);
int app_alarm_set_event_callback(app_alarm_event_cb cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_H_ */
