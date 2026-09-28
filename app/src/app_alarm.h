/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ALARM_H_
#define APP_ALARM_H_

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
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

/* One currently-active alarm, for a status snapshot (Response.Info.active_alarms).
 * Same taxonomy as a configured rule / fPort 3 AlarmEvent: source and quantity are
 * enum app_alarm_source / app_alarm_quantity; type is the AlarmEvent.Type value
 * (1=low, 2=high, 3=trigger, 4=no_data). */
struct app_alarm_active {
	uint8_t source;
	uint8_t quantity;
	uint8_t type;
};

/* Snapshot the alarms latched active right now (rule slots + no-data / low-battery
 * watchdogs) into out[0..max-1]. Read-only, like app_alarm_status_flags() (no
 * evaluation / expiry / send). Returns the number of entries written (<= max). */
size_t app_alarm_active_snapshot(struct app_alarm_active *out, size_t max);

/* Per-alarm active bitmask as of the most recent app_alarm_poll() (#397).
 * Bits 0..APP_ALARM_SLOT_COUNT-1 are the rule slots; the bits above them are
 * the internal watchdogs (the no-data entries, then low-battery) — treat
 * anything above the slot bits as opaque "a watchdog is active", their count
 * and order may grow. Read-only (no evaluation / expiry / send). */
uint32_t app_alarm_active_mask(void);

/* Monotonic activation sequence (#397): incremented once by every
 * app_alarm_poll() that latched at least one NEWLY activated alarm (a bit
 * that was clear in the previous poll's mask) — deactivations never bump it.
 * A reader detects "a new alarm fired since I last looked" by comparing
 * against its own saved copy (compare with !=, it wraps) — non-destructive,
 * so any number of independent readers can watch it. This is the same edge
 * the alarm-driven buzzer melody replays on. */
uint32_t app_alarm_activation_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_H_ */
