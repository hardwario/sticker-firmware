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
 * is the wire order within a record and the bit order of the selection mask
 * (uint32 → up to 32 channels). The 1-Wire slots mirror the telemetry slot model
 * (s1..s4 = ROM-bound w1[0..3]); each slot stores temperature + humidity (a
 * Dallas slot leaves humidity at the sentinel). The selection mask lets a
 * deployment enable only the channels it cares about. */
enum app_history_sensor {
	APP_HISTORY_TEMPERATURE = 0,
	APP_HISTORY_HUMIDITY,
	APP_HISTORY_S1_TEMP,
	APP_HISTORY_S1_HUM,
	APP_HISTORY_S2_TEMP,
	APP_HISTORY_S2_HUM,
	APP_HISTORY_S3_TEMP,
	APP_HISTORY_S3_HUM,
	APP_HISTORY_S4_TEMP,
	APP_HISTORY_S4_HUM,
	APP_HISTORY_HALL_LEFT,
	APP_HISTORY_HALL_RIGHT,
	APP_HISTORY_INPUT_A,
	APP_HISTORY_INPUT_B,
	APP_HISTORY_MOTION,
	/* #311: barometer/light/accelerometer — already sampled + in telemetry,
	 * newly recordable in history. accel_motion is the LIS2DH any-motion event
	 * counter, distinct from MOTION above (PIR person-detection). */
	APP_HISTORY_PRESSURE,
	APP_HISTORY_ILLUMINANCE,
	APP_HISTORY_ORIENTATION,
	APP_HISTORY_ACCEL_MOTION,
	APP_HISTORY_SENSOR_COUNT
};

/* A decoded record handed to the shell. `value[i]` is valid only when
 * `present` has bit i set; counters are whole numbers, analog values are in
 * physical units (deg C, %RH). `time_unix` is absolute UTC when `time_synced`,
 * otherwise it is seconds relative to the buffer base (no wall-clock yet). */
struct app_history_record {
	uint32_t time_unix;
	bool time_synced;
	uint32_t present;
	double value[APP_HISTORY_SENSOR_COUNT];
};

/* Initialize the history subsystem: mount the flash backend (or the RAM ring),
 * seed enable + sensor mask from g_app_config, and rebuild buffer state.
 * Returns 0 on success or a negative errno. */
int app_history_init(void);

/* Capture one record from the current g_app_sensor_data (called once per
 * interval_report). No-op when history is disabled or while a replay is active. */
void app_history_capture(void);

/* Pause/resume history capture while a LoRaWAN replay is streaming records back
 * (#126). app_lrw sets it true at replay start and false at finish; capture
 * self-skips in between so the buffer it is replaying can't shift underneath it. */
void app_history_set_replay_active(bool active);

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

/* True if the history backend is ready. Flash backend: the partition mounted OK
 * (false = mount failed, history degraded, #88 style). RAM backend: always true. */
bool app_history_is_ready(void);
void app_history_set_enabled(bool enable);

/* Sensor selection mask (bit i = enum app_history_sensor i). Setting a new mask
 * clears the buffer (record layout changes). Sensors whose capability is off
 * are silently dropped from the mask. */
uint32_t app_history_get_mask(void);
void app_history_set_mask(uint32_t mask);

/* interval_report (s) the buffer is currently recorded at; records are periodic
 * so a wire frame carries this once and per-record time = t0 + ord*interval. */
uint32_t app_history_get_interval(void);

/* True once the buffer's base time has been anchored to absolute UTC (the RTC
 * synced while records were held). Until then export_page's t0 is uptime-relative,
 * so the replay frame must carry time_synced=false (L-1/L-3). */
bool app_history_base_synced(void);

/* Pack one page of stored records into `buf` for a LoRaWAN replay (ReqHistory ->
 * HistoryFrame), starting at ordinal `start_ord` (0 = oldest), oldest-first, as
 * many whole records as fit in `cap`. Each record is the raw stored bytes (values
 * only, fixed size = the sample size, sentinels mark absent values); the shared
 * present mask + interval travel in the frame header, not per record. Records in
 * [from_unix, to_unix] only (filter skipped until the clock is synced). Returns
 * bytes written; *t0_out = first packed record's absolute time, *n_written =
 * records packed, *next_ord = next ordinal to pass for the following page
 * (== app_history_count() when the scan is exhausted). */
size_t app_history_export_page(uint32_t from_unix, uint32_t to_unix, size_t start_ord, uint8_t *buf,
			       size_t cap, uint32_t *t0_out, uint16_t *n_written, size_t *next_ord);

/* Number of frames the [from_unix, to_unix] window needs at `cap` bytes/frame
 * (whole records per frame). Mirrors export_page's packing so the replay can
 * announce frame_count up front. */
uint16_t app_history_count_frames(uint32_t from_unix, uint32_t to_unix, size_t cap);

/* Descriptor helpers for the shell. */
enum app_history_sensor app_history_sensor_by_name(const char *name);
bool app_history_sensor_available(enum app_history_sensor s);
uint32_t app_history_available_mask(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_HISTORY_H_ */
