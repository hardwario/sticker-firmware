/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SENSOR_READ_H_
#define APP_SENSOR_READ_H_

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

/* Standard includes */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fetch one sample from a Zephyr sensor device and read `n` channels into `out`
 * as floats. Shared fetch/channel-get/convert boilerplate for the onboard I2C
 * sensors (SHT4x, OPT3001, MPL3115A2) so each driver only keeps its own
 * channel list, logging and range checks (#220.C). Returns 0, -ENODEV if the
 * device is not ready, or the first failing sensor API error. */
int app_sensor_read_channels(const struct device *dev, const enum sensor_channel *chans, float *out,
			     size_t n);

#ifdef __cplusplus
}
#endif

#endif /* APP_SENSOR_READ_H_ */
