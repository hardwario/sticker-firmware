/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SHT40_H_
#define APP_SHT40_H_

#ifdef __cplusplus
extern "C" {
#endif

int app_sht40_read(float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif

#endif /* APP_SHT40_H_ */
