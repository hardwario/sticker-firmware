/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_slot.h"

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

/* Floor division for a positive divisor (C `/` truncates toward zero). */
static int32_t floor_div(int32_t a, int32_t b)
{
	int32_t q = a / b;

	if ((a % b) != 0 && a < 0) {
		q--;
	}
	return q;
}

uint32_t app_slot_round(uint32_t anchor, uint32_t interval, uint32_t t)
{
	if (interval == 0) {
		interval = 1;
	}

	/* Signed distance from the anchor: both are seconds of one clock domain, so
	 * |t - anchor| stays far below 2^31 (68 years). */
	int32_t d = (int32_t)(t - anchor);
	int32_t half = (int32_t)(interval / 2);
	int32_t k;

	if (d > INT32_MAX - half) {
		k = floor_div(d, (int32_t)interval) + 1; /* d + half would overflow */
	} else {
		k = floor_div(d + half, (int32_t)interval);
	}
	return anchor + (uint32_t)k * interval;
}

uint32_t app_slot_next(struct app_slot *s, uint32_t interval, uint32_t now, bool synced,
		       uint32_t uptime_now, bool periodic, uint32_t *slot_out)
{
	if (interval == 0) {
		interval = 1;
	}

	if (!s->valid || s->interval != interval) {
		/* No grid yet, or a new interval_report: start a fresh grid here. */
		s->valid = true;
		s->interval = interval;
		s->synced = synced;
		s->anchor = now;
	} else if (s->synced != synced) {
		if (synced) {
			/* First RTC sync: re-express the anchor in unix time with the
			 * current (unix - uptime) offset, so the phase survives. */
			s->anchor += now - uptime_now;
		} else {
			/* RTC unset again (read error): no offset to carry over. */
			s->anchor = now;
		}
		s->synced = synced;
	}

	uint32_t cur = app_slot_round(s->anchor, interval, now);
	uint32_t next;

	if (periodic) {
		uint32_t tol =
			(interval / 2 < APP_SLOT_TOLERANCE_S) ? interval / 2 : APP_SLOT_TOLERANCE_S;
		int32_t off = (int32_t)(now - cur);

		if (off > (int32_t)tol || off < -(int32_t)tol) {
			/* Far off every slot (a halt or stall longer than the timer's
			 * remaining time, an RTC step): labelling this run with the
			 * nearest slot would stamp its record up to interval / 2 away
			 * from when it was sampled. Re-lay the grid at now instead, so
			 * the record keeps its true time (history opens a new segment
			 * for the off-grid slot). */
			s->anchor = now;
			cur = now;
		}
		/* This run is slot `cur`. */
		next = cur + interval;
	} else {
		/* Arm from outside the cadence: first slot strictly after now. */
		next = ((int32_t)(cur - now) > 0) ? cur : cur + interval;
	}

	if (slot_out) {
		*slot_out = cur;
	}

	int32_t delay = (int32_t)(next - now);

	return (delay > 0) ? (uint32_t)delay : 1U;
}
