/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_settings.h"

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

LOG_MODULE_REGISTER(app_settings, LOG_LEVEL_DBG);

#if defined(CONFIG_SHELL)
static const char m_shell_msg_error[] = "command failed";
#endif

static int save(bool reboot)
{
	int ret;

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

static int reset(bool reboot)
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
		return ret;
	}

	flash_area_close(fa);
#endif /* defined(CONFIG_SETTINGS_FILE) */

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

static int cmd_reset(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = reset(true);
	if (ret) {
		LOG_ERR("Call `reset` failed: %d", ret);
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

	SHELL_CMD_ARG(reset, NULL,
	              "Reset all settings and reboot.",
	              cmd_reset, 1, 0),

	SHELL_SUBCMD_SET_END
);

/* clang-format on */

SHELL_CMD_REGISTER(settings, &sub_settings, "Settings commands.", print_help);

#endif /* defined(CONFIG_SHELL) */

int app_settings_save(void)
{
	return save(true);
}

int app_settings_reset(void)
{
	return reset(true);
}
