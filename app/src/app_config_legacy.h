/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CONFIG_LEGACY_H_
#define APP_CONFIG_LEGACY_H_

/* Zephyr includes */
#include <zephyr/settings/settings.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct app_config;

/* Conversion of NVS keys left behind by removed config parameters (#498).
 * Called only from the generated app_config.c (module.legacy_hook in
 * app_config.yml):
 *
 *   app_config_legacy_set()     — from h_set for a key no parameter claims:
 *                                 keeps a known legacy value, else -ENOENT;
 *   app_config_legacy_commit()  — from h_commit: writes the replacement
 *                                 parameters into `cfg`, returns true when any
 *                                 legacy key was loaded (config must be saved);
 *   app_config_legacy_cleanup() — after the converted config is persisted:
 *                                 deletes the legacy keys from NVS.
 *
 * Legacy keys (all 1-byte bool): cap-input-a/-b, cap-analog-a/-b,
 * cap-pir-detector, cap-buzzer. They map onto cap_gpio_mode and
 * cap_gpio_input_a/_b with the precedence app_sensor_init() applied to them at
 * run time: PIR > buzzer > digital > analog. */
int app_config_legacy_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg);
bool app_config_legacy_commit(struct app_config *cfg);
void app_config_legacy_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_LEGACY_H_ */
