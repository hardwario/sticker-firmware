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

/* Factory reset for the factory_reset command: restore every config parameter
 * (and clear all dynamic alarm rules) to defaults but KEEP the device identity
 * and LoRaWAN credentials, then reboot, so the device stays provisioned and on
 * the network. Differs from app_settings_reset(), which wipes the whole NVS
 * partition (identity included) and is reserved for the shell. */
int app_settings_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETTINGS_H_ */
