/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MPL3115A2_H_
#define APP_MPL3115A2_H_

#ifdef __cplusplus
extern "C" {
#endif

int app_mpl3115a2_read(float *altitude, float *pressure, float *temperature);

#ifdef __cplusplus
}
#endif

#endif /* APP_MPL3115A2_H_ */
