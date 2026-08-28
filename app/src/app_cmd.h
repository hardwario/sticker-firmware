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
	/* Raw-LoRa P2P downlink command (0x56), dispatched by app_p2p.c (#118 B4).
	 * Same generic Command/Response dispatch and writability gating as the
	 * LoRaWAN transport -- P2P is the network-server-less equivalent. */
	APP_CMD_TRANSPORT_P2P,
	/* NFC hio.stck:vnd record, authenticated with vendor_token instead of
	 * secret_key (#316). Runs the same generic Command/Response dispatch; gates
	 * the vendor-only command (vendor_reset) and writable:[vendor] fields. */
	APP_CMD_TRANSPORT_VENDOR,
};

/* Aggregated device status reported in Device Info (device_status, fPort 85 +
 * NFC). uint32 bitmask; proto3 omits a 0 so an empty field means "all OK /
 * nothing active". Low bits are alarm categories (derived read-only from the
 * alarm latches, no side effects), high bits are health/degradation. The raw
 * LoRaWAN link state is NOT duplicated here (it has its own lrw_state field);
 * only the derived LRW_DISABLED bit is included. Bit positions are stable --
 * never renumber; new states take new bits. Plain (1u << n), not zephyr BIT(),
 * so this header stays free of zephyr includes. */
#define APP_DEVICE_STATUS_ALARM_ANY       (1u << 0) /* any alarm latched active */
#define APP_DEVICE_STATUS_ALARM_THRESHOLD (1u << 1) /* analog threshold rule active */
#define APP_DEVICE_STATUS_ALARM_STATE     (1u << 2) /* discrete state rule active */
#define APP_DEVICE_STATUS_ALARM_RATE      (1u << 3) /* counter-rate rule active */
#define APP_DEVICE_STATUS_ALARM_NO_DATA   (1u << 4) /* no-data watchdog latched */
#define APP_DEVICE_STATUS_ALARM_LOW_BATT  (1u << 5) /* low-battery watchdog latched (#210) */
/* bits 6..7 reserved for future alarm categories */
#define APP_DEVICE_STATUS_NFC_DOWN        (1u << 8)  /* NFC (ST25DV) init failed, degraded */
#define APP_DEVICE_STATUS_HISTORY_DOWN    (1u << 9)  /* history flash mount failed */
#define APP_DEVICE_STATUS_I2C_WEDGED      (1u << 10) /* I2C bus wedged (fail streak >= threshold) */
#define APP_DEVICE_STATUS_TIME_UNSYNCED   (1u << 11) /* RTC not synced (no wall-clock) */
#define APP_DEVICE_STATUS_LRW_DISABLED    (1u << 12) /* radio-silent: DevEUI all-zero (#98) */

/* Action the caller must perform AFTER the response has been sent (so the Ack
 * leaves before the device reboots). Set by app_cmd_handle(). */
enum app_cmd_action {
	APP_CMD_ACTION_NONE = 0,
	APP_CMD_ACTION_SETTINGS_SAVE, /* persist staged config + reboot */
	APP_CMD_ACTION_REBOOT,        /* reboot (discards staged edits) */
	/* Reset ladder (#299) — device_reset (renamed from the old, single
	 * factory_reset) keeps identity+full LoRaWAN; factory_reset (new, narrower)
	 * keeps identity only, drops the LoRaWAN session/keys. vendor_reset is
	 * reachable only over the vendor transport — the NFC hio.stck:vnd record
	 * (decrypted with vendor_token, app_nfc.c) dispatched as a generic Command
	 * (#316), or the shell `settings vendor-reset`. */
	APP_CMD_ACTION_DEVICE_RESET,      /* defaults (keep identity+LoRaWAN) + reboot */
	APP_CMD_ACTION_FACTORY_RESET,     /* defaults (keep identity only) + reboot */
	APP_CMD_ACTION_VENDOR_RESET,      /* defaults (keep serial+vendor_token) + reboot */
	APP_CMD_ACTION_ENTER_CALIBRATION, /* persist calibration=true + reboot */
	APP_CMD_ACTION_LRW_RESET,       /* reset LoRaWAN NVM (counters+DevNonce) + reboot (#109) */
	APP_CMD_ACTION_LRW_JOIN,        /* trigger a forced (re)join, no reboot (#109) */
	APP_CMD_ACTION_COUNTERS_SAVE,   /* persist pulse totalizers (no reboot) */
	APP_CMD_ACTION_SECRET_KEY_SAVE, /* persist the new secret_key + reboot (#299, #322) */
	APP_CMD_ACTION_CLM_REARM_SAVE,  /* persist new claim_token + reboot, then re-arm clm (#351)
					 */
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
	bool has_unix_time;      /* unix_time valid (RTC synced) */
	uint32_t unix_time;      /* UTC seconds since epoch */
	uint8_t claim_token[16]; /* 128-bit device claim token (#170); all-zero = uncommissioned */
	uint32_t battery_mv;     /* supply voltage in mV; 0 = measurement unavailable */
	uint32_t reset_cause;   /* hwinfo reset-cause bitmask of the last boot (#88); 0 = unknown */
	uint8_t lrw_state;      /* current LoRaWAN state (enum app_lrw_state) */
	uint8_t dev_eui[8];     /* LoRaWAN DevEUI; all-zero = unset */
	uint32_t device_status; /* aggregated status (APP_DEVICE_STATUS_* bitmask) */
};

/* Cache the hwinfo reset-cause bitmask read once at boot (RESET_* flags from
 * <zephyr/drivers/hwinfo.h>). Reported back in GetInfo so a watchdog/brownout
 * reset is visible in the field (#88). */
void app_cmd_set_reset_cause(uint32_t cause);

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

/* The replacement secret_key staged by a successful vendor_reset Command (#316,
 * carried over the NFC hio.stck:vnd vendor-token channel) — call exactly once,
 * when handling the deferred APP_CMD_ACTION_VENDOR_RESET set by app_cmd_handle().
 * Returns a pointer to a static 16-byte buffer valid until the next vendor_reset.
 * vendor_reset zeroes secret_key, so this replacement is mandatory to keep the
 * encrypted channel usable (see app_settings_vendor_reset). */
const uint8_t *app_cmd_take_pending_vendor_secret_key(void);

/* Build an unsolicited device Info frame (Response{ seq=0, info=... },
 * the same payload a GetInfo command returns) into `out`. Used to send an
 * autonomous GetInfo uplink on join. Returns 0 with *out_len set, -EINVAL on a
 * NULL argument, or -EMSGSIZE if `out_cap` is too small. */
int app_cmd_build_info(uint8_t *out, size_t out_cap, size_t *out_len);

/* Staging buffer for one LoRaWAN history-replay frame (version byte + Response{
 * seq, history_frame } protobuf). Sized to exceed the largest EU868 payload (242 B
 * at DR4/5) so it holds a full frame at every LoRaWAN data rate and the transmit
 * limit history_frame_cap() = MIN(DR payload, this) is always the DR, never this
 * buffer (#89). Intentionally decoupled from Response.HistoryFrame.samples (440 B
 * for the NFC paged read, #260): the LoRaWAN replay is bounded by the DR (~14
 * records/frame at DR4/5), the NFC pull by its own larger per-tap budget. */
#define APP_CMD_HISTORY_FRAME_BUF_SIZE 256

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
 * set, -EINVAL on a NULL/oversized argument, or -EMSGSIZE if it won't encode.
 * `time_synced` reports whether `t0_unix` is absolute UTC (L-1/L-3). */
int app_cmd_build_history_frame(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				uint32_t t0_unix, uint32_t present, uint32_t interval_s,
				bool time_synced, const uint8_t *samples, size_t samples_len,
				uint8_t *out, size_t out_cap, size_t *out_len);

/* One alarm edge for app_cmd_build_alarm_report(). source/edge/type carry the
 * AlarmEvent_Source/Edge/Type enum values (app_alarm fills these without
 * including the nanopb header). value is the scaled current reading and is only
 * meaningful when has_value is true (discrete sources leave it absent). */
struct app_cmd_alarm_event {
	uint8_t slot;     /* alarm rule slot index (0..APP_ALARM_SLOT_COUNT-1) that fired */
	uint8_t source;   /* enum app_alarm_source (onboard/s1..s4/hall/input/pir/accel) */
	uint8_t quantity; /* enum app_alarm_quantity */
	uint8_t edge;     /* AlarmEvent_Edge: 0=activate, 1=deactivate */
	uint8_t type;     /* AlarmEvent_Type: 0=none, 1=low, 2=high, 3=trigger, 4=no_data (#212) */
	bool has_value;   /* value present */
	int32_t value;    /* scaled value (×100 temp/hum, ×10 pressure, digital 0/1, counter) */
	uint32_t rel_s;   /* seconds since base_time */
};

/* Build an alarm-detail batch (AlarmReport) for fPort 3 (#27) into `out`.
 * `events[0..n_events)` are encoded (capped to the message's 8-event array);
 * `total` is the true window count and may exceed the encoded events when the
 * caller trimmed to fit the data rate. Returns 0 with *out_len set, -EINVAL on
 * a NULL argument, or -EMSGSIZE if it won't fit `out_cap`. `time_synced` reports
 * whether `base_time` is absolute UTC (L-3/L-4). */
int app_cmd_build_alarm_report(uint32_t base_time, uint32_t total, bool time_synced,
			       const struct app_cmd_alarm_event *events, size_t n_events,
			       uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* APP_CMD_H_ */
