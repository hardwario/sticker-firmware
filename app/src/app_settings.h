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

/* Reset for the shell `settings reset` and the factory_reset command (LoRaWAN /
 * NFC): restore every config parameter (and clear all dynamic alarm rules) to
 * defaults but KEEP the device identity and LoRaWAN credentials, then reboot, so
 * the device stays provisioned and on the network. This is the only reset
 * reachable remotely, so no command can ever un-provision a field device. */
int app_settings_reset(void);

/* Full NVS wipe: erase the whole storage partition (identity + LoRaWAN
 * credentials included), then reboot. Destructive and un-provisions the device,
 * so it is reserved for the local shell `settings erase` only — never wired to
 * any LoRaWAN or NFC path. */
int app_settings_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETTINGS_H_ */
