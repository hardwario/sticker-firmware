/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_NFC_INGEST_H_
#define APP_NFC_INGEST_H_

#include <stddef.h>
#include <stdint.h>

#include "src/nfc_config.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

bool app_nfc_ingest(const NfcConfigMessage *message);

/* Apply LoRaWAN / Application config fields into the staging config (app_config()).
 * Valid fields are written; an invalid one is skipped and (if fault_field is
 * non-NULL) its proto field tag is recorded in *fault_field (first offender).
 * Returns 0 if all present fields were valid, -EINVAL otherwise. */
int app_config_apply_lorawan(const NfcConfigMessage_Lorawan *src, uint32_t *fault_field);
int app_config_apply_application(const NfcConfigMessage_Application *src, uint32_t *fault_field);

/* Populate dst with only the requested fields (by proto field tag in ids[0..n))
 * from the staging config, setting their has_* flags. Secret keys are not dumped. */
void app_config_fill_lorawan(NfcConfigMessage_Lorawan *dst, const uint32_t *ids, size_t n);
void app_config_fill_application(NfcConfigMessage_Application *dst, const uint32_t *ids, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* APP_NFC_INGEST_H_ */
