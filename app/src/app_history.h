/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_HISTORY_H_
#define APP_HISTORY_H_

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Analog + counter channels that can be stored in the history buffer. The order
 * is the wire order within a record and the bit order of the selection mask. */
enum app_history_sensor {
	APP_HISTORY_TEMPERATURE = 0,
	APP_HISTORY_HUMIDITY,
	APP_HISTORY_EXT1,
	APP_HISTORY_EXT2,
	APP_HISTORY_MP1_TEMP,
	APP_HISTORY_MP2_TEMP,
	APP_HISTORY_MP1_HUM,
	APP_HISTORY_MP2_HUM,
	APP_HISTORY_HALL_LEFT,
	APP_HISTORY_HALL_RIGHT,
	APP_HISTORY_INPUT_A,
	APP_HISTORY_INPUT_B,
	APP_HISTORY_MOTION,
	APP_HISTORY_SENSOR_COUNT
};

/* A decoded record handed to the shell. `value[i]` is valid only when
 * `present` has bit i set; counters are whole numbers, analog values are in
 * physical units (deg C, %RH). `time_unix` is absolute UTC when `time_synced`,
 * otherwise it is seconds relative to the buffer base (no wall-clock yet). */
struct app_history_record {
	uint32_t time_unix;
	bool time_synced;
	uint16_t present;
	double value[APP_HISTORY_SENSOR_COUNT];
};

/* Initialize the history subsystem: mount the flash backend (or the RAM ring),
 * seed enable + sensor mask from g_app_config, and rebuild buffer state.
 * Returns 0 on success or a negative errno. */
int app_history_init(void);

/* Capture one record from the current g_app_sensor_data (called once per
 * interval_report). No-op when history is disabled. */
void app_history_capture(void);

/* Fix up the buffer base time once the wall-clock becomes available, so all
 * stored records gain correct absolute timestamps. Idempotent. */
void app_history_on_clock_sync(uint32_t unix_now);

/* Number of records currently stored (0..capacity). */
size_t app_history_count(void);

/* Capacity in records for the current sensor selection. */
size_t app_history_capacity(void);

/* Erase all stored records and reset the base time. */
void app_history_clear(void);

/* Decode the record at ordinal `idx` (0 = oldest). Returns 0 on success,
 * -ENOENT when idx >= count. */
int app_history_get(size_t idx, struct app_history_record *out);

/* Master enable (mirrors g_app_config.history_enable at boot). */
bool app_history_is_enabled(void);
void app_history_set_enabled(bool enable);

/* Sensor selection mask (bit i = enum app_history_sensor i). Setting a new mask
 * clears the buffer (record layout changes). Sensors whose capability is off
 * are silently dropped from the mask. */
uint16_t app_history_get_mask(void);
void app_history_set_mask(uint16_t mask);

/* Serialize stored records in the window [from_unix, to_unix] into `buf` for a
 * LoRaWAN replay (ReqHistory -> HistoryFrame). Records are written oldest-first,
 * as many as fit in `cap`, each: delta_s (uint16 LE, from *t0_out) + present
 * (uint16 LE) + per present sensor a scaled value (int16 LE x100 temp / uint8 x2
 * humidity / uint32 LE counter), in enum order. When the buffer has no absolute
 * time yet (RTC not synced) the window filter is ignored. Returns bytes written;
 * *t0_out = first record's timestamp, *n_written = records packed, *total = total
 * records matching the window (so the host knows how many pages remain).
 * For the next page the host narrows from_unix to the last returned time + 1. */
size_t app_history_export(uint32_t from_unix, uint32_t to_unix, uint8_t *buf, size_t cap,
			  uint32_t *t0_out, uint16_t *n_written, uint16_t *total);

/* Descriptor helpers for the shell. */
const char *app_history_sensor_name(enum app_history_sensor s);
enum app_history_sensor app_history_sensor_by_name(const char *name);
bool app_history_sensor_available(enum app_history_sensor s);
uint16_t app_history_available_mask(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_HISTORY_H_ */
