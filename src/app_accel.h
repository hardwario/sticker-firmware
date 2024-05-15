/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_ACCEL_H_
#define APP_ACCEL_H_

#ifdef __cplusplus
extern "C" {
#endif

int app_accel_read(float *accel_x, float *accel_y, float *accel_z, int *orientation);

#ifdef __cplusplus
}
#endif

#endif /* APP_ACCEL_H_ */
