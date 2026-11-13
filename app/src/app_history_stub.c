/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* History disabled (CONFIG_APP_HISTORY=n, provisioning image): nothing is
 * stored, every query reports an empty buffer. */

#include "app_history.h"

#include <errno.h>
#include <string.h>

int app_history_init(void)
{
	return 0;
}

void app_history_capture_at(uint32_t slot, bool synced)
{
	(void)slot;
	(void)synced;
}

void app_history_capture(void)
{
}

void app_history_set_replay_active(bool active)
{
	(void)active;
}

void app_history_on_clock_sync(uint32_t unix_now)
{
	(void)unix_now;
}

void app_history_set_work_queue(struct k_work_q *queue)
{
	(void)queue;
}

size_t app_history_count(void)
{
	return 0;
}

size_t app_history_capacity(void)
{
	return 0;
}

void app_history_clear(void)
{
}

int app_history_get(size_t idx, struct app_history_record *out)
{
	(void)idx;
	(void)out;
	return -ENODATA;
}

bool app_history_is_ready(void)
{
	return false;
}

void app_history_set_enabled(bool enable)
{
	(void)enable;
}

void app_history_get_layout(struct app_history_layout *out)
{
	memset(out, 0, sizeof(*out));
}

void app_history_set_channels(const uint8_t *list, size_t n)
{
	(void)list;
	(void)n;
}

void app_history_get_channels(uint8_t list[APP_HISTORY_MAX_CH])
{
	memset(list, APP_HISTORY_ENTRY_UNUSED, APP_HISTORY_MAX_CH);
}

uint32_t app_history_get_interval(void)
{
	return 0;
}

size_t app_history_export_page(uint32_t from_unix, uint32_t to_unix, size_t start_ord, uint8_t *buf,
			       size_t cap, uint32_t *t0_out, bool *synced_out, uint16_t *n_written,
			       size_t *next_ord)
{
	(void)from_unix;
	(void)to_unix;
	(void)buf;
	(void)cap;
	if (t0_out) {
		*t0_out = 0;
	}
	if (synced_out) {
		*synced_out = false;
	}
	if (n_written) {
		*n_written = 0;
	}
	if (next_ord) {
		*next_ord = start_ord;
	}
	return 0;
}

void app_history_span(uint32_t *first_abs, uint32_t *end_abs)
{
	if (first_abs) {
		*first_abs = 0;
	}
	if (end_abs) {
		*end_abs = 0;
	}
}

size_t app_history_export_abs(uint32_t from_unix, uint32_t to_unix, uint32_t start_abs,
			      uint32_t end_abs, uint8_t *buf, size_t cap, uint32_t *t0_out,
			      bool *synced_out, uint16_t *n_written, uint32_t *next_abs)
{
	(void)from_unix;
	(void)to_unix;
	(void)end_abs;
	(void)buf;
	(void)cap;
	if (t0_out) {
		*t0_out = 0;
	}
	if (synced_out) {
		*synced_out = false;
	}
	if (n_written) {
		*n_written = 0;
	}
	if (next_abs) {
		*next_abs = start_abs;
	}
	return 0;
}

uint16_t app_history_count_frames(uint32_t from_unix, uint32_t to_unix, size_t cap)
{
	(void)from_unix;
	(void)to_unix;
	(void)cap;
	return 0;
}

int app_history_entry_name(uint8_t entry, char *buf, size_t cap)
{
	(void)entry;
	(void)buf;
	(void)cap;
	return -ENOTSUP;
}

int app_history_entry_by_name(const char *name, uint8_t *entry)
{
	(void)name;
	(void)entry;
	return -ENOTSUP;
}

bool app_history_entry_available(uint8_t entry)
{
	(void)entry;
	return false;
}
