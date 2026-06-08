/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_W1_SLOTS_H_
#define APP_W1_SLOTS_H_

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of logical 1-Wire sensor slots. Bounded by the per-slot config keys
 * (sensor1..4_*) and, for heterogeneous use, by the devicetree driver
 * instances. Grow together with those. */
#define APP_W1_SLOT_COUNT 4

/* Slot sensor type. Persisted in config (sensorN_type) and used to dispatch
 * which transport driver reads the slot. Extend with new families here +
 * one entry in the type registry in app_w1_slots.c. */
enum app_w1_slot_type {
	APP_W1_SLOT_EMPTY = 0,
	APP_W1_SLOT_DALLAS = 1,        /* DS18B20, family 0x28 — temperature */
	APP_W1_SLOT_MACHINE_PROBE = 2, /* DS28E17 bridge, family 0x19 — temp + humidity + tilt */
};

/* One slot's latest readings. Quantities a slot's type doesn't provide are
 * NaN (floats) / false (tilt). present=false when the bound ROM was not seen
 * on the last scan (alarms stay inert via NaN). */
struct app_w1_slot_reading {
	float temperature; /* degC, NaN if absent/unsupported */
	float humidity;    /* %RH, NaN unless machine-probe */
	bool is_tilt_alert;
	bool present;
};

/* Re-bind logical slots to discovered driver indices by ROM-serial match.
 * Call once at init AFTER app_ds18b20_scan() / app_machine_probe_scan() have
 * run. For each configured slot (sensorN_rom != 0) finds the driver index
 * whose ROM serial matches and records it (stable across reboots regardless of
 * discovery order). Devices present but matching no slot are auto-enrolled into
 * the lowest free slot and persisted (one-time migration / first bind). A
 * configured ROM not seen this scan leaves the slot present=false. A different
 * device where one was bound is flagged replaced (kept, not silently rebound).
 * Returns the number of present (bound) slots, or negative errno. */
int app_w1_slots_rebind(void);

/* Read a slot's current values through its bound driver. slot is 0-based
 * (0..APP_W1_SLOT_COUNT-1). Fills *out (present=false + NaN when unbound /
 * absent). Returns 0 on success, negative errno on a read error. */
int app_w1_slots_read(int slot, struct app_w1_slot_reading *out);

/* Slot metadata accessors (0-based slot index). */
enum app_w1_slot_type app_w1_slot_get_type(int slot);
uint64_t app_w1_slot_get_rom(int slot); /* 48-bit serial, 0 = empty */
bool app_w1_slot_is_present(int slot);
bool app_w1_slot_is_replaced(int slot); /* configured ROM absent but a same-type device appeared */

#ifdef __cplusplus
}
#endif

#endif /* APP_W1_SLOTS_H_ */
