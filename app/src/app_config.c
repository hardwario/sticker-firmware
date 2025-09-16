/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"

/* Zephyr includes */
#include <zephyr/fs/fs.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

/* Standard includes */
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(app_config, LOG_LEVEL_DBG);

#define SETTINGS_PFX "config"

struct app_config g_app_config;

static struct app_config m_app_config = {
	.interval_report = 900,
};

static int h_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	int ret;
	const char *next;

#define SETTINGS_SET(_key, _var, _size)                                                            \
	do {                                                                                       \
		if (settings_name_steq(key, _key, &next) && !next) {                               \
			if (len != _size) {                                                        \
				return -EINVAL;                                                    \
			}                                                                          \
                                                                                                   \
			ret = read_cb(cb_arg, _var, len);                                          \
                                                                                                   \
			if (ret < 0) {                                                             \
				LOG_ERR("Call `read_cb` failed: %d", ret);                         \
				return ret;                                                        \
			}                                                                          \
                                                                                                   \
			return 0;                                                                  \
		}                                                                                  \
	} while (0)

	SETTINGS_SET("secret-key", m_app_config.secret_key, sizeof(m_app_config.secret_key));
	SETTINGS_SET("serial-number", &m_app_config.serial_number,
		     sizeof(m_app_config.serial_number));
	SETTINGS_SET("nonce-counter", &m_app_config.nonce_counter,
		     sizeof(m_app_config.nonce_counter));
	SETTINGS_SET("calibration", &m_app_config.calibration, sizeof(m_app_config.calibration));
	SETTINGS_SET("interval-sample", &m_app_config.interval_sample,
		     sizeof(m_app_config.interval_sample));
	SETTINGS_SET("interval-report", &m_app_config.interval_report,
		     sizeof(m_app_config.interval_report));
	SETTINGS_SET("lrw-deveui", m_app_config.lrw_deveui, sizeof(m_app_config.lrw_deveui));
	SETTINGS_SET("lrw-devaddr", m_app_config.lrw_devaddr, sizeof(m_app_config.lrw_devaddr));
	SETTINGS_SET("lrw-nwkskey", m_app_config.lrw_nwkskey, sizeof(m_app_config.lrw_nwkskey));
	SETTINGS_SET("lrw-appskey", m_app_config.lrw_appskey, sizeof(m_app_config.lrw_appskey));
	SETTINGS_SET("corr-temperature", &m_app_config.corr_temperature,
		     sizeof(m_app_config.corr_temperature));
	SETTINGS_SET("corr-ext-temperature-1", &m_app_config.corr_ext_temperature_1,
		     sizeof(m_app_config.corr_ext_temperature_1));
	SETTINGS_SET("corr-ext-temperature-2", &m_app_config.corr_ext_temperature_2,
		     sizeof(m_app_config.corr_ext_temperature_2));
	SETTINGS_SET("has-mpl3115a2", &m_app_config.has_mpl3115a2,
		     sizeof(m_app_config.has_mpl3115a2));

#undef SETTINGS_SET

	return -ENOENT;
}

static int h_commit(void)
{
	LOG_DBG("Loaded settings in full");

	memcpy(&g_app_config, &m_app_config, sizeof(g_app_config));
	return 0;
}

static int h_export(int (*export_func)(const char *name, const void *val, size_t val_len))
{
#define EXPORT_FUNC(_key, _var, _size)                                                             \
	do {                                                                                       \
		(void)export_func(SETTINGS_PFX "/" _key, _var, _size);                             \
	} while (0)

	EXPORT_FUNC("secret-key", m_app_config.secret_key, sizeof(m_app_config.secret_key));
	EXPORT_FUNC("serial-number", &m_app_config.serial_number,
		    sizeof(m_app_config.serial_number));
	EXPORT_FUNC("nonce-counter", &m_app_config.nonce_counter,
		    sizeof(m_app_config.nonce_counter));
	EXPORT_FUNC("calibration", &m_app_config.calibration, sizeof(m_app_config.calibration));
	EXPORT_FUNC("interval-sample", &m_app_config.interval_sample,
		    sizeof(m_app_config.interval_sample));
	EXPORT_FUNC("interval-report", &m_app_config.interval_report,
		    sizeof(m_app_config.interval_report));
	EXPORT_FUNC("lrw-deveui", m_app_config.lrw_deveui, sizeof(m_app_config.lrw_deveui));
	EXPORT_FUNC("lrw-devaddr", m_app_config.lrw_devaddr, sizeof(m_app_config.lrw_devaddr));
	EXPORT_FUNC("lrw-nwkskey", m_app_config.lrw_nwkskey, sizeof(m_app_config.lrw_nwkskey));
	EXPORT_FUNC("lrw-appskey", m_app_config.lrw_appskey, sizeof(m_app_config.lrw_appskey));
	EXPORT_FUNC("corr-temperature", &m_app_config.corr_temperature,
		    sizeof(m_app_config.corr_temperature));
	EXPORT_FUNC("corr-ext-temperature-1", &m_app_config.corr_ext_temperature_1,
		    sizeof(m_app_config.corr_ext_temperature_1));
	EXPORT_FUNC("corr-ext-temperature-2", &m_app_config.corr_ext_temperature_2,
		    sizeof(m_app_config.corr_ext_temperature_2));
	EXPORT_FUNC("has-mpl3115a2", &m_app_config.has_mpl3115a2,
		    sizeof(m_app_config.has_mpl3115a2));

#undef EXPORT_FUNC

	return 0;
}

static int init(void)
{
	int ret;

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("Call `settings_subsys_init` failed: %d", ret);
		return ret;
	}

	static struct settings_handler sh = {
		.name = SETTINGS_PFX,
		.h_set = h_set,
		.h_commit = h_commit,
		.h_export = h_export,
	};

	ret = settings_register(&sh);
	if (ret) {
		LOG_ERR("Call `settings_register` failed: %d", ret);
		return ret;
	}

	ret = settings_load_subtree(SETTINGS_PFX);
	if (ret) {
		LOG_ERR("Call `settings_load_subtree` failed: %d", ret);
		return ret;
	}

	return 0;
}

SYS_INIT(init, APPLICATION, 0);

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
		LOG_WRN("Call `fs_unlink` failed: %d", ret);
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
	if (ret < 0) {
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

static void print_secret_key(const struct shell *shell)
{
	char buf[2 * sizeof(m_app_config.secret_key) + 1];

	int ret =
		bin2hex(m_app_config.secret_key, sizeof(m_app_config.secret_key), buf, sizeof(buf));
	if (ret == 0) {
		LOG_ERR("Call `bin2hex` failed: %d", ret);
		return;
	}

	shell_print(shell, SETTINGS_PFX " secret-key %s", buf);
}

static void print_serial_number(const struct shell *shell)
{
	shell_print(shell, SETTINGS_PFX " serial-number %010u", m_app_config.serial_number);
}

static void print_nonce_counter(const struct shell *shell)
{
	shell_print(shell, SETTINGS_PFX " nonce-counter %u", m_app_config.nonce_counter);
}

static void print_calibration(const struct shell *shell)
{
	shell_print(shell, SETTINGS_PFX " calibration %s",
		    m_app_config.calibration ? "true" : "false");
}

static void print_interval_sample(const struct shell *shell)
{
	shell_print(shell, SETTINGS_PFX " interval-sample %d", m_app_config.interval_sample);
}

static void print_interval_report(const struct shell *shell)
{
	shell_print(shell, SETTINGS_PFX " interval-report %d", m_app_config.interval_report);
}

static void print_lrw_deveui(const struct shell *shell)
{
	int ret;

	char buf[2 * sizeof(m_app_config.lrw_deveui) + 1];

	ret = bin2hex(m_app_config.lrw_deveui, sizeof(m_app_config.lrw_deveui), buf, sizeof(buf));
	if (ret == 0) {
		LOG_ERR("Call `hex2char` failed: %d", ret);
		return;
	}

	shell_print(shell, SETTINGS_PFX " lrw-deveui %s", buf);
}

static void print_lrw_devaddr(const struct shell *shell)
{
	int ret;

	char buf[2 * sizeof(m_app_config.lrw_devaddr) + 1];

	ret = bin2hex(m_app_config.lrw_devaddr, sizeof(m_app_config.lrw_devaddr), buf, sizeof(buf));
	if (ret == 0) {
		LOG_ERR("Call `hex2char` failed: %d", ret);
		return;
	}

	shell_print(shell, SETTINGS_PFX " lrw-devaddr %s", buf);
}

static void print_lrw_nwkskey(const struct shell *shell)
{
	int ret;

	char buf[2 * sizeof(m_app_config.lrw_nwkskey) + 1];

	ret = bin2hex(m_app_config.lrw_nwkskey, sizeof(m_app_config.lrw_nwkskey), buf, sizeof(buf));
	if (ret == 0) {
		LOG_ERR("Call `hex2char` failed: %d", ret);
		return;
	}

	shell_print(shell, SETTINGS_PFX " lrw-nwkskey %s", buf);
}

static void print_lrw_appskey(const struct shell *shell)
{
	int ret;

	char buf[2 * sizeof(m_app_config.lrw_appskey) + 1];

	ret = bin2hex(m_app_config.lrw_appskey, sizeof(m_app_config.lrw_appskey), buf, sizeof(buf));
	if (ret == 0) {
		LOG_ERR("Call `hex2char` failed: %d", ret);
		return;
	}

	shell_print(shell, SETTINGS_PFX " lrw-appskey %s", buf);
}

static void print_corr_temperature(const struct shell *shell)
{
	shell_print(shell, SETTINGS_PFX " corr-temperature %.2f",
		    (double)m_app_config.corr_temperature);
}

static void print_corr_ext_temperature_1(const struct shell *shell)
{
	shell_print(shell, SETTINGS_PFX " corr-ext-temperature-1 %.2f",
		    (double)m_app_config.corr_ext_temperature_1);
}

static void print_corr_ext_temperature_2(const struct shell *shell)
{
	shell_print(shell, SETTINGS_PFX " corr-ext-temperature-2 %.2f",
		    (double)m_app_config.corr_ext_temperature_2);
}

static void print_has_mpl3115a2(const struct shell *shell)
{
	shell_print(shell, SETTINGS_PFX " has-mpl3115a2 %s",
		    m_app_config.has_mpl3115a2 ? "true" : "false");
}

static int cmd_show(const struct shell *shell, size_t argc, char **argv)
{
	print_secret_key(shell);
	print_serial_number(shell);
	print_nonce_counter(shell);
	print_calibration(shell);
	print_interval_sample(shell);
	print_interval_report(shell);
	print_lrw_deveui(shell);
	print_lrw_devaddr(shell);
	print_lrw_nwkskey(shell);
	print_lrw_appskey(shell);
	print_corr_temperature(shell);
	print_corr_ext_temperature_1(shell);
	print_corr_ext_temperature_2(shell);
	print_has_mpl3115a2(shell);

	return 0;
}

static int cmd_save(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	ret = save(true);
	if (ret) {
		LOG_ERR("Call `save` failed: %d", ret);
		shell_error(shell, "command failed");
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
		shell_error(shell, "command failed");
		return ret;
	}

	return 0;
}

static int cmd_secret_key(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc == 1) {
		print_secret_key(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	if (strlen(argv[1]) != 2 * sizeof(m_app_config.secret_key)) {
		shell_error(shell, "invalid argument length");
		return -EINVAL;
	}

	ret = hex2bin(argv[1], strlen(argv[1]), m_app_config.secret_key,
		      sizeof(m_app_config.secret_key));
	if (ret == 0) {
		LOG_ERR("Call `hex2bin` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	return 0;
}

static int cmd_serial_number(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		print_serial_number(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	size_t len = strlen(argv[1]);

	if (len != 10) {
		shell_error(shell, "invalid argument format");
		return -EINVAL;
	}

	for (size_t i = 0; i < len; i++) {
		if (!isdigit((int)argv[1][i])) {
			shell_error(shell, "invalid argument format");
			return -EINVAL;
		}
	}

	m_app_config.serial_number = strtoul(argv[1], NULL, 10);

	return 0;
}

static int cmd_nonce_counter(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		print_nonce_counter(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	m_app_config.nonce_counter = strtoul(argv[1], NULL, 10);

	return 0;
}

static int cmd_calibration(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		print_calibration(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	if (!strcmp(argv[1], "true")) {
		m_app_config.calibration = true;
	} else if (!strcmp(argv[1], "false")) {
		m_app_config.calibration = false;
	} else {
		shell_error(shell, "invalid argument");
		return -EINVAL;
	}

	return 0;
}

static int cmd_interval_sample(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		print_interval_sample(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	int a = strtol(argv[1], NULL, 10);

	if (a != 0 && (a < 5 || a > 3600)) {
		shell_error(shell, "invalid argument range");
		return -EINVAL;
	}

	m_app_config.interval_sample = a;

	return 0;
}

static int cmd_interval_report(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		print_interval_report(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	int a = strtol(argv[1], NULL, 10);

	if (a < 60 || a > 86400) {
		shell_error(shell, "invalid argument range");
		return -EINVAL;
	}

	m_app_config.interval_report = a;

	return 0;
}

static int cmd_lrw_deveui(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc == 1) {
		print_lrw_deveui(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	if (strlen(argv[1]) != 2 * sizeof(m_app_config.lrw_deveui)) {
		shell_error(shell, "invalid argument length");
		return -EINVAL;
	}

	uint8_t buf[sizeof(m_app_config.lrw_deveui)];

	ret = hex2bin(argv[1], strlen(argv[1]), buf, sizeof(buf));
	if (ret == 0) {
		LOG_ERR("Call `hex2bin` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	memcpy(m_app_config.lrw_deveui, buf, sizeof(m_app_config.lrw_deveui));

	return 0;
}

static int cmd_lrw_devaddr(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc == 1) {
		print_lrw_devaddr(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	if (strlen(argv[1]) != 2 * sizeof(m_app_config.lrw_devaddr)) {
		shell_error(shell, "invalid argument length");
		return -EINVAL;
	}

	uint8_t buf[sizeof(m_app_config.lrw_devaddr)];

	ret = hex2bin(argv[1], strlen(argv[1]), buf, sizeof(buf));
	if (ret == 0) {
		LOG_ERR("Call `hex2bin` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	memcpy(m_app_config.lrw_devaddr, buf, sizeof(m_app_config.lrw_devaddr));

	return 0;
}

static int cmd_lrw_nwkskey(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc == 1) {
		print_lrw_nwkskey(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	if (strlen(argv[1]) != 2 * sizeof(m_app_config.lrw_nwkskey)) {
		shell_error(shell, "invalid argument length");
		return -EINVAL;
	}

	uint8_t buf[sizeof(m_app_config.lrw_nwkskey)];

	ret = hex2bin(argv[1], strlen(argv[1]), buf, sizeof(buf));
	if (ret == 0) {
		LOG_ERR("Call `hex2bin` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	memcpy(m_app_config.lrw_nwkskey, buf, sizeof(m_app_config.lrw_nwkskey));

	return 0;
}

static int cmd_lrw_appskey(const struct shell *shell, size_t argc, char **argv)
{
	int ret;

	if (argc == 1) {
		print_lrw_appskey(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	if (strlen(argv[1]) != 2 * sizeof(m_app_config.lrw_appskey)) {
		shell_error(shell, "invalid argument length");
		return -EINVAL;
	}

	uint8_t buf[sizeof(m_app_config.lrw_appskey)];

	ret = hex2bin(argv[1], strlen(argv[1]), buf, sizeof(buf));
	if (ret == 0) {
		LOG_ERR("Call `hex2bin` failed: %d", ret);
		shell_error(shell, "command failed");
		return ret;
	}

	memcpy(m_app_config.lrw_appskey, buf, sizeof(m_app_config.lrw_appskey));

	return 0;
}

static int cmd_corr_temperature(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		print_corr_temperature(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	float a = atof(argv[1]);

	if (a < -5.f || a > 5.f) {
		shell_error(shell, "invalid argument range");
		return -EINVAL;
	}

	m_app_config.corr_temperature = a;

	return 0;
}

static int cmd_corr_ext_temperature_1(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		print_corr_ext_temperature_1(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	float a = atof(argv[1]);

	if (a < -5.f || a > 5.f) {
		shell_error(shell, "invalid argument range");
		return -EINVAL;
	}

	m_app_config.corr_ext_temperature_1 = a;

	return 0;
}

static int cmd_corr_ext_temperature_2(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		print_corr_ext_temperature_2(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	float a = atof(argv[1]);

	if (a < -5.f || a > 5.f) {
		shell_error(shell, "invalid argument range");
		return -EINVAL;
	}

	m_app_config.corr_ext_temperature_2 = a;

	return 0;
}

static int cmd_has_mpl3115a2(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		print_has_mpl3115a2(shell);
		return 0;
	}

	if (argc != 2) {
		shell_error(shell, "invalid number of arguments");
		return -EINVAL;
	}

	if (!strcmp(argv[1], "true")) {
		m_app_config.has_mpl3115a2 = true;
	} else if (!strcmp(argv[1], "false")) {
		m_app_config.has_mpl3115a2 = false;
	} else {
		shell_error(shell, "invalid argument");
		return -EINVAL;
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
	sub_config,

	SHELL_CMD_ARG(show, NULL,
	              "Show all configuration.",
	              cmd_show, 1, 0),

	SHELL_CMD_ARG(save, NULL,
	              "Save all configuration.",
	              cmd_save, 1, 0),

	SHELL_CMD_ARG(reset, NULL,
	              "Reset all configuration.",
	              cmd_reset, 1, 0),

	SHELL_CMD_ARG(secret-key, NULL,
			"Get/Set device secret key (32 hexadecimal digits).",
			cmd_secret_key, 1, 1),

	SHELL_CMD_ARG(serial-number, NULL,
	              "Get/Set device serial number (10 decimal digits).",
	              cmd_serial_number, 1, 1),

	SHELL_CMD_ARG(nonce-counter, NULL,
	              "Get/Set nonce counter (unsigned integer).",
	              cmd_nonce_counter, 1, 1),

	SHELL_CMD_ARG(calibration, NULL,
	              "Get/Set calibration mode (true/false).",
	              cmd_calibration, 1, 1),

	SHELL_CMD_ARG(interval-sample, NULL,
	              "Get/Set sample interval (range 5 to 3600 seconds; 0 = precede report).",
	              cmd_interval_sample, 1, 1),

	SHELL_CMD_ARG(interval-report, NULL,
	              "Get/Set report interval (range 60 to 86400 seconds).",
	              cmd_interval_report, 1, 1),

	SHELL_CMD_ARG(lrw-deveui, NULL,
	              "Get/Set LoRaWAN DevEUI (16 hex digits).",
	              cmd_lrw_deveui, 1, 1),

	SHELL_CMD_ARG(lrw-devaddr, NULL,
	              "Get/Set LoRaWAN DevAddr (8 hex digits).",
	              cmd_lrw_devaddr, 1, 1),

	SHELL_CMD_ARG(lrw-nwkskey, NULL,
	              "Get/Set LoRaWAN NwkSKey (32 hexadecimal digits).",
	              cmd_lrw_nwkskey, 1, 1),

	SHELL_CMD_ARG(lrw-appskey, NULL,
	              "Get/Set LoRaWAN AppSKey (32 hexadecimal digits).",
	              cmd_lrw_appskey, 1, 1),

	SHELL_CMD_ARG(corr-temperature, NULL,
	              "Get/Set temperature correction (range -5.0 to +5.0 degrees of Celsius).",
	              cmd_corr_temperature, 1, 1),

	SHELL_CMD_ARG(corr-ext-temperature-1, NULL,
	              "Get/Set external temperature 1 correction (range -5.0 to +5.0 degrees of Celsius).",
	              cmd_corr_ext_temperature_1, 1, 1),

	SHELL_CMD_ARG(corr-ext-temperature-2, NULL,
	              "Get/Set external temperature 2 correction (range -5.0 to +5.0 degrees of Celsius).",
	              cmd_corr_ext_temperature_2, 1, 1),

	SHELL_CMD_ARG(has-mpl3115a2, NULL,
	              "Get/Set capability MPL3115A2 (true/false).",
	              cmd_has_mpl3115a2, 1, 1),

	SHELL_SUBCMD_SET_END
);

/* clang-format on */

SHELL_CMD_REGISTER(config, &sub_config, "Configuration commands.", print_help);

#endif /* defined(CONFIG_SHELL) */

struct app_config *app_config(void)
{
	return &m_app_config;
}

int app_config_save(void)
{
	int ret;

	ret = save(true);
	if (ret) {
		LOG_ERR("Call `save` failed: %d", ret);
		return ret;
	}

	return 0;
}

int app_config_reset(void)
{
	int ret;

	ret = reset(true);
	if (ret) {
		LOG_ERR("Call `reset` failed: %d", ret);
		return ret;
	}

	return 0;
}
