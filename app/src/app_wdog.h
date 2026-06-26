/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_WDOG_H_
#define APP_WDOG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_wdog_init(void);

/* Feed the hardware IWDG *iff* every registered liveness channel has been pinged
 * within its timeout. Returns 0 when fed, -EBUSY when the feed was withheld
 * because a worker is wedged (the SoC is then reset by the IWDG ~10 s later — the
 * recovery for #181/#182, a permanently blocked m_work_q), or another negative
 * errno. Called from the main loop. With no channels registered it always feeds,
 * preserving the original unconditional behaviour during boot. */
int app_wdog_feed(void);

/* Register a worker thread / work queue for liveness monitoring. The worker must
 * call app_wdog_ping() at least every `timeout_ms`, otherwise app_wdog_feed()
 * stops feeding the IWDG and the SoC resets. Returns a non-negative channel id
 * for app_wdog_ping(), or a negative errno when the channel table is full. The
 * channel is seeded fresh, so a worker that registers and only later starts
 * pinging still gets one full timeout of grace. */
int app_wdog_register(uint32_t timeout_ms);

/* Prove a registered worker is still making progress (resets its liveness
 * deadline). Lock-free and cheap; safe from any thread or work handler. A bad
 * channel id is ignored. */
void app_wdog_ping(int channel);

#ifdef __cplusplus
}
#endif

#endif /* APP_WDOG_H_ */
