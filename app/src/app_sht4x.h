/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SHT4X_H_
#define APP_SHT4X_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_sht4x_read(float *temperature, float *humidity);
int app_sht4x_read_serial(uint32_t *serial_number);

#ifdef __cplusplus
}
#endif

#endif /* APP_SHT4X_H_ */
