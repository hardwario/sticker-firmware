/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ALARM_RULES_H_
#define APP_ALARM_RULES_H_

/* Dynamic alarm rules — a small, persisted array of {slot, channel, condition}
 * rules that unifies every alarm (on-board sensors and inputs, ROM-bound 1-Wire
 * slots, counters) under the sensor channel model of #430
 * (app_sensor_types.yaml).
 *
 * Storage lives in app_config: each rule is a `bytes` config parameter
 * (alarm_0..alarm_15), so rules are set/read over SetParam/GetParam like any
 * other parameter (manager-app); the `alarm` shell edits the same entries. The
 * cache here is just a decoded copy, rebuilt from app_config on load and after
 * every change.
 *
 * Three terms (#430 D7):
 *  - rule:    the storage position and stable identity (alarm_0..alarm_15).
 *             Several rules may target the same channel (e.g. a warning band
 *             and a critical band). Clearing a rule never renumbers the others.
 *  - slot:    where the sensor sits, 0 = motherboard, 1..4 = 1-Wire s1..s4.
 *  - channel: the value of the slot's sensor type (type-local number).
 *
 * The rule also stores the sensor type it was written for. A rule whose type
 * differs from the slot's current type (motherboard for slot 0, sensorN_type
 * for slot N) is stale: kept, but inert until the type matches again.
 *
 * Kind, scale, momentary and counter come from the channel descriptor:
 *  - THRESHOLD: lo/hi band, no hysteresis; dwell = seconds the value must stay
 *    outside [lo,hi] before activating (0 = immediate). Deactivation is always
 *    immediate on returning inside the band.
 *  - STATE (digital 0/1): from_state/to_state — from != to is an edge (dwell =
 *    confirm time before firing, then the same duration holds the alarm
 *    active/re-arm-blocked, no separate deactivate event); from == to is a
 *    level (dwell = time before activating; deactivate is immediate once
 *    state != to). A momentary channel (PIR / accelerometer motion) fires
 *    immediately on each pulse and uses dwell as the post-fire hold only.
 *  - RATE: hi = max counter increase allowed per report interval; dwell =
 *    hold/re-arm window after firing.
 */

#include "app_sensor_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_ALARM_RULE_COUNT 16

/* Sensor slots a rule can target. */
#define APP_ALARM_SLOT_MB  0 /* motherboard (on-board sensors and inputs) */
#define APP_ALARM_SLOT_MAX 4 /* 1-Wire s1..s4 */

struct app_alarm_rule {
	uint8_t slot;        /* 0 = motherboard, 1..4 = s1..s4 */
	uint8_t channel;     /* channel of sensor_type (app_sensor_types.h) */
	uint8_t sensor_type; /* registry type id the rule was written for */
	uint8_t enabled;
	uint8_t from_state;  /* STATE only: previous level (0/1) */
	uint8_t to_state;    /* STATE only: triggering level (0/1); from==to => level */
	float lo, hi, dwell; /* THRESHOLD/RATE use hi; dwell is a hold in seconds, all kinds */
};

/* Build the rule cache from the app_config entries (called once at init,
 * after app_config has loaded). */
int app_alarm_rules_init(void);

/* Rebuild the rule cache from the app_config alarm_N entries. Call after a
 * SetParam that touched the alarms or sensors group so the runtime reflects it
 * without a reboot. Returns the count of faulty rules: invalid ones (sanitized,
 * H-10) plus stale ones (kept, inert) under the staged slot types, so a
 * SetParam caller can report a fault instead of a misleading ACK. */
int app_alarm_rules_reload_from_config(void);

/* Count of cached rules that are stale under the staged slot types. */
int app_alarm_rules_stale_count(void);

/* Registry type of `slot`: motherboard for slot 0, the runtime sensorN_type for
 * slots 1..4 (APP_SENSOR_TYPE_NONE when not set or out of range). */
uint8_t app_alarm_slot_type(uint8_t slot);

/* True if (slot, channel, sensor_type) is a structurally valid rule target:
 * slot 0 with the motherboard type, slots 1..4 with a 1-Wire type, and a
 * channel of that type that is not retired, has a kind (not `none`) and is
 * not a built-in watchdog input. Does not require the sensor to be present or
 * the slot to hold that type (see app_alarm_rule_stale()). */
bool app_alarm_rule_valid(uint8_t slot, uint8_t channel, uint8_t sensor_type);

/* Channel descriptor of a rule (NULL for an invalid one). */
const struct app_sensor_channel *app_alarm_rule_channel(const struct app_alarm_rule *r);

/* True if the rule's sensor type differs from the slot's current runtime type. */
bool app_alarm_rule_stale(const struct app_alarm_rule *r);

/* True if the rule is evaluated now: enabled, not stale, and the channel's
 * capability (motherboard) is on. Anything else is inert. */
bool app_alarm_rule_armed(const struct app_alarm_rule *r);

/* Number of occupied rules, and per-rule access. Iterate occupied rules with
 *   struct app_alarm_rule r;
 *   for (uint8_t i = 0; i < APP_ALARM_RULE_COUNT; i++)
 *           if (app_alarm_rules_get(i, &r)) { ... use &r ... }
 * app_alarm_rules_get() copies the rule into *out under the rules lock (M-6) and
 * returns false for an out-of-range or empty rule. */
uint8_t app_alarm_rules_count(void);
bool app_alarm_rules_get(uint8_t rule, struct app_alarm_rule *out);
bool app_alarm_rules_occupied(uint8_t rule);

/* Lowest empty rule, or -1 if all APP_ALARM_RULE_COUNT rules are occupied. */
int app_alarm_rules_first_free(void);

/* Store a rule at index `rule`, overwriting whatever was there. Returns 0 or
 * -EINVAL (bad index, invalid target or shape). A stale rule is accepted (the
 * type may be provisioned later). Does not persist — call
 * app_alarm_rules_save(). */
int app_alarm_rules_set(uint8_t rule, const struct app_alarm_rule *r);

/* Empty `rule` (no renumbering). Returns 0, -EINVAL (bad index), -ENOENT
 * (already empty). Not persisted. */
int app_alarm_rules_clear(uint8_t rule);

/* Empty all rules. Not persisted. */
void app_alarm_rules_clear_all(void);

/* Persist the rules. They live in the app_config entries, so this saves the
 * config (storage NVS) without rebooting. */
int app_alarm_rules_save(void);

/* Slot names for the shell: "mb", "s1".."s4". */
const char *app_alarm_slot_name(uint8_t slot);
int app_alarm_slot_by_name(const char *name); /* -1 if unknown */

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_RULES_H_ */
