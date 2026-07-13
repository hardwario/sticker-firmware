/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SETTINGS_H_
#define APP_SETTINGS_H_

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_settings_save(bool reboot);

/* Reset ladder (#299) for the shell `settings <op>` and the matching LoRaWAN/NFC
 * command. Each restores every config parameter (and clears all dynamic alarm
 * rules) to defaults, keeping only the tier's persistent fields (app_config.yml
 * `persistent: [...]`), then reboots:
 *   device_reset  - keeps identity + full LoRaWAN. Renamed from the old,
 *                    single `app_settings_reset()` (was: today's factory_reset
 *                    command / `settings reset`) — same wire id, reachable over
 *                    LoRaWAN/NFC/shell.
 *   factory_reset  - keeps identity only; drops the LoRaWAN session/keys —
 *                    the device must re-join after this. Also reachable over
 *                    LoRaWAN/NFC/shell.
 *   vendor_reset   - keeps only serial_number + vendor_token; also wipes the
 *                    pulse totalizers, the NFC claim-record state, and the
 *                    history ring — everything a freshly manufactured device
 *                    would not yet have. Goes through the same live settings
 *                    API as the two tiers above (a raw flash_area_erase() of
 *                    the settings-backed "storage" partition would desync
 *                    Zephyr's NVS for the rest of the boot — see the comment
 *                    in app_settings.c); only the separate, non-NVS history
 *                    partition is raw-erased. Never reachable over LoRaWAN or
 *                    the generic hio.stck:cmd channel — only the shell
 *                    (`settings vendor-reset`) or NFC's own vendor-token
 *                    hio.stck:rst record (app_nfc.c) ever reach it. */
int app_settings_device_reset(void);
int app_settings_factory_reset(void);

/* vendor_reset also drops secret_key (not in its persistent tier) — leaving it
 * at the all-zero "unprovisioned" sentinel would lock the device out of its own
 * encrypted NFC channel (the key_is_provisioned() gate in app_nfc.c), with no
 * way back in short of a J-Link. So the caller MUST supply the replacement key
 * in the same call; `new_secret_key` (16 bytes) is rejected (-EACCES) if NULL
 * or all-zero, same as the `vendor_reset_allow` config gate below. Returns
 * -EACCES if `vendor_reset_allow` is false or `new_secret_key` is missing/
 * all-zero, otherwise 0 or a negative errno from the erase/reset itself. */
int app_settings_vendor_reset(const uint8_t *new_secret_key);

/* Full NVS wipe: erase the whole storage partition (identity + LoRaWAN
 * credentials included), then reboot. Destructive and un-provisions the device,
 * so it is reserved for the local shell `settings erase` only — never wired to
 * any LoRaWAN or NFC path. */
int app_settings_erase(void);

/* Persist only the NFC anti-replay nonce counter to NVS as a single settings
 * key (not the whole config blob). Called from the NFC decrypt path on every
 * accepted command so a captured command can't be replayed after a power-cycle.
 * Returns 0 or a negative errno. */
int app_settings_save_nonce_counter(void);

/* Persist only secret_key to NVS as a single settings key (#299, set_secret_key
 * command). Returns 0 or a negative errno. */
int app_settings_save_secret_key(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETTINGS_H_ */
