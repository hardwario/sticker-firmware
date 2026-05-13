/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SETTINGS_H_
#define APP_SETTINGS_H_

/* Standard includes */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_settings_save(bool reboot);
int app_settings_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETTINGS_H_ */
