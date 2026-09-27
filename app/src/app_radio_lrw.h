/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_RADIO_LRW_H_
#define APP_RADIO_LRW_H_

#include "app_radio.h" /* enum app_radio_state */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* struct k_work (app_radio_lrw_run_on_work_q()'s parameter) - included here rather
 * than relying on callers to have pulled in kernel.h before this header;
 * app_radio_lrw_run_on_work_q() used to be CONFIG_SHELL-gated, so no non-Zephyr
 * caller had ever included this header first before (#340 M22). */
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

struct app_radio_lrw_info {
	enum app_radio_state state;
	uint32_t dev_addr; /* Device address (from OTAA or ABP) */
	uint32_t fcnt_up;  /* Uplink frame counter */
	int datarate;
	int tx_power; /* LoRaMac TX power index: 0 = max EIRP, higher = weaker */
	int16_t rssi;
	int8_t snr;
	uint8_t margin;
	uint8_t gw_count;
	/* Link supervision (app_radio's, struct app_radio_link) */
	int consecutive_lc_fail;   /* LC failures in a row */
	int warning_lc_fail_total; /* LC failures in WARNING towards the rejoin */
	int message_count;         /* Reports sent since the join */
	/* Thresholds for display */
	int thresh_warning;      /* APP_RADIO_LINK_WARNING_THRESHOLD */
	int thresh_reconnect;    /* radio-link-check-fail-rejoin */
	int link_check_interval; /* Every N-th report has LC */
};

int app_radio_lrw_init(void);
void app_radio_lrw_join(void);
enum app_radio_state app_radio_lrw_get_state(void);
int app_radio_lrw_get_info(struct app_radio_lrw_info *info);
bool app_radio_lrw_is_ready(void);

/* The LoRaWAN TX backend of the common scheduler (app_radio, doc/plan/460 F4):
 * sends one frame against the live DR budget -- telemetry on fPort 2, alarms on
 * fPort 3, answers on their port (85) -- with the LinkCheckReq piggyback and
 * the empty MAC-flush uplink at budget 0. All unconfirmed. */
extern const struct app_radio_backend app_radio_lrw_backend;

/* Register a callback fired on a link-ready edge (join success) so app_report
 * can resume the report cadence with an immediate uplink. NULL clears it.
 * Called once from app_report_init(). */
void app_radio_lrw_register_ready_cb(void (*cb)(void));

/* Current application-payload budget (bytes) for the next uplink, taken from the
 * LoRaWAN stack (lorawan_get_payload_sizes) and refreshed on every DR change and
 * after join. 0 when unknown (before the first join). app_compose() uses this to
 * decide how many telemetry fields fit. */
uint8_t app_radio_lrw_get_max_payload(void);

/* Arm a deferred GetInfo uplink to answer a ClockSync command: the next network
 * time-update (DeviceTimeAns) sends an Info carrying the synced unix_time and the
 * command's `seq`, so the host can pair it with the request. The command itself
 * does not ack (saves an uplink; a bare ack can't carry the time). A newer
 * ClockSync before the time lands takes over the seq. */
void app_radio_lrw_send_info_on_clock_sync(uint32_t seq);

/* app_radio_clock_sync() on LoRaWAN: force a DeviceTimeReq and answer with the
 * seq-carrying Info once the time lands. */
void app_radio_lrw_clock_sync(uint32_t seq);

/* Erase the persisted LoRaWAN NVM context (frame counters, DevNonce, session).
 * Used when re-provisioning credentials so a new ABP/OTAA identity starts from
 * a clean state. The caller must reboot afterwards for the MAC to re-init from
 * the cleared NVM. Returns 0 on success or a negative errno. */
int app_radio_lrw_reset_nvm(void);

/* Stop LoRaWAN activity (all TX/link-check/rejoin timers) ahead of a deep-sleep
 * poweroff, so nothing re-arms the radio before the MCU shuts down. Does not
 * deinit the stack — wake from deep sleep is a clean boot. */
void app_radio_lrw_suspend(void);

#if defined(CONFIG_SHELL)
/* Debug/test only: inject a synthetic link-check outcome (ok=true success,
 * false failure) onto the LRW work queue, to drive the state-machine
 * transitions (HEALTHY->WARNING->RECONNECT->rejoin and the late-LC-in-RECONNECT
 * guard, #71) deterministically from the shell without a real RF outage. */
void app_radio_lrw_debug_inject_lc(bool ok);
#endif

/* Submit an arbitrary work item onto m_work_q. Lets a caller (a shell command
 * like `ats radio compose`, or calibration mode's own send path, #340 M22) run
 * logic that app_compose.c documents as "solely on m_work_q" without racing
 * the real telemetry TX path and without standing up a second queue - the
 * caller submits its own struct k_work and (if it needs to wait)
 * k_work_flush()es it. Returns k_work_submit_to_queue()'s result: >=0
 * queued/running, a negative errno if the queue rejected it (in which case
 * the caller must not flush — nothing was submitted). */
int app_radio_lrw_run_on_work_q(struct k_work *work);

#ifdef __cplusplus
}
#endif

#endif /* APP_RADIO_LRW_H_ */
