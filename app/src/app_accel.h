/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ACCEL_H_
#define APP_ACCEL_H_

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

int app_accel_read(float *accel_x, float *accel_y, float *accel_z, int *orientation);

/* Motion (any-motion) detection via the LIS2DH hardware interrupt. */
typedef void (*app_accel_motion_cb_t)(void *user_data);

/* Register the motion callback and arm the interrupt at the configured
 * sensitivity (g_app_config.accel_motion_sensitivity). Call once at init. */
int app_accel_init_motion(app_accel_motion_cb_t cb, void *user_data);

/* Apply a sensitivity level at runtime. OFF disables the interrupt (power
 * saving); LOW/MEDIUM/HIGH set the slope threshold + duration and (re)arm it. */
int app_accel_set_motion_sensitivity(enum app_config_motion_sensitivity level);

#ifdef __cplusplus
}
#endif

#endif /* APP_ACCEL_H_ */
