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

/* The rule model (slot, channel, sensor type) lives in app_alarm_rules.h. */
#include "app_alarm_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Most watchdog alarms active at once: no-data on up to APP_ALARM_NODATA_MB_MAX
 * motherboard liveness channels, per 1-Wire slot one for the whole device
 * (channel APP_SENSOR_CH_DEVICE) plus one per part (chip) of its type, low
 * battery and one sensor mismatch per 1-Wire slot. Sizes
 * app_alarm_active_snapshot() buffers. */
#define APP_ALARM_NODATA_MB_MAX 4
#define APP_ALARM_NODATA_W1_MAX (1 + APP_SENSOR_W1_PART_MAX)
#define APP_ALARM_NODATA_MAX                                                                       \
	(APP_ALARM_NODATA_MB_MAX + APP_ALARM_SLOT_MAX * APP_ALARM_NODATA_W1_MAX)
#define APP_ALARM_WATCHDOG_MAX (APP_ALARM_NODATA_MAX + 1 + APP_ALARM_SLOT_MAX)

typedef void (*app_alarm_event_cb)(uint8_t channel, bool active, void *user_data);

/* Evaluate the threshold / state / counter-rate rules against the latest
 * sensor data. Returns true while any rule's alarm is latched active. Called
 * periodically from the main loop. */
bool app_alarm_poll(void);

/* Report a motherboard state-channel edge (APP_SENSOR_CH_MOTHERBOARD_*: hall /
 * input state, PIR / accelerometer motion). `active` is the new digital level.
 * Matched against every STATE rule on that channel (from/to: edge one-shot or
 * level). Driven by the GPIO/poll handlers in app_hall/app_input/app_sensor. */
void app_alarm_event(uint8_t channel, bool active);

int app_alarm_set_event_callback(app_alarm_event_cb cb, void *user_data);

/* Aggregated alarm state as an APP_DEVICE_STATUS_ALARM_* bitmask, read-only:
 * unlike app_alarm_poll() it does NOT evaluate rules, expire one-shot latches,
 * or send an fPort-3 report. Just reads the latched state (rule slots + no-data
 * / low-battery watchdogs) under the alarm locks. Safe to call from fill_info. */
uint32_t app_alarm_status_flags(void);

/* One currently-active alarm, for a status snapshot (Response.Info.active_alarms).
 * Same taxonomy as a configured rule / fPort 3 AlarmEvent (#430): slot (0 =
 * motherboard, 1..4 = s1..s4), channel of the slot's sensor type, the sensor
 * type id and the AlarmEvent.Type value (1=low, 2=high, 3=trigger, 4=no_data,
 * 5=sensor_mismatch; channel 0 for the last). */
struct app_alarm_active {
	uint8_t slot;
	uint8_t channel;
	uint8_t sensor_type;
	uint8_t type;
};

/* Snapshot the alarms latched active right now (rule slots + no-data / low-battery
 * watchdogs) into out[0..max-1]. Read-only, like app_alarm_status_flags() (no
 * evaluation / expiry / send). Returns the number of entries written (<= max). */
size_t app_alarm_active_snapshot(struct app_alarm_active *out, size_t max);

/* Per-alarm active bitmask as of the most recent app_alarm_poll() (#397).
 * Bits 0..APP_ALARM_RULE_COUNT-1 are the rules; the bits above them are the
 * internal watchdogs (no-data per slot, low battery, mismatch) — treat
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

/* app_radio: the boot/join announce is out -- send an alarm batch that waited
 * for it (app_radio_data_hold_ms()). No-op when nothing is held. */
void app_alarm_flush_held(void);

/* app_radio, before a post-command action (#462): send an alarm batch that is
 * still collecting (alarm-limit window, or held for queue room) now, so the
 * action's reboot does not drop it. Returns true while such a batch waits and
 * the radio takes data; false when none waits or the link is down. */
bool app_alarm_flush_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_H_ */
