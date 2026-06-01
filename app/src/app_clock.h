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

/* Ask the LoRaWAN network for the current time (DeviceTimeReq MAC command).
 * The answer arrives asynchronously on the next downlink; feed it to
 * app_clock_handle_downlink(). No-op when CONFIG_LORAWAN is disabled. */
void app_clock_request_sync(void);

/* Force a re-sync: clear the synced guard and request DeviceTimeReq again.
 * Used by the ClockSync downlink command. No-op when CONFIG_LORAWAN is off. */
void app_clock_force_resync(void);

/* Inspect a downlink's flags; if LORAWAN_TIME_UPDATED is set, read the network
 * time and set the RTC from it. Call from the LoRaWAN downlink callback.
 * No-op when CONFIG_LORAWAN is disabled. */
void app_clock_handle_downlink(uint8_t flags);

/* Read the current wall-clock time as a Unix timestamp (seconds, UTC).
 * Returns 0 on success, -ENODATA if the RTC has not been set yet, -EINVAL on
 * a NULL argument, or another negative errno from the RTC driver. */
int app_clock_get_unix(uint32_t *unix_s);

/* Set the RTC from a Unix timestamp (seconds, UTC). Mainly for testing /
 * manual provisioning; the normal path is app_clock_handle_downlink(). Returns
 * 0 on success or a negative errno from the RTC driver. */
int app_clock_set_unix(uint32_t unix_s);

#ifdef __cplusplus
}
#endif

#endif /* APP_CLOCK_H_ */
