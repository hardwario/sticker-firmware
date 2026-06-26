/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ALARM_RULES_H_
#define APP_ALARM_RULES_H_

/* Dynamic alarm rules — a small, persisted array of {source, quantity, condition}
 * rules that unifies every alarm (onboard sensors, ROM-bound 1-Wire slots,
 * discrete inputs and counters) under one model. Replaces the fixed per-source
 * flat config keys: a new sensor quantity is one enum value, and config grows
 * only with the rules actually set.
 *
 * Storage lives in app_config: each slot is a `bytes` config parameter
 * (alarm_0..alarm_15), so rules are set/read over SetParam/GetParam like any
 * other parameter (manager-app); the `alarm` shell edits the same slots. m_slots
 * here is just a decoded cache, rebuilt from app_config on load and after every
 * change. APP_ALARM_SLOT_COUNT slots.
 *
 * Rules live in fixed alarm slots 0..APP_ALARM_SLOT_COUNT-1. The slot index is
 * the rule's stable identity: several slots may carry the same (source,quantity)
 * — that is the multi-level case (e.g. a warning band and a critical band on one
 * sensor as two independent rules). Clearing a slot empties it without
 * renumbering the others, so a host / AlarmReport can refer to a rule by slot
 * index reliably. NOTE: the rule "slot" (this index) is distinct from the
 * 1-Wire sensor "slot" (APP_ALARM_SRC_SLOT1..4); shell/proto call this one the
 * rule index.
 *
 * A rule's kind is derived from its quantity:
 *  - THRESHOLD (analog): lo/hi/hst hysteresis band — temperature, humidity,
 *    pressure, illuminance, magnetic_field.
 *  - STATE (digital 0/1): from_state/to_state — from != to is an edge (one-shot
 *    alarm; the red LED blinks for alarm_notif_time), from == to is a level
 *    (alarm active while the state == to). tilt and the discrete inputs.
 *  - COUNT (rate): hi = max counter increase allowed per report interval.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_ALARM_SLOT_COUNT 16

/* Physical sensor a rule targets. Slot 1..4 mirror the telemetry/history slot
 * model (ROM-bound w1[0..3]). Discrete + counter sources reuse these too. */
enum app_alarm_source {
	APP_ALARM_SRC_ONBOARD = 0, /* on-board SHT4x / MPL3115A2 */
	APP_ALARM_SRC_SLOT1 = 1,
	APP_ALARM_SRC_SLOT2 = 2,
	APP_ALARM_SRC_SLOT3 = 3,
	APP_ALARM_SRC_SLOT4 = 4,
	APP_ALARM_SRC_HALL_LEFT = 5,
	APP_ALARM_SRC_HALL_RIGHT = 6,
	APP_ALARM_SRC_INPUT_A = 7,
	APP_ALARM_SRC_INPUT_B = 8,
	APP_ALARM_SRC_PIR = 9,
	APP_ALARM_SRC_ACCEL = 10,
	APP_ALARM_SRC_BATTERY =
		11, /* system supply rail — low-battery watchdog (#210), not a rule slot */
	APP_ALARM_SRC_COUNT
};

/* What is measured. THRESHOLD quantities use lo/hi/hst; STATE uses from/to;
 * COUNT uses hi as the per-interval rate limit. Extend here + in the validity
 * table; the wire (AlarmEvent.quantity) and ttn.js mirror these values. */
enum app_alarm_quantity {
	APP_ALARM_Q_TEMPERATURE = 0,
	APP_ALARM_Q_HUMIDITY = 1,
	APP_ALARM_Q_PRESSURE = 2,
	APP_ALARM_Q_ILLUMINANCE = 3,
	APP_ALARM_Q_MAGNETIC_FIELD = 4,
	APP_ALARM_Q_TILT = 5,    /* digital (STATE) */
	APP_ALARM_Q_STATE = 6,   /* digital line of a discrete input (STATE) */
	APP_ALARM_Q_COUNT = 7,   /* counter rate (COUNT) */
	APP_ALARM_Q_VOLTAGE = 8, /* supply voltage in V (THRESHOLD) — battery watchdog (#210) */
	APP_ALARM_Q_QUANTITY_COUNT
};

enum app_alarm_kind {
	APP_ALARM_KIND_THRESHOLD, /* lo/hi/hst */
	APP_ALARM_KIND_STATE,     /* from_state/to_state */
	APP_ALARM_KIND_RATE,      /* hi = max delta per report interval */
};

struct app_alarm_rule {
	uint8_t source;   /* enum app_alarm_source */
	uint8_t quantity; /* enum app_alarm_quantity */
	uint8_t enabled;
	uint8_t from_state; /* STATE only: previous level (0/1) */
	uint8_t to_state;   /* STATE only: triggering level (0/1); from==to => level */
	float lo, hi, hst;  /* THRESHOLD (and hi for RATE) */
};

/* Build the rule cache from the app_config slots (called once at init, after
 * app_config has loaded). */
int app_alarm_rules_init(void);

/* Rebuild the rule cache from the app_config alarm_N slots. Call after a
 * SetParam that touched the alarms group so the runtime reflects it without a
 * reboot. */
void app_alarm_rules_reload_from_config(void);

/* The kind implied by a quantity (THRESHOLD / STATE / RATE). */
enum app_alarm_kind app_alarm_quantity_kind(enum app_alarm_quantity q);

/* True if (source, quantity) is a structurally valid pair (onboard → temp/hum/
 * pressure; slot → temp/hum/lux/mag/tilt; hall/input → state/count; pir/accel →
 * state/count). Does not require the sensor to be present (provisioning may set
 * a slot rule before the sensor is enrolled). */
bool app_alarm_rule_valid(enum app_alarm_source source, enum app_alarm_quantity quantity);

/* Number of occupied slots, and per-slot access. Iterate occupied rules with
 *   for (uint8_t s = 0; s < APP_ALARM_SLOT_COUNT; s++)
 *           if ((r = app_alarm_rules_get(s))) { ... }
 * app_alarm_rules_get() returns NULL for an out-of-range or empty slot. */
uint8_t app_alarm_rules_count(void);
const struct app_alarm_rule *app_alarm_rules_get(uint8_t slot);
bool app_alarm_rules_occupied(uint8_t slot);

/* Lowest empty slot, or -1 if all APP_ALARM_SLOT_COUNT slots are occupied. */
int app_alarm_rules_first_free(void);

/* Store a rule in `slot`, overwriting whatever was there. Returns 0, -EINVAL
 * (bad slot or invalid source+quantity pair). Does not persist — call
 * app_alarm_rules_save(). */
int app_alarm_rules_set(uint8_t slot, const struct app_alarm_rule *rule);

/* Empty `slot` (no renumbering). Returns 0, -EINVAL (bad slot), -ENOENT (already
 * empty). Not persisted. */
int app_alarm_rules_clear(uint8_t slot);

/* Empty all slots. Not persisted. */
void app_alarm_rules_clear_all(void);

/* Persist the rules. They live in the app_config slots, so this saves the
 * config (storage NVS) without rebooting. */
int app_alarm_rules_save(void);

/* Names for shell / decoder parity. */
const char *app_alarm_source_name(enum app_alarm_source source);
const char *app_alarm_quantity_name(enum app_alarm_quantity quantity);
int app_alarm_source_by_name(const char *name);   /* -1 if unknown */
int app_alarm_quantity_by_name(const char *name); /* -1 if unknown */

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_RULES_H_ */
