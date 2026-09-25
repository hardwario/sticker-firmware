/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SLOT_H_
#define APP_SLOT_H_

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Report-cadence slot grid (F27). The periodic report (sample + history capture
 * + telemetry) runs on slots `anchor + k * interval` of the wall clock instead of
 * a timer re-armed `interval` after the previous run, so a kernel clock that runs
 * off the RTC (debug SysTick on the MSI), work-queue latency or a late run never
 * accumulate: every run re-reads the clock and aims at the next slot.
 *
 * Times are whole seconds in one clock domain: unix time once the RTC is set
 * (`synced`), else kernel uptime. The grid phase is kept when the clock switches
 * from uptime to unix (first RTC sync): the anchor is carried over by the
 * (unix - uptime) offset, so the cadence does not jump. Pure logic, no kernel
 * calls — the caller passes the clock readings (unit-tested in tests/slot). */
struct app_slot {
	uint32_t anchor;   /* a slot time on the grid (domain: `synced`) */
	uint32_t interval; /* seconds between slots the grid was laid for */
	bool synced;       /* anchor domain: true = unix (RTC), false = uptime */
	bool valid;        /* false = no grid yet; the next call lays one at `now` */
};

/* Grid point `anchor + k * interval` nearest to `t` (k may be negative; ties
 * round up). interval 0 is treated as 1. */
uint32_t app_slot_round(uint32_t anchor, uint32_t interval, uint32_t t);

/* Keep (or lay) the grid and compute the delay to the next report.
 *
 * `now` is the current time in the domain `synced` says (unix when the RTC is
 * set, else uptime seconds); `uptime_now` is the current uptime in seconds, used
 * only to carry the grid over the uptime -> unix switch. A fresh grid (first
 * call, or `interval` changed) is anchored at `now`; an RTC that became unset
 * again (unix -> uptime) also lays a fresh grid.
 *
 * `periodic` = called from a cadence run: the run belongs to the grid slot
 * nearest to `now` (a timer that fired a little early or late still maps to the
 * slot it was armed for) and the next report is that slot + interval. Otherwise
 * (arming at boot) the next report is the first grid slot after `now`.
 *
 * *slot_out = the slot of the current run (periodic) or the grid point nearest
 * to `now`. Returns the delay in seconds until the next slot, >= 1. */
uint32_t app_slot_next(struct app_slot *s, uint32_t interval, uint32_t now, bool synced,
		       uint32_t uptime_now, bool periodic, uint32_t *slot_out);

#ifdef __cplusplus
}
#endif

#endif /* APP_SLOT_H_ */
