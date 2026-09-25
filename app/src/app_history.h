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
 * otherwise it is uptime seconds of the boot that recorded the record (no
 * wall-clock yet). */
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
 * interval_report). No-op when history is disabled. Keeps capturing while a
 * replay streams records back (the replay cursor is absolute, see
 * app_history_export_abs()). */
void app_history_capture(void);

/* Tell history that a LoRaWAN replay is streaming records back (#126). app_lrw
 * sets it true at replay start and false at finish. Capture goes on; only the
 * flash backend holds off its page rollover (a ~20 ms erase that would stall
 * the replay's RX windows) — a record that needs the next page meanwhile is
 * dropped. */
void app_history_set_replay_active(bool active);

/* The RTC was set to `unix_now`: re-base the segments recorded on this boot's
 * uptime (before the RTC was set) to unix time, so their records gain absolute
 * timestamps. Segments of an earlier boot that never saw the clock stay
 * unsynced. Idempotent. */
void app_history_on_clock_sync(uint32_t unix_now);

/* Work queue for deferred history flash maintenance (the clock-sync fix-up
 * double word, flash backend): app_report registers its own queue, the same
 * context the captures (and their flash writes) run in. NULL = none; the next
 * capture then writes pending fix-ups. */
struct k_work_q;
void app_history_set_work_queue(struct k_work_q *queue);

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

/* Pack one HistoryFrame's worth of stored records into `buf` (NFC paged read,
 * ReqHistoryPage), starting at ordinal `start_ord` (0 = oldest), oldest-first, as
 * many whole records as fit in `cap`. Each record is the raw stored bytes (values
 * only, fixed size = the sample size, sentinels mark absent values); the shared
 * present mask + interval travel in the frame header, not per record.
 *
 * Record times are periodic within a segment (a flash page, stamped from the
 * clock when it was opened), and a frame never crosses a segment boundary, so
 * time(j) = t0 + j * interval holds for every frame. Records in [from_unix,
 * to_unix] only; records whose segment was stamped before the RTC was set
 * (unsynced) are returned only for an open window (0..UINT32_MAX) or while the
 * device has no wall clock. Returns bytes written; *t0_out = first packed
 * record's time, *synced_out = true when that time is unix (time_synced of the
 * frame; false = uptime, L-1/L-3), *n_written = records packed, *next_ord = next
 * ordinal to pass for the following page (== app_history_count() when the scan
 * is exhausted). Output pointers may be NULL. */
size_t app_history_export_page(uint32_t from_unix, uint32_t to_unix, size_t start_ord, uint8_t *buf,
			       size_t cap, uint32_t *t0_out, bool *synced_out, uint16_t *n_written,
			       size_t *next_ord);

/* Absolute-ordinal span [*first_abs, *end_abs) of the stored records. An
 * absolute ordinal names one record for as long as it is stored: appends don't
 * move it, eviction only raises first_abs, and a logical reset (clear / layout /
 * interval change) starts past every earlier end_abs. Either pointer may be
 * NULL. */
void app_history_span(uint32_t *first_abs, uint32_t *end_abs);

/* app_history_export_page() on absolute ordinals, for the LoRaWAN replay:
 * packs records from `start_abs` (clamped up to the oldest stored record when it
 * was evicted meanwhile) up to `end_abs` (exclusive, clamped to the newest).
 * *next_abs = cursor for the following frame; the scan is exhausted once it
 * reaches `end_abs`. */
size_t app_history_export_abs(uint32_t from_unix, uint32_t to_unix, uint32_t start_abs,
			      uint32_t end_abs, uint8_t *buf, size_t cap, uint32_t *t0_out,
			      bool *synced_out, uint16_t *n_written, uint32_t *next_abs);

/* Number of frames the [from_unix, to_unix] window needs at `cap` bytes/frame
 * (whole records per frame, one frame never spans two segments). Mirrors
 * export_page's packing so the replay can announce frame_count up front. */
uint16_t app_history_count_frames(uint32_t from_unix, uint32_t to_unix, size_t cap);

/* Descriptor helpers for the shell. */
enum app_history_sensor app_history_sensor_by_name(const char *name);
bool app_history_sensor_available(enum app_history_sensor s);
uint32_t app_history_available_mask(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_HISTORY_H_ */
