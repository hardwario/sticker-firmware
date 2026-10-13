/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CLOCK_H_
#define APP_CLOCK_H_

/* Standard includes */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the wall-clock subsystem (binds the RTC device). Returns 0 on
 * success, -ENODEV if the RTC is not ready. */
int app_clock_init(void);

/* Read the current wall-clock time as a Unix timestamp (seconds, UTC).
 * Returns 0 on success, -ENODATA if the RTC has not been set yet, -EINVAL on
 * a NULL argument, or another negative errno from the RTC driver. */
int app_clock_get_unix(uint32_t *unix_s);

/* Set the RTC from a Unix timestamp (seconds, UTC). Mainly for testing /
 * manual provisioning; the normal path is app_clock_set_network_time(). Returns
 * 0 on success or a negative errno from the RTC driver. */
int app_clock_set_unix(uint32_t unix_s);

/* Apply a wall-clock time received from the network: the LoRaWAN DeviceTimeAns
 * or the P2P Ack time tail (the radio backends; asking for one is
 * app_radio_time_request()). Rejects a value outside the plausible window
 * (2024-01-01 .. 2100-01-01, L-5) instead of skewing every history/alarm
 * timestamp. The first one arms the weekly re-sync (#96). Returns 0, -ERANGE
 * for an implausible time, or the app_clock_set_unix() error. */
int app_clock_set_network_time(uint32_t unix_s);

/* Uptime (ms) at which the last network time was applied by
 * app_clock_set_network_time(), or 0 if none has been since boot. */
int64_t app_clock_network_time_at_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CLOCK_H_ */
