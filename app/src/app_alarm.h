/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ALARM_H_
#define APP_ALARM_H_

#include "app_w1_slots.h"

/* Standard includes */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Alarm sources. The first five are discrete (edge) sources driven by
 * app_alarm_event(); the threshold sources are driven by app_alarm_poll(). The
 * enum value is the source id used in the fPort-3 alarm-detail payload (#27).
 * New sources insert before the per-slot block (the id is stable per position).
 *
 * Per-slot 1-Wire sources (P2) occupy a contiguous block after the fixed
 * sources: APP_W1_SLOT_COUNT slots × {temperature, humidity, tilt}. The id of
 * a per-slot source is APP_ALARM_SLOT_SRC(slot, quantity). */
enum app_alarm_source {
	APP_ALARM_SOURCE_HALL_LEFT,
	APP_ALARM_SOURCE_HALL_RIGHT,
	APP_ALARM_SOURCE_PIR_MOTION,
	APP_ALARM_SOURCE_INPUT_A,
	APP_ALARM_SOURCE_INPUT_B,
	APP_ALARM_SOURCE_TEMPERATURE,
	APP_ALARM_SOURCE_HUMIDITY,
	APP_ALARM_SOURCE_PRESSURE,
	APP_ALARM_SOURCE_T1_TEMPERATURE,
	APP_ALARM_SOURCE_T2_TEMPERATURE,
	APP_ALARM_SOURCE_SLOT_BASE,
	APP_ALARM_SOURCE_COUNT = APP_ALARM_SOURCE_SLOT_BASE + APP_W1_SLOT_COUNT * 3,
};

/* Per-slot quantities, in the order they're packed after SLOT_BASE. */
enum app_alarm_slot_quantity {
	APP_ALARM_SLOT_TEMPERATURE = 0,
	APP_ALARM_SLOT_HUMIDITY = 1,
	APP_ALARM_SLOT_TILT = 2,
	APP_ALARM_SLOT_QUANTITIES = 3,
};

#define APP_ALARM_SLOT_SRC(slot, quantity)                                                         \
	(APP_ALARM_SOURCE_SLOT_BASE + (slot) * APP_ALARM_SLOT_QUANTITIES + (quantity))

typedef void (*app_alarm_event_cb)(enum app_alarm_source source, bool active, void *user_data);

bool app_alarm_poll(void);
void app_alarm_event(enum app_alarm_source source, bool active);
int app_alarm_set_event_callback(app_alarm_event_cb cb, void *user_data);

/* Human-readable source name (for logs / decoder parity). */
const char *app_alarm_source_name(enum app_alarm_source source);

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_H_ */
