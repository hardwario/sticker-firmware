/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LRW_STALE_H_
#define APP_LRW_STALE_H_

/* Standard includes */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M-2 stale-uplink watchdog decision (pure logic, unit-tested in tests/lrw_stale).
 *
 * The watchdog forces a MAC-reset rejoin when joined but no telemetry uplink has
 * left for APP_LRW_STALE_FACTOR x interval_report: sends perpetually skipped
 * (budget == 0 loop, retries exhausted) leave m_work_q live and the IWDG fed
 * while the station is mute.
 *
 * A send refused by the EU868 duty cycle (lorawan_send() -> -ECONNREFUSED,
 * LORAMAC_STATUS_DUTYCYCLE_RESTRICTED) is not a mute station: the MAC is alive
 * and throttled, and the band credits come back when the 1 h observation window
 * rolls over. A rejoin there only re-initialises the MAC, which resets the band
 * credits kept in RAM — the device would bypass the 1 % limit (F29, HIL
 * 2026-09-25: DR0 at 60 s, two forced rejoins in 91 min). So the watchdog holds
 * while refusals keep coming, bounded by APP_LRW_STALE_DC_HOLD_MAX_MS so a MAC
 * stuck in "restricted" still ends in a rejoin. */

/* Report intervals without a telemetry uplink before M-2 forces a rejoin. */
#define APP_LRW_STALE_FACTOR 4

/* Longest duty-cycle streak M-2 waits out: the 1 h observation window of the
 * LoRaMac band credits plus a margin. */
#define APP_LRW_STALE_DC_HOLD_MAX_MS (75LL * 60 * 1000)

/* A refusal counts as "recent" within one report interval plus this margin (the
 * telemetry retry chain after a report is 8 x 15 s). */
#define APP_LRW_STALE_DC_RECENT_MARGIN_MS (3LL * 60 * 1000)

enum app_lrw_stale_verdict {
	APP_LRW_STALE_OK = 0,  /* an uplink left recently enough (or no clock) */
	APP_LRW_STALE_HOLD_DC, /* stale, but the duty cycle explains it: wait */
	APP_LRW_STALE_REJOIN,  /* stale with no duty-cycle excuse: force rejoin */
};

/* Duty-cycle refusal streak: first and most recent refusal (uptime ms, 0 =
 * none). Cleared by a successful send. */
struct app_lrw_stale_dc {
	int64_t since_ms;
	int64_t last_ms;
};

/* Record a lorawan_send() result: 0 clears the streak, -ECONNREFUSED (duty
 * cycle) extends it, anything else leaves it unchanged. */
void app_lrw_stale_note_send(struct app_lrw_stale_dc *dc, int ret, int64_t now_ms);

/* Decide the watchdog action. `last_uplink_ms` = uptime of the last successful
 * telemetry uplink (0 = none since the last (re)join: no decision), `interval_s`
 * = interval_report (0 = no cadence: no decision). */
enum app_lrw_stale_verdict app_lrw_stale_check(int64_t now_ms, int64_t last_uplink_ms,
					       const struct app_lrw_stale_dc *dc,
					       uint32_t interval_s);

#ifdef __cplusplus
}
#endif

#endif /* APP_LRW_STALE_H_ */
