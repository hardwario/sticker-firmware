/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CONFIG_INGEST_H_
#define APP_CONFIG_INGEST_H_

#include <stddef.h>
#include <stdint.h>

#include "src/app_config.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

bool app_config_ingest(const AppConfigMessage *message);

/* Apply LoRaWAN / Application config fields into the staging config (app_config()).
 * Valid fields are written; an invalid one is skipped and (if fault_field is
 * non-NULL) its proto field tag is recorded in *fault_field (first offender).
 * Returns 0 if all present fields were valid, -EINVAL otherwise. */
int app_config_apply_lorawan(const AppConfigMessage_Lorawan *src, uint32_t *fault_field);
int app_config_apply_application(const AppConfigMessage_Application *src, uint32_t *fault_field);

/* Populate dst with only the requested fields (by proto field tag in ids[0..n))
 * from the staging config, setting their has_* flags. Secret keys are not dumped. */
void app_config_fill_lorawan(AppConfigMessage_Lorawan *dst, const uint32_t *ids, size_t n);
void app_config_fill_application(AppConfigMessage_Application *dst, const uint32_t *ids, size_t n);

/* Cross-validate the alarm threshold pairs on `cfg`. An enabled threshold alarm
 * can only deactivate inside lo+hst < v < hi-hst; that band is empty when
 * lo + 2*hst >= hi (which also covers lo >= hi), so an activated alarm would
 * latch until NaN/reboot and hold the alarm LED. Sets *fault_field (if non-NULL)
 * to the offending alarm's `lo` proto tag and returns -EINVAL on the first bad
 * enabled pair; returns 0 if all enabled pairs are valid. Does not mutate cfg.
 * One source of truth shared by the shell/SetParam/NFC commit paths. */
struct app_config;
int app_config_validate_alarm_pairs(const struct app_config *cfg, uint32_t *fault_field);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_INGEST_H_ */
