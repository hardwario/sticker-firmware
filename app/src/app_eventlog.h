/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_EVENTLOG_H_
#define APP_EVENTLOG_H_

#include "app_cmd.h"

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
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

/* req_events by seq: `n_pairs` (seq_from, count) ranges (max 8), or `n` single
 * seqs (absolute, max 20). A selection with no retained event is answered by
 * an empty replay page carrying oldest_seq. -EINVAL on a bad count. */
int app_eventlog_request_ranges(const uint32_t *pairs, size_t n_pairs);
int app_eventlog_request_seqs(const uint32_t *seqs, size_t n);

/* One NFC page of the event log (app_eventlog_export()). */
struct app_eventlog_page {
	uint32_t base_time; /* events' rel_s are from here */
	uint32_t seq_first; /* seq of the first event (or next_seq when none) */
	uint32_t oldest;    /* oldest retained seq */
	uint32_t next_seq;  /* cursor for the next page */
	bool synced;        /* base_time is unix */
	bool more;          /* events at/after next_seq remain */
};

/* Copy up to `max` retained events with seq >= start_seq (time in [from, to]
 * when synced) into `ev` for a client-driven read (NFC). Events carry rel_s
 * from page->base_time and their own seq where the +1 run breaks. A page never
 * mixes synced and unsynced times. Returns the count or -ENODATA. */
int app_eventlog_export(uint32_t start_seq, uint32_t from_unix, uint32_t to_unix,
			struct app_cmd_alarm_event *ev, size_t max, struct app_eventlog_page *page);

/* Events waiting for upload, and events dropped by a ring overflow since boot. */
uint32_t app_eventlog_pending(void);
uint32_t app_eventlog_lost(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_EVENTLOG_H_ */
