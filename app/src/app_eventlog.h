/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_EVENTLOG_H_
#define APP_EVENTLOG_H_

#include "app_cmd.h"

/* Standard includes */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LoRaWAN fPort of the event log batches (AlarmReport, version-prefixed). */
#define APP_EVENTLOG_PORT 4

/* Event log: every alarm edge (when event_log_enable) is stored in a flash
 * ring (log_split % of history_partition, kept after upload for req_events)
 * and uploaded on fPort 4 after event_flush_count
 * new events or every event_flush_interval seconds, whichever comes first. LoRaWAN only. */

/* Record one alarm edge that happened at uptime `at_ms` (0 = now); rel_s is
 * ignored, the log stamps its own time. Any thread. */
void app_eventlog_add(const struct app_cmd_alarm_event *ev, int64_t at_ms);

/* Mount the flash ring (log_split % of history_partition). A changed
 * log_split erases the whole partition first, so call it before
 * app_history_init(). -ENOSPC: log_split leaves the event log no flash. */
int app_eventlog_init(void);

/* History_partition pages given to the event log by log_split. */
uint16_t app_eventlog_flash_pages(void);

/* Clock synced: this boot's events stamped on uptime become unix time. */
void app_eventlog_on_clock_sync(uint32_t unix_now);

/* Upload the pending events now (shell / tests). Any thread. */
void app_eventlog_flush(void);

/* req_events: re-send the retained events with time in [from_unix, to_unix]
 * as fPort-4 batches (a batch already streaming finishes first). -ENODATA when
 * the log is empty, -ENOTSUP outside LoRaWAN. Any thread. */
int app_eventlog_request(uint32_t from_unix, uint32_t to_unix);

/* Events waiting for upload, and events dropped by a ring overflow since boot. */
uint32_t app_eventlog_pending(void);
uint32_t app_eventlog_lost(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_EVENTLOG_H_ */
