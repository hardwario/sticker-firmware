/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_REPORT_H_
#define APP_REPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Report orchestration (#126).
 *
 * app_report decides *when* to measure and send: it owns the interval_report
 * cadence, the lazy sample trigger (interval_sample == 0) and the history
 * capture, then triggers the transport via app_lrw_send_telemetry(). It runs on
 * its own work queue so the sensor read and the history flash write never sit on
 * the LoRaWAN TX work queue (m_work_q) or the system work queue (which also
 * drives LoRaMacProcess()).
 *
 * Layering: app_report = *when* to measure & send; app_lrw = *how* it reaches the
 * network (it composes the snapshot via app_compose, splits it into DR-budget
 * frames, and handles the LinkCheckReq piggyback / duty-cycle retry). app_report
 * only reads app_lrw (app_lrw_get_state / app_lrw_is_ready); app_lrw kicks
 * app_report on a link-ready edge (join success / history-replay finish) via the
 * callback it registers in app_report_init(). */

/* Initialize the orchestrator: create the work queue, init the timer/works and
 * register the link-ready kick with app_lrw. Does NOT start the cadence — that
 * begins on the first link-ready kick from app_lrw (join success). Returns 0 on
 * success or a negative errno. */
int app_report_init(void);

/* Force an immediate report cycle (force_send command, `send` shell, alarm
 * uplink). Sample-if-lazy + capture history + trigger app_lrw_send_telemetry(),
 * exactly like a timer-driven cycle. The cycle self-skips if the link isn't
 * ready, and the periodic timer keeps ticking. */
void app_report_trigger(void);

/* Stop the report cadence before a deep-sleep/poweroff so the timer can't fire
 * and re-arm the radio on the way down. Called from app_power_suspend(). */
void app_report_suspend(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_REPORT_H_ */
