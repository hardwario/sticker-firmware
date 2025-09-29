/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_NFC_INGEST_H_
#define APP_NFC_INGEST_H_

#include "src/nfc_config.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_nfc_ingest(const NfcConfigMessage *message);

#ifdef __cplusplus
}
#endif

#endif /* APP_NFC_INGEST_H_ */