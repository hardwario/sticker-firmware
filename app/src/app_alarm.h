/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ALARM_H_
#define APP_ALARM_H_

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

/* The alarm source/quantity enums + the rule model live in app_alarm_rules.h —
 * alarms are now driven by a dynamic rule list, not a fixed source enum. */
#include "app_alarm_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_alarm_event_cb)(enum app_alarm_source source, bool active, void *user_data);

/* Evaluate the threshold / slot-tilt / counter-rate rules against the latest
 * sensor data. Returns true while any rule's alarm is latched active. Called
 * periodically from the main loop. */
bool app_alarm_poll(void);

/* Report a discrete-source edge (hall/input/PIR/accel). `active` is the new
 * digital level. Matched against the source's STATE rule (from/to: edge one-shot
 * or level). Driven by the GPIO/poll handlers in app_hall/app_input/app_sensor. */
void app_alarm_event(enum app_alarm_source source, bool active);

int app_alarm_set_event_callback(app_alarm_event_cb cb, void *user_data);

/* True if the rule for (source, quantity) currently has its alarm latched. */
bool app_alarm_is_active(enum app_alarm_source source, enum app_alarm_quantity quantity);

/* Aggregated alarm state as an APP_DEVICE_STATUS_ALARM_* bitmask, read-only:
 * unlike app_alarm_poll() it does NOT evaluate rules, expire one-shot latches,
 * or send an fPort-3 report. Just reads the latched state (rule slots + no-data
 * / low-battery watchdogs) under the alarm locks. Safe to call from fill_info. */
uint32_t app_alarm_status_flags(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_H_ */
