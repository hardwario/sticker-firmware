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

/* Slot sensor type = the sensor type id of app_sensor_types.yaml (#430), so
 * config (sensorN_type), telemetry (SensorReading.type), ConfigDump.w1_slot_type
 * and history (HistoryFrame.w1_types) all carry the same number. Extend with a
 * new registry entry + one entry in the type table in app_w1_slots.c (a
 * BUILD_ASSERT there pins these values to APP_SENSOR_TYPE_*). Wire-visible, so
 * never renumber an existing entry. */
enum app_w1_slot_type {
	APP_W1_SLOT_EMPTY = 0,         /* APP_SENSOR_TYPE_NONE */
	APP_W1_SLOT_DALLAS = 2,        /* DS18B20, family 0x28 — temperature */
	APP_W1_SLOT_MACHINE_PROBE = 3, /* DS28E17 bridge, family 0x19 — temp + humidity + tilt */
};

/* Slot state after a rebind (#430 step 3), reported per slot in
 * Response.Info.w1_slot_state. Wire-visible: append-only. */
enum app_w1_slot_state {
	APP_W1_SLOT_STATE_NONE = 0,     /* no ROM and no expected type: unused slot */
	APP_W1_SLOT_STATE_OK = 1,       /* bound device present */
	APP_W1_SLOT_STATE_ABSENT = 2,   /* ROM or expected type set, no device for it */
	APP_W1_SLOT_STATE_REPLACED = 3, /* ROM absent, an unbound same-type device appeared */
	APP_W1_SLOT_STATE_MISMATCH = 4, /* ROM / expected type absent, a different type appeared */
};

/* A slot's readings are a channel vector of its sensor type (struct
 * app_sensor_w1 in app_sensor.h, #430). The machine-probe carries a whole
 * sensor cluster (SHT temp/hum, TMP112 temp, OPT3001 lux, Si7210 field,
 * LIS2DH12 accel + tilt); a Dallas slot fills only its temperature. A
 * sub-sensor that fails to respond (e.g. an unpopulated TMP112 on older probe
 * revisions) stays NaN without failing the whole slot read. */
struct app_sensor_w1;

/* True if at least one 1-Wire slot is taught (has a non-zero configured ROM,
 * `sensorN-rom`). Used to skip the pointless boot sensor init+scan when
 * `cap-w1-sensors` is on but nothing is enrolled. */
bool app_w1_slots_any_taught(void);

/* Re-bind logical slots to discovered driver indices by ROM-serial match.
 * Call once at init AFTER app_ds18b20_scan() / app_machine_probe_scan() have
 * run, and after every rescan (shell scan / teach / clear).
 *  1. Each slot with a configured ROM (sensorN_rom != 0) binds to the device
 *     with that serial (stable across reboots regardless of discovery order);
 *     its type is written to sensorN_type.
 *  2. Each unbound device is auto-enrolled into the lowest slot without a ROM
 *     whose expected type (sensorN_type) is its type or none, and persisted.
 *  3. A slot left without a device is REPLACED when an unbound device of its
 *     type was on the bus, and MISMATCH when an unbound device of a different
 *     type is still on the bus (no free slot expects that type); one foreign
 *     device marks one slot, lowest first. Otherwise ABSENT (or NONE when the
 *     slot expects nothing). A swapped probe is flagged, never silently rebound.
 * Returns the number of present (bound) slots, or negative errno. */
int app_w1_slots_rebind(void);

/* Read a slot's current values through its bound driver. slot is 0-based
 * (0..APP_W1_SLOT_COUNT-1). Fills *out (type + channels; present=false and all
 * channels NaN when unbound / absent). Returns 0 on success, negative errno on
 * a read error. */
int app_w1_slots_read(int slot, struct app_sensor_w1 *out);

/* Encode a slot's reading into its telemetry SensorReading (Telemetry field 27),
 * dispatched to the slot type's driver — the caller (composer) owns the slot
 * index, type and the repeated array; this fills only the value fields the
 * driver provides (a Dallas slot fills temperature, a machine-probe the whole
 * cluster). NaN quantities stay absent. `sr` is a nanopb SensorReading
 * (forward-declared so this header stays protobuf-free; the dispatch lives in
 * app_w1_slots.c, the HW drivers never see the wire schema). No-op for an
 * unknown/empty type. */
struct _SensorReading;
void app_w1_slot_encode(int slot, const struct app_sensor_w1 *r, struct _SensorReading *sr);

/* Human-readable name for a slot type ("dallas", "machine-probe", "empty"),
 * from the sensor-type registry. */
const char *app_w1_slot_type_name(enum app_w1_slot_type type);

/* Slot metadata accessors (0-based slot index). */
enum app_w1_slot_type app_w1_slot_get_type(int slot);
uint64_t app_w1_slot_get_rom(int slot);   /* 48-bit serial, 0 = empty */
bool app_w1_slot_is_configured(int slot); /* a probe was taught to this slot (persisted ROM != 0) */
bool app_w1_slot_is_present(int slot);
enum app_w1_slot_state app_w1_slot_get_state(int slot);
/* Expected type of the slot (sensorN_type, a registry id; 0 = none). Set by a
 * bind / enroll, or by provisioning before the probe is plugged in. */
uint8_t app_w1_slot_get_expected_type(int slot);
/* Type of the foreign device that put the slot in MISMATCH (registry id), 0
 * in any other state. */
uint8_t app_w1_slot_get_detected_type(int slot);

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
 * after `settings save`). On success fills *bound. Returns 0, -EAGAIN (no unbound
 * device found), -E2BIG (more than one — use assign), -EINVAL (the device's type
 * differs from the slot's set sensorN_type — clear the slot first), or other
 * negative errno. */
int app_w1_slots_teach(int slot, struct app_w1_scan_entry *bound);

/* Explicit enrollment: bind the device with ROM `serial` to `slot`. The device
 * must be present on the bus (its type is taken from the scan). Returns 0,
 * -ENODEV (serial not on the bus), -EEXIST (already bound elsewhere), -EINVAL
 * (type differs from the slot's set sensorN_type), errno. */
int app_w1_slots_assign(int slot, uint64_t serial);

/* Forget a slot's binding: ROM and expected type (staging config; durable
 * after `settings save`). */
int app_w1_slots_clear(int slot);

/* Batch enroll: scan the bus and bind every unbound device into the lowest free
 * slots (staging config; durable after `settings save`). Returns bound count. */
int app_w1_slots_enroll_all(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_W1_SLOTS_H_ */
