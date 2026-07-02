/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CONFIG_INGEST_H_
#define APP_CONFIG_INGEST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/app_config.pb.h"

#include "app_cmd.h" /* enum app_cmd_transport (per-field write-transport gating, M-3) */

#ifdef __cplusplus
extern "C" {
#endif

/* Apply LoRaWAN / Application config fields into the staging config (app_config()).
 * Valid fields are written; an invalid one is skipped and (if fault_field is
 * non-NULL) its proto field tag is recorded in *fault_field (first offender).
 * `tp` is the transport the SetParam arrived on: a field whose `writable` list
 * excludes `tp` is rejected (M-3). Returns 0 if all present fields were valid and
 * writable on `tp`; -EACCES if a field was not writable over `tp`; -EINVAL on an
 * invalid value (first offender wins). */
int app_config_apply_lorawan(enum app_cmd_transport tp, const AppConfigMessage_Lorawan *src,
			     uint32_t *fault_field);
int app_config_apply_application(enum app_cmd_transport tp, const AppConfigMessage_Application *src,
				 uint32_t *fault_field);
int app_config_apply_sensors(enum app_cmd_transport tp, const AppConfigMessage_Sensors *src,
			     uint32_t *fault_field);
int app_config_apply_alarms(enum app_cmd_transport tp, const AppConfigMessage_Alarms *src,
			    uint32_t *fault_field);

/* Populate dst with only the requested fields (by proto field tag in ids[0..n))
 * from the staging config, setting their has_* flags. Secret keys are not dumped. */
void app_config_fill_lorawan(AppConfigMessage_Lorawan *dst, const uint32_t *ids, size_t n);
void app_config_fill_application(AppConfigMessage_Application *dst, const uint32_t *ids, size_t n);
void app_config_fill_sensors(AppConfigMessage_Sensors *dst, const uint32_t *ids, size_t n);
void app_config_fill_alarms(AppConfigMessage_Alarms *dst, const uint32_t *ids, size_t n);

/* True when alarms dump field `tag` (3..18 -> alarm_0..15) is an unconfigured
 * (all-zero) slot. Such slots are omitted from a ConfigDump; the get_config
 * paging loop calls this to skip their byte budget so page_count matches what
 * fill_alarms() actually emits. Returns false for any non-slot tag. GENERATED
 * from dump_omit_if_zero params in app_config.yml. */
bool app_config_alarms_slot_empty(uint32_t tag);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_INGEST_H_ */
