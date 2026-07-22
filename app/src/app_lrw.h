/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LRW_H_
#define APP_LRW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum app_lrw_state {
	APP_LRW_STATE_IDLE,
	APP_LRW_STATE_JOINING,
	APP_LRW_STATE_HEALTHY,
	APP_LRW_STATE_WARNING,
	APP_LRW_STATE_RECONNECT,
	APP_LRW_STATE_DISABLED, /* DevEUI all-zero: radio-silent, no join/TX (#98) */
};

struct app_lrw_info {
	enum app_lrw_state state;
	uint32_t dev_addr; /* Device address (from OTAA or ABP) */
	uint32_t fcnt_up;  /* Uplink frame counter */
	int datarate;
	int16_t rssi;
	int8_t snr;
	uint8_t margin;
	uint8_t gw_count;
	/* State machine counters */
	int consecutive_lc_fail;   /* LC failures in a row (HEALTHY) */
	int consecutive_lc_ok;     /* LC successes in a row (WARNING) */
	int warning_lc_fail_total; /* Total LC failures in WARNING */
	int message_count;         /* Messages sent since boot/rejoin */
	/* Thresholds for display */
	int thresh_warning;      /* FAIL_THRESHOLD_WARNING */
	int thresh_healthy;      /* OK_THRESHOLD_HEALTHY */
	int thresh_reconnect;    /* FAIL_THRESHOLD_RECONNECT */
	int link_check_interval; /* Every N-th message has LC */
};

int app_lrw_init(void);
void app_lrw_join(void);
enum app_lrw_state app_lrw_get_state(void);
int app_lrw_get_info(struct app_lrw_info *info);
bool app_lrw_is_ready(void);

/* Compose + split + send a telemetry snapshot (fPort 2) from the current sensor
 * data. app_lrw builds the snapshot (app_compose), splits it into DR-budget
 * frames, piggybacks a LinkCheckReq on the first frame when the N-th-message
 * cadence is due, and retries on a duty-cycle backoff. Triggered by app_report
 * after it samples + captures history. No-op while joining/reconnecting, during
 * calibration or while a history replay owns the radio. */
void app_lrw_send_telemetry(void);

/* Register a callback fired on a link-ready edge (join success / history-replay
 * finish) so app_report can resume the report cadence with an immediate uplink.
 * NULL clears it. Called once from app_report_init(). */
void app_lrw_register_ready_cb(void (*cb)(void));

/* Arm a forced LinkCheckReq on the next telemetry first-frame (shell/test path;
 * pair with app_report_trigger() to actually emit the uplink). */
void app_lrw_force_link_check(void);

/* Current application-payload budget (bytes) for the next uplink, taken from the
 * LoRaWAN stack (lorawan_get_payload_sizes) and refreshed on every DR change and
 * after join. 0 when unknown (before the first join). app_compose() uses this to
 * decide how many telemetry fields fit. */
uint8_t app_lrw_get_max_payload(void);

/* Stage a serialized response (e.g. Response on port 85) for the next
 * uplink. send_work_handler() drains this slot before composing telemetry, so
 * the response leaves at the next jitter window. Single-slot, overwritten with
 * a warning if a prior response hasn't been transmitted yet. */
int app_lrw_queue_response(uint8_t port, const uint8_t *buf, size_t len);

/* Arm a deferred GetInfo uplink to answer a ClockSync command: the next network
 * time-update (DeviceTimeAns) sends an Info carrying the synced unix_time. The
 * command itself does not ack (saves an uplink; a bare ack can't carry the time). */
void app_lrw_send_info_on_clock_sync(void);

/* #320: a LOCAL alarm-config change (shell `alarm` command or NFC SetParam) →
 * push the current alarm settings up on fPort 85 so the backend reconciles its
 * shadow. Coalesces a burst of edits into one paged report emitted on the next
 * jitter window; no-op unless config_report_on_change is set. Any thread. */
void app_lrw_arm_config_report(void);

/* #320 variant for a durable local edit that persists by rebooting (SetParam
 * save=true): persist a one-shot marker so the report is emitted once after the
 * re-join rather than lost in the reboot. No-op unless config_report_on_change. */
void app_lrw_arm_config_report_after_reboot(void);

/* Stage an alarm-detail batch (issue #27) for the next uplink on fPort 3. Own
 * slot, drained after the command response and before telemetry, so it never
 * collides with app_lrw_queue_response(). Returns 0, -EINVAL, or -EMSGSIZE. */
int app_lrw_send_alarm(const uint8_t *buf, size_t len);

/* Start a device-driven history replay (issue #52): stream every stored record
 * in [from_unix, to_unix] back as N HistoryFrame uplinks on the command port,
 * back-to-back ASAP (duty-cycle permitting), echoing `seq`. Returns true when a
 * replay was armed (the first frame is the reply, so the caller should NOT also
 * send an Ack), false if the link isn't ready or the window is empty. */
bool app_lrw_start_history_replay(uint32_t from_unix, uint32_t to_unix, uint32_t seq);

/* Erase the persisted LoRaWAN NVM context (frame counters, DevNonce, session).
 * Used when re-provisioning credentials so a new ABP/OTAA identity starts from
 * a clean state. The caller must reboot afterwards for the MAC to re-init from
 * the cleared NVM. Returns 0 on success or a negative errno. */
int app_lrw_reset_nvm(void);

/* Stop LoRaWAN activity (all TX/link-check/rejoin timers) ahead of a deep-sleep
 * poweroff, so nothing re-arms the radio before the MCU shuts down. Does not
 * deinit the stack — wake from deep sleep is a clean boot. */
void app_lrw_suspend(void);

#if defined(CONFIG_SHELL)
/* Debug/test only: inject a synthetic link-check outcome (ok=true success,
 * false failure) onto the LRW work queue, to drive the state-machine
 * transitions (HEALTHY->WARNING->RECONNECT->rejoin and the late-LC-in-RECONNECT
 * guard, #71) deterministically from the shell without a real RF outage. */
void app_lrw_debug_inject_lc(bool ok);
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_LRW_H_ */
