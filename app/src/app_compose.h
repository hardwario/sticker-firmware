/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_COMPOSE_H_
#define APP_COMPOSE_H_

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compose one telemetry frame into `buf` (max `size`, also bounded by the
 * LoRaWAN payload budget). On the first call of a report a consistent snapshot
 * of all sensor groups is taken; each call emits the highest-priority pending
 * groups that fit (whole groups only). *len = encoded length, *more = true when
 * groups remain for a follow-up frame of the same snapshot (send ASAP).
 *
 * Returns 0 on success, -EAGAIN when the payload budget is unknown (pre-join),
 * or -EMSGSIZE on encode failure. */
int app_compose(uint8_t *buf, size_t size, size_t *len, bool *more);

/* As app_compose(), but with an explicit payload budget instead of the live
 * LoRaWAN one. Lets a test/debug path (e.g. `ats lrw compose`) build the exact
 * uplink bytes for a chosen data rate without a network join. budget == 0 still
 * returns -EAGAIN. #340 M16: unlike app_compose(), this never consumes the
 * one-shot post-boot marker (SYSTEM_FLAG_BOOT) — a debug probe run before the
 * real first uplink must not steal it. */
int app_compose_ex(uint8_t *buf, size_t size, size_t *len, bool *more, uint8_t budget);

/* Take a fresh, full reading of every sensor group into `*out` (a complete
 * Telemetry, not budget-split). For a synchronous response such as the Sample
 * command over NFC, where the whole message fits in one frame. Independent of
 * the app_compose() multi-frame state, so it can run off m_work_q. */
struct _Telemetry; /* fwd: real type is the nanopb Telemetry */
void app_compose_snapshot(struct _Telemetry *out);

/* Discard any in-progress snapshot so the next app_compose() starts fresh.
 * Call from the join path: a rejoin must not continue a pre-outage snapshot
 * with stale data and no indication (#93.5). */
void app_compose_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_COMPOSE_H_ */
