/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_DS18B20_H_
#define APP_DS18B20_H_

/* Standard includes */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_ds18b20_scan(void);
int app_ds18b20_get_count(void);
int app_ds18b20_read(int index, uint64_t *serial_number, float *temperature);

#ifdef __cplusplus
}
#endif

#endif /* APP_DS18B20_H_ */
