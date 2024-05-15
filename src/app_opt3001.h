/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_OPT3001_H_
#define APP_OPT3001_H_

/* Standard includes */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_opt3001_read(float *illuminance);

#ifdef __cplusplus
}
#endif

#endif /* APP_OPT3001_H_ */
