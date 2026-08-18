/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_settings.h"
#include "app_alarm_rules.h"
#include "app_config.h"
#include "app_config_ingest.h"
#include "app_counters.h"
#include "app_hall.h"
#include "app_history.h"
#include "app_input.h"
#include "app_lrw.h"
#include "app_nfc.h"

/* Zephyr includes */
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

/* Standard includes */
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_settings, LOG_LEVEL_DBG);

#if defined(CONFIG_SHELL)
static const char m_shell_msg_error[] = "command failed";
#endif /* defined(CONFIG_SHELL) */

static int save(bool reboot)
{
	int ret;

	/* Alarm threshold-pair cross-validation was removed with the fixed alarm
	 * config keys (dynamic-alarms migration); alarm rules validate on their own
	 * SET path in app_alarm_rules, so there is nothing to pre-check here. */

	ret = settings_save();
	if (ret) {
		LOG_ERR("Call `settings_save` failed: %d", ret);
		return ret;
	}

	if (reboot) {
		sys_reboot(SYS_REBOOT_COLD);
	}

	return 0;
}

/* Wipe the whole "config" settings backing store (identity + LoRaWAN credentials
 * included). Split out from erase() below only so a future caller could erase
 * without rebooting immediately — but see the GOTCHA in app_settings_vendor_reset()
 * (#299) for why "erase raw flash, then keep using the settings API this same
 * boot" is NOT actually safe; every current caller erases only right before a
 * reboot with no further settings_save_*() in between. */
static int erase_storage(void)
{
	int ret;

#if defined(CONFIG_SETTINGS_FILE)
	/* Settings in external FLASH as a LittleFS file */
	ret = fs_unlink(CONFIG_SETTINGS_FILE_PATH);
	if (ret) {
		LOG_ERR("Call `fs_unlink` failed: %d", ret);
	}

	/* Needs to be static so it is zero-ed */
	static struct fs_file_t file;
	ret = fs_open(&file, CONFIG_SETTINGS_FILE_PATH, FS_O_CREATE);
	if (ret) {
		LOG_ERR("Call `fs_open` failed: %d", ret);
		return ret;
	}

	ret = fs_close(&file);
	if (ret) {
		LOG_ERR("Call `fs_close` failed: %d", ret);
		return ret;
	}
#else
	/* Settings in the internal FLASH partition */
	const struct flash_area *fa;
	ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
	if (ret) {
		LOG_ERR("Call `flash_area_open` failed: %d", ret);
		return ret;
	}

	ret = flash_area_erase(fa, 0, FIXED_PARTITION_SIZE(storage_partition));
	if (ret) {
		LOG_ERR("Call `flash_area_erase` failed: %d", ret);
		flash_area_close(fa);
		return ret;
	}

	flash_area_close(fa);
#endif /* defined(CONFIG_SETTINGS_FILE) */

	return 0;
}

static int erase(bool reboot)
{
	int ret = erase_storage();
	if (ret) {
		return ret;
	}

	if (reboot) {
		sys_reboot(SYS_REBOOT_COLD);
	}

	return 0;
}

#if defined(CONFIG_SHELL)

static int cmd_save(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = save(true);
	if (ret) {
		LOG_ERR("Call `save` failed: %d", ret);
		shell_error(shell, "%s", m_shell_msg_error);
		return ret;
	}

	return 0;
}

static int cmd_device_reset(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = app_settings_device_reset();
	if (ret) {
		LOG_ERR("Call `app_settings_device_reset` failed: %d", ret);
		shell_error(shell, "%s", m_shell_msg_error);
		return ret;
	}

	return 0;
}

static int cmd_factory_reset(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = app_settings_factory_reset();
	if (ret) {
		LOG_ERR("Call `app_settings_factory_reset` failed: %d", ret);
		shell_error(shell, "%s", m_shell_msg_error);
		return ret;
	}

	return 0;
}

static int cmd_vendor_reset(const struct shell *shell, size_t argc, char **argv)
{
	int ret;
	uint8_t key[16];

	if (argc != 2 || strlen(argv[1]) != 2 * sizeof(key)) {
		shell_error(shell, "usage: settings vendor-reset <new-secret-key (32 hex digits)>");
		return -EINVAL;
	}

	ret = hex2bin(argv[1], strlen(argv[1]), key, sizeof(key));
	if (ret != (int)sizeof(key)) {
		shell_error(shell, "invalid key");
		return -EINVAL;
	}

	ret = app_settings_vendor_reset(key);
	if (ret) {
		LOG_ERR("Call `app_settings_vendor_reset` failed: %d", ret);
		shell_error(shell, "%s", m_shell_msg_error);
		return ret;
	}

	return 0;
}

static int cmd_erase(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = erase(true);
	if (ret) {
		LOG_ERR("Call `erase` failed: %d", ret);
		shell_error(shell, "%s", m_shell_msg_error);
		return ret;
	}

	return 0;
}

static int print_help(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_error(shell, "command not found: %s", argv[1]);
		shell_help(shell);
		return -EINVAL;
	}

	shell_help(shell);

	return 0;
}

/* clang-format off */

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_settings,

	SHELL_CMD_ARG(save, NULL,
	              "Save all settings and reboot.",
	              cmd_save, 1, 0),

	SHELL_CMD_ARG(device-reset, NULL,
	              "Reset config + alarm rules to defaults (keeps identity + LoRaWAN) and reboot.",
	              cmd_device_reset, 1, 0),

	SHELL_CMD_ARG(factory-reset, NULL,
	              "Reset config + alarm rules to defaults (keeps identity only, drops LoRaWAN) and reboot.",
	              cmd_factory_reset, 1, 0),

	SHELL_CMD_ARG(vendor-reset, NULL,
	              "Erase storage+history and re-provision (keeps serial_number + vendor_token only), then reboot. Usage: settings vendor-reset <new-secret-key (32 hex digits)>",
	              cmd_vendor_reset, 2, 0),

	SHELL_CMD_ARG(erase, NULL,
	              "Erase the whole NVS partition incl. identity + LoRaWAN credentials, then reboot.",
	              cmd_erase, 1, 0),

	SHELL_SUBCMD_SET_END
);

/* clang-format on */

SHELL_CMD_REGISTER(settings, &sub_settings, "Settings commands.", print_help);

#endif /* defined(CONFIG_SHELL) */

int app_settings_save(bool reboot)
{
	return save(reboot);
}

int app_settings_save_nonce_counter(void)
{
	/* Single-key write to the "config" settings subtree (see SETTINGS_PFX in the
	 * generated app_config.c). Persisting just this key keeps the NFC accept path
	 * cheap and avoids rewriting the whole config blob. */
	return settings_save_one("config/nonce-counter", &app_config()->nonce_counter,
				 sizeof(app_config()->nonce_counter));
}

/* Persist only secret_key to NVS as a single settings key — same single-key
 * rationale as nonce_counter above. Internal to vendor_reset below: the
 * set_secret_key command (#299) used to persist through here too, but a narrow
 * persist alone never made the new key live (it does not touch g_app_config,
 * which is what the NFC channel authenticates from), so it now goes through the
 * full app_settings_save(true) save+reboot instead (#322). */
static int save_secret_key(void)
{
	return settings_save_one("config/secret-key", app_config()->secret_key,
				 sizeof(app_config()->secret_key));
}

/* Shared by every reset-ladder tier: clear the decoded alarm-rule cache so the
 * running state matches the just-reset config slots immediately, then persist.
 * Returns 0 or a negative errno; the caller reboots. */
static int clear_and_save_alarm_rules(void)
{
	int ret;

	app_alarm_rules_clear_all();
	ret = app_alarm_rules_save();
	if (ret) {
		LOG_ERR("Call `app_alarm_rules_save` failed: %d", ret);
	}

	return ret;
}

/* factory_reset and vendor_reset both reset the LoRaWAN identity/keys back to
 * defaults (app_config.yml persistent tiers) — unlike device_reset, whose
 * persistent tier keeps ALL lorawan_* fields untouched. Without this, the
 * LoRaMac stack would keep running with its own persisted NVM (frame
 * counters + DevNonce + session state) built under the OLD keys, risking a
 * frame-counter desync with the network server after re-provisioning. Call
 * this immediately before every `sys_reboot()` in those two tiers, including
 * the reboot-on-error paths: app_config_factory_reset()/
 * app_config_vendor_reset() may already have zeroed the keys on flash even if
 * a later step in the same function then fails and reboots.
 *
 * Deliberately NOT wired into a live SetParam key change (e.g. someone
 * changing lrw_nwkskey via NFC/shell without going through a reset tier) —
 * that would need a synchronous cross-module call from app_cmd.c into
 * app_lrw.c's NVM handling from an arbitrary caller thread, which is riskier
 * and out of scope here. Left as a follow-up. */
#if defined(CONFIG_LORAWAN)
static void lrw_reset_nvm_before_reboot(void)
{
	app_lrw_reset_nvm();
}
#else
static void lrw_reset_nvm_before_reboot(void)
{
}
#endif /* defined(CONFIG_LORAWAN) */

int app_settings_device_reset(void)
{
	int ret;

	/* Config: defaults for everything except the persistent device_reset fields
	 * (identity + full LoRaWAN, see app_config_device_reset). This is the only
	 * reset reachable over LoRaWAN/NFC, so a remote command can never
	 * un-provision the device past this tier; a full wipe (incl. identity) stays
	 * shell-only via `settings erase`. */
	ret = app_config_device_reset();
	if (ret) {
		LOG_ERR("Call `app_config_device_reset` failed: %d", ret);
		return ret;
	}

	/* Past this point the reset has already begun: app_config_device_reset()
	 * already wrote to flash. A nonzero return from here on would leave the
	 * device running live with a half-applied reset, so any further failure
	 * reboots instead (see the atomicity note in app_settings.h). No
	 * lrw_reset_nvm_before_reboot() here: device_reset's persistent tier keeps
	 * every lorawan_* field, so the LoRaMac NVM stays valid across it. */
	/* Error already logged inside; this tier reboots either way (see the
	 * atomicity note in app_settings.h). */
	clear_and_save_alarm_rules();

	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

int app_settings_factory_reset(void)
{
	int ret;

	/* Narrower than device_reset (#299): also drops the LoRaWAN session/keys —
	 * only the persistent factory_reset fields (identity) survive. */
	ret = app_config_factory_reset();
	if (ret) {
		LOG_ERR("Call `app_config_factory_reset` failed: %d", ret);
		return ret;
	}

	/* Past this point the reset has already begun: app_config_factory_reset()
	 * already wrote to flash (including zeroing the LoRaWAN keys back to
	 * defaults). A nonzero return from here on would leave the device running
	 * live with a half-applied reset, so any further failure reboots instead
	 * (see the atomicity note in app_settings.h) — and every reboot from here
	 * on must also wipe the LoRaMac stack's own NVM, since the keys it was
	 * built under are already gone. */
	/* Error already logged inside; this tier reboots either way. */
	clear_and_save_alarm_rules();

	lrw_reset_nvm_before_reboot();
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

static bool key_is_set(const uint8_t *key, size_t size)
{
	if (!key) {
		return false;
	}

	for (size_t i = 0; i < size; i++) {
		if (key[i] != 0) {
			return true;
		}
	}

	return false;
}

int app_settings_vendor_reset(const uint8_t *new_secret_key)
{
	int ret;

	/* Policy gate (#299): a device that disabled vendor recovery, or a caller
	 * that didn't supply the mandatory replacement key, gets -EACCES before
	 * anything destructive happens — same failure for both, deliberately: never
	 * hint which check failed to an unauthenticated/unauthorized caller. */
	if (!app_config()->vendor_reset_allow) {
		LOG_WRN("vendor_reset refused: vendor_reset_allow is false");
		return -EACCES;
	}

	if (!key_is_set(new_secret_key, sizeof(app_config()->secret_key))) {
		LOG_WRN("vendor_reset refused: no replacement secret_key supplied");
		return -EACCES;
	}

	/* GOTCHA (found on hardware): do NOT flash_area_erase() the live-mounted
	 * "storage" partition and then keep using the settings API in the same boot
	 * session. Zephyr's NVS caches its write pointers/sector state in RAM at
	 * mount time and does not know the flash changed under it — every
	 * settings_save_*() call after a raw erase silently stops persisting
	 * correctly for the rest of the session (confirmed empirically: an earlier
	 * version of this function erased first, then called app_config_vendor_reset()
	 * + save_secret_key() same as below, and EVERY field — including
	 * serial_number/vendor_token, which should have survived — came back all-zero
	 * after reboot). So, narrowest tier (#299) or not, this goes through the same
	 * live settings API as device_reset/factory_reset above: no raw erase of the
	 * settings-backed "storage" partition at all. */
	ret = app_config_vendor_reset();
	if (ret) {
		LOG_ERR("Call `app_config_vendor_reset` failed: %d", ret);
		return ret;
	}

	/* Past this point the reset has already begun: app_config_vendor_reset()
	 * already wrote to flash (including zeroing the LoRaWAN keys back to
	 * defaults and secret_key to the all-zero sentinel below). A nonzero return
	 * from here on would leave the device running live with a half-applied
	 * reset, so any further failure reboots instead (see the atomicity note in
	 * app_settings.h) — and every reboot from here on must also wipe the
	 * LoRaMac stack's own NVM, since the keys it was built under are already
	 * gone. */

	/* secret_key is NOT in the vendor_reset persistent tier (app_config.yml), so
	 * the call above just zeroed it — the all-zero "unprovisioned" sentinel that
	 * would lock the encrypted NFC channel out from ever reaching set_secret_key
	 * again. Apply + persist the caller-supplied replacement now, separately
	 * (single-key save, same rationale as nonce_counter/set_secret_key above). */
	memcpy(app_config()->secret_key, new_secret_key, sizeof(app_config()->secret_key));
	ret = save_secret_key();
	if (ret) {
		LOG_ERR("Call `save_secret_key` failed: %d", ret);
		lrw_reset_nvm_before_reboot();
		sys_reboot(SYS_REBOOT_COLD);
	}

	/* The pulse totalizers and the NFC claim-record state each live in their own
	 * settings subtree, outside "config" — device_reset/factory_reset leave them
	 * alone on purpose, but vendor_reset is the deep tier meant to look like a
	 * freshly manufactured device, so wipe both explicitly through their own live
	 * APIs (same reasoning as the GOTCHA above: no raw erase). */
	app_hall_reset_counts();
	app_input_reset_counts();
	ret = app_counters_save(true);
	if (ret) {
		LOG_ERR("Call `app_counters_save` failed: %d", ret);
		lrw_reset_nvm_before_reboot();
		sys_reboot(SYS_REBOOT_COLD);
	}
	app_nfc_clm_reset();

	/* History is a separate, non-NVS flash ring (its own on-flash format, not
	 * settings-API-backed), so a raw erase here is safe — app_history_clear()
	 * also resets its own in-RAM bookkeeping to match. */
	app_history_clear();

	app_alarm_rules_clear_all();
	ret = app_alarm_rules_save();
	if (ret) {
		LOG_ERR("Call `app_alarm_rules_save` failed: %d", ret);
		lrw_reset_nvm_before_reboot();
		sys_reboot(SYS_REBOOT_COLD);
	}

	lrw_reset_nvm_before_reboot();
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}
