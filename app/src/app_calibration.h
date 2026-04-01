/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CALIBRATION_H_
#define APP_CALIBRATION_H_

#include <stdbool.h>

#define APP_CALIBRATION_ACTIVATION_WINDOW_MIN 5

#ifdef __cplusplus
extern "C" {
#endif

bool app_calibration_is_active(void);
void app_calibration_apply_keys(void);
int app_calibration_init(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CALIBRATION_H_ */
