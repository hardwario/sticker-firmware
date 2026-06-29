/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_DFU_META_H
#define APP_DFU_META_H

#ifdef __cplusplus
extern "C" {
#endif

/* Seed the NFC bootloader's `sfu_meta` record with this device's secret_key +
 * serial, so the bootloader can authenticate encrypted DFU frames addressed to
 * it. Idempotent: a no-op (no flash wear) when the stored record already carries
 * the current key/serial and marks the slot bootable. Only built for the
 * variant-B image that runs behind the bootloader (CONFIG_USE_DT_CODE_PARTITION).
 *
 * Returns 0 on success or no-op, a negative errno on flash failure. A failure is
 * non-fatal to the caller: DFU just falls back to unkeyed.
 */
int app_dfu_meta_provision(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_DFU_META_H */
