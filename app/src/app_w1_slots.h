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

/* Human-readable name for a slot type ("dallas", "machine-probe", "empty"),
 * from the sensor-type registry. */
const char *app_w1_slot_type_name(enum app_w1_slot_type type);

/* Slot metadata accessors (0-based slot index). */
enum app_w1_slot_type app_w1_slot_get_type(int slot);
uint64_t app_w1_slot_get_rom(int slot); /* 48-bit serial, 0 = empty */
bool app_w1_slot_is_present(int slot);
bool app_w1_slot_is_replaced(int slot); /* configured ROM absent but a same-type device appeared */

/* One device seen on the bus during a scan, for the `sensor` shell. */
struct app_w1_scan_entry {
	uint64_t serial;            /* 48-bit ROM serial */
	enum app_w1_slot_type type; /* detected from family code */
	int bound_slot;             /* 0-based slot this ROM is bound to, or -1 */
};

/* Re-scan both transport drivers and list every device currently on the bus
 * (serial + detected type + which slot it's bound to). Fills up to max entries,
 * returns the count or negative errno. Used by `sensor scan` / `teach`. */
int app_w1_slots_scan(struct app_w1_scan_entry *out, int max);

/* Plug-one enrollment: scan, and if exactly one device is unbound, bind it to
 * `slot` (0-based) — records ROM + detected type in the staging config (durable
 * after `config save`). On success fills *bound. Returns 0, -EAGAIN (no unbound
 * device found), -E2BIG (more than one — use assign), or other negative errno. */
int app_w1_slots_teach(int slot, struct app_w1_scan_entry *bound);

/* Explicit enrollment: bind the device with ROM `serial` to `slot`. The device
 * must be present on the bus (its type is taken from the scan). Returns 0,
 * -ENODEV (serial not on the bus), -EEXIST (already bound elsewhere), errno. */
int app_w1_slots_assign(int slot, uint64_t serial);

/* Forget a slot's binding (staging config; durable after `config save`). */
int app_w1_slots_clear(int slot);

#ifdef __cplusplus
}
#endif

#endif /* APP_W1_SLOTS_H_ */
