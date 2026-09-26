/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_lrw_stale.h"

/* Standard includes */
#include <errno.h>
#include <stdint.h>

void app_lrw_stale_note_send(struct app_lrw_stale_dc *dc, int ret, int64_t now_ms)
{
	if (ret == 0) {
		dc->since_ms = 0;
		dc->last_ms = 0;
	} else if (ret == -ECONNREFUSED) {
		if (dc->since_ms == 0) {
			dc->since_ms = now_ms;
		}
		dc->last_ms = now_ms;
	}
}

enum app_lrw_stale_verdict app_lrw_stale_check(int64_t now_ms, int64_t last_uplink_ms,
					       const struct app_lrw_stale_dc *dc,
					       uint32_t interval_s)
{
	if (last_uplink_ms == 0 || interval_s == 0) {
		return APP_LRW_STALE_OK;
	}

	int64_t interval_ms = (int64_t)interval_s * 1000;

	if (now_ms - last_uplink_ms <= interval_ms * APP_LRW_STALE_FACTOR) {
		return APP_LRW_STALE_OK;
	}

	/* Stale. Hold only while refusals keep coming (the MAC is alive and
	 * throttled) and the streak is no longer than the duty-cycle window. */
	if (dc->since_ms != 0 && dc->last_ms != 0 &&
	    now_ms - dc->last_ms <= interval_ms + APP_LRW_STALE_DC_RECENT_MARGIN_MS &&
	    now_ms - dc->since_ms < APP_LRW_STALE_DC_HOLD_MAX_MS) {
		return APP_LRW_STALE_HOLD_DC;
	}

	return APP_LRW_STALE_REJOIN;
}
