/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_COUNTERS_H_
#define APP_COUNTERS_H_

/* Standard includes */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Persist the hall/input pulse totalizers across reboots and power loss.
 *
 * The live counters live in RAM (app_hall / app_input). This module mirrors
 * them to NVS (storage partition, Settings API) so a reset, FUOTA, battery swap
 * or brownout does not silently zero a metering total.
 *
 * Save cadence (all independent of the LoRaWAN join state, so counters persist
 * even on a device that never joins):
 *   - interval_sample > 0: backed up on every sensor sample (app_sensor), so the
 *     worst-case lost-pulse window is one interval_sample — typically far tighter
 *     than the report/send cadence (e.g. back up every 5 min, send every 60 min).
 *   - interval_sample == 0: backed up at the report cadence (app_report), so the
 *     worst-case window is one interval_report.
 * Both paths are dirty-flagged (no flash write when nothing changed).
 */

/* Register the settings handler, load the persisted totals and seed them back
 * into app_hall / app_input. Must run AFTER app_hall_init / app_input_init
 * (i.e. after app_sensor_init) so the seed is not clobbered by module init. */
int app_counters_init(void);

/* Persist the current totals if they changed since the last save (dirty flag).
 * Pass force=true to write unconditionally (e.g. right after ResetCounters, so
 * the cleared value cannot be resurrected by a reboot). */
int app_counters_save(bool force);

#ifdef __cplusplus
}
#endif

#endif /* APP_COUNTERS_H_ */
