/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Event log without flash history (debug / provisioning images): the log
 * lives in history_partition, which these images do not use, so it is off and
 * costs no flash. */

#include "app_eventlog.h"

#include <errno.h>

void app_eventlog_add(const struct app_cmd_alarm_event *ev, int64_t at_ms)
{
	(void)ev;
	(void)at_ms;
}

int app_eventlog_init(void)
{
	return -ENOTSUP;
}

uint16_t app_eventlog_flash_pages(void)
{
	return 0;
}

void app_eventlog_on_clock_sync(uint32_t unix_now)
{
	(void)unix_now;
}

void app_eventlog_flush(void)
{
}

int app_eventlog_request(uint32_t from_unix, uint32_t to_unix)
{
	(void)from_unix;
	(void)to_unix;
	return -ENODATA;
}

int app_eventlog_request_ranges(const uint32_t *pairs, size_t n_pairs)
{
	(void)pairs;
	(void)n_pairs;
	return -ENODATA;
}

int app_eventlog_request_seqs(const uint32_t *seqs, size_t n)
{
	(void)seqs;
	(void)n;
	return -ENODATA;
}

void app_eventlog_frame_dropped(const uint8_t *buf, size_t len)
{
	(void)buf;
	(void)len;
}

int app_eventlog_export(uint32_t start_seq, uint32_t from_unix, uint32_t to_unix,
			struct app_cmd_alarm_event *ev, size_t max, struct app_eventlog_page *page)
{
	(void)start_seq;
	(void)from_unix;
	(void)to_unix;
	(void)ev;
	(void)max;
	(void)page;
	return -ENODATA;
}

uint32_t app_eventlog_pending(void)
{
	return 0;
}

uint32_t app_eventlog_lost(void)
{
	return 0;
}
