/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CMD_H_
#define APP_CMD_H_

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1-byte format version prefixed to application protobuf payloads so the
 * encoding can change incompatibly in future while staying decodable (protobuf
 * alone only handles additive field changes). Carried on fPort 2 (telemetry)
 * and fPort 85 (command response). fPort 1 legacy bitmap stays unversioned. */
#define APP_PROTO_VERSION 0x01

enum app_cmd_transport {
	APP_CMD_TRANSPORT_LRW,
	APP_CMD_TRANSPORT_NFC,
	APP_CMD_TRANSPORT_SHELL_DEBUG,
};

/* Action the caller must perform AFTER the response has been sent (so the Ack
 * leaves before the device reboots). Set by app_cmd_handle(). */
enum app_cmd_action {
	APP_CMD_ACTION_NONE = 0,
	APP_CMD_ACTION_SETTINGS_SAVE,     /* persist staged config + reboot */
	APP_CMD_ACTION_REBOOT,            /* reboot (discards staged edits) */
	APP_CMD_ACTION_FACTORY_RESET,     /* defaults (keep identity+LoRaWAN) + reboot */
	APP_CMD_ACTION_ENTER_CALIBRATION, /* persist calibration=true + reboot */
	APP_CMD_ACTION_LRW_RESET,         /* reset LoRaWAN NVM (counters+DevNonce) + reboot (#109) */
	APP_CMD_ACTION_LRW_JOIN,          /* trigger a forced (re)join, no reboot (#109) */
	APP_CMD_ACTION_COUNTERS_SAVE,     /* persist pulse totalizers (no reboot) */
};

/* Plain-C device info snapshot (no protobuf dependency), filled by
 * app_cmd_get_info(). Single source of truth for both the GetInfo response and
 * the `ats device info` shell command. build_type: 0=main, 1=dev, 2=custom. */
struct app_cmd_info {
	uint8_t fw_major;
	uint8_t fw_minor;
	uint8_t fw_patch;
	uint8_t build_type; /* 0=main, 1=dev, 2=custom */
	bool debug;         /* debug build (CONFIG_FW_DEBUG) */
	uint32_t serial_number;
	uint32_t uptime_s;
	bool has_unix_time; /* unix_time valid (RTC synced) */
	uint32_t unix_time; /* UTC seconds since epoch */
};

/* Fill `info` with the current device info: FW version, build type, serial,
 * uptime, and wall-clock time (has_unix_time=false when the RTC is unsynced or
 * the clock module is absent). Single source of truth for GetInfo + shell. */
void app_cmd_get_info(struct app_cmd_info *info);

/* Decode a serialized Command from `in`, dispatch it, encode the
 * resulting Response into `out`.
 *
 * Returns 0 on success with *out_len set to the encoded length. Returns a
 * negative errno only on encode failure (the caller's buffer is too small or
 * the protobuf library refused to serialize). A successful dispatch may still
 * produce an Error{...} response — that is not a function-level failure.
 *
 * On decode failure the function still returns 0 and encodes a
 * Response{ seq=0, error={ code=BAD_REQUEST } } into out.
 *
 * *action (if non-NULL) is set to a deferred action the caller must run after
 * transmitting the response (reboot/save/factory-reset). APP_CMD_ACTION_NONE
 * otherwise.
 */
int app_cmd_handle(enum app_cmd_transport transport, const uint8_t *in, size_t in_len, uint8_t *out,
		   size_t out_cap, size_t *out_len, enum app_cmd_action *action);

/* Build an unsolicited device Info frame (Response{ seq=0, info=... },
 * the same payload a GetInfo command returns) into `out`. Used to send an
 * autonomous GetInfo uplink on join. Returns 0 with *out_len set, -EINVAL on a
 * NULL argument, or -EMSGSIZE if `out_cap` is too small. */
int app_cmd_build_info(uint8_t *out, size_t out_cap, size_t *out_len);

/* Buffer size that always holds one fully-populated history-replay frame
 * (version byte + maximal Response{ seq, history_frame } protobuf). Sized from
 * the nanopb-generated Response_HistoryFrame_size (80) plus the Response
 * envelope (seq + submessage tag/len) plus the 1-byte version prefix, rounded
 * up. The transmit path still clamps the payload to the current data rate; this
 * macro only guarantees the staging buffer is never the binding limit (#89). */
#define APP_CMD_HISTORY_FRAME_BUF_SIZE 96

/* Largest `samples_len` that app_cmd_build_history_frame() can encode into
 * `out_cap` bytes for these frame fields, clamped to the HistoryFrame.samples
 * capacity. Computes the exact protobuf envelope overhead (no fixed guess), so
 * a frame built with <= the returned length never overflows `out` and never
 * serializes to an oversized/empty uplink. Returns 0 when even one sample byte
 * will not fit. Pass worst-case (max-varint) field values to get a stable lower
 * bound across a whole replay. */
size_t app_cmd_history_sample_capacity(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				       uint32_t t0_unix, uint32_t present, uint32_t interval_s,
				       size_t out_cap);

/* Build one history-replay frame (Response{ seq, history_frame={...} }) into
 * `out`. `samples` holds values-only records (the shared `present` mask +
 * `interval_s` describe their layout/timing). Used by the app_lrw replay state
 * machine to stream a ReqHistory window as N frames. Returns 0 with *out_len
 * set, -EINVAL on a NULL/oversized argument, or -EMSGSIZE if it won't encode. */
int app_cmd_build_history_frame(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				uint32_t t0_unix, uint32_t present, uint32_t interval_s,
				const uint8_t *samples, size_t samples_len, uint8_t *out,
				size_t out_cap, size_t *out_len);

/* One alarm edge for app_cmd_build_alarm_report(). source/edge/side carry the
 * AlarmEvent_Source/Edge/Side enum values (app_alarm fills these without
 * including the nanopb header). value is the scaled current reading and is only
 * meaningful when has_value is true (discrete sources leave it absent). */
struct app_cmd_alarm_event {
	uint8_t slot;     /* alarm rule slot index (0..APP_ALARM_SLOT_COUNT-1) that fired */
	uint8_t source;   /* enum app_alarm_source (onboard/s1..s4/hall/input/pir/accel) */
	uint8_t quantity; /* enum app_alarm_quantity */
	uint8_t edge;     /* AlarmEvent_Edge: 0=activate, 1=deactivate */
	uint8_t side;     /* AlarmEvent_Side: 0=none, 1=lo, 2=hi */
	bool has_value;   /* value present */
	int32_t value;    /* scaled value (×100 temp/hum, ×10 pressure, digital 0/1, counter) */
	uint32_t rel_s;   /* seconds since base_time */
};

/* Build an alarm-detail batch (AlarmReport) for fPort 3 (#27) into `out`.
 * `events[0..n_events)` are encoded (capped to the message's 8-event array);
 * `total` is the true window count and may exceed the encoded events when the
 * caller trimmed to fit the data rate. Returns 0 with *out_len set, -EINVAL on
 * a NULL argument, or -EMSGSIZE if it won't fit `out_cap`. */
int app_cmd_build_alarm_report(uint32_t base_time, uint32_t total,
			       const struct app_cmd_alarm_event *events, size_t n_events,
			       uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* APP_CMD_H_ */
