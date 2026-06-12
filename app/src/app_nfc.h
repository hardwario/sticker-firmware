/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_NFC_H_
#define APP_NFC_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum app_nfc_action {
	APP_NFC_ACTION_NONE = 0,
	APP_NFC_ACTION_SAVE = 1,
	APP_NFC_ACTION_RESET = 2,
};

int app_nfc_init(void);

/* Full check: always reads the tag. Use at boot and for on-demand checks. */
int app_nfc_check(enum app_nfc_action *action);

/* Gated poll for the periodic check: reads the 1-byte IT_STS_Dyn first and only
 * does the full read when RF activity is flagged. Cheaper when nothing changed. */
int app_nfc_poll(enum app_nfc_action *action);

/* Whether the main loop should run the periodic NFC check. Toggled by the
 * `nfc autocheck on|off` shell command so a multi-step `nfc write` of a config
 * blob is not raced (and overwritten) by the periodic check mid-write. */
bool app_nfc_periodic_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_NFC_H_ */
