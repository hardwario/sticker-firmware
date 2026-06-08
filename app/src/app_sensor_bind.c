/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_w1_slots.h"

/* Zephyr includes */
#include <zephyr/kernel.h>

#if defined(CONFIG_SHELL)

#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static const char *type_name(enum app_w1_slot_type t)
{
	switch (t) {
	case APP_W1_SLOT_DALLAS:
		return "dallas";
	case APP_W1_SLOT_MACHINE_PROBE:
		return "machine-probe";
	default:
		return "empty";
	}
}

static int cmd_sensor_list(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "SLOT  TYPE           ROM           STATE      READING");
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		enum app_w1_slot_type t = app_w1_slot_get_type(s);

		if (t == APP_W1_SLOT_EMPTY) {
			shell_print(shell, "%-4d  (empty)", s + 1);
			continue;
		}

		const char *state = app_w1_slot_is_present(s)    ? "present"
				    : app_w1_slot_is_replaced(s) ? "REPLACED?"
								 : "absent";

		struct app_w1_slot_reading r;
		char reading[40] = "--";
		if (app_w1_slots_read(s, &r) == 0 && r.present) {
			if (!isnan(r.humidity)) {
				snprintf(reading, sizeof(reading), "%.2f C / %.1f %%%s",
					 (double)r.temperature, (double)r.humidity,
					 r.is_tilt_alert ? " / TILT" : "");
			} else if (!isnan(r.temperature)) {
				snprintf(reading, sizeof(reading), "%.2f C", (double)r.temperature);
			}
		}

		shell_print(shell, "%-4d  %-13s  %012llx  %-9s  %s", s + 1, type_name(t),
			    app_w1_slot_get_rom(s), state, reading);
	}
	return 0;
}

static int cmd_sensor_scan(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct app_w1_scan_entry e[APP_W1_SLOT_COUNT * 2];
	int n = app_w1_slots_scan(e, APP_W1_SLOT_COUNT * 2);

	if (n < 0) {
		shell_error(shell, "scan failed: %d", n);
		return n;
	}
	if (n == 0) {
		shell_print(shell, "no 1-Wire devices found");
		return 0;
	}

	shell_print(shell, "TYPE           ROM           BOUND");
	for (int i = 0; i < n; i++) {
		if (e[i].bound_slot >= 0) {
			shell_print(shell, "%-13s  %012llx  slot %d", type_name(e[i].type),
				    e[i].serial, e[i].bound_slot + 1);
		} else {
			shell_print(shell, "%-13s  %012llx  -", type_name(e[i].type), e[i].serial);
		}
	}
	return 0;
}

static int parse_slot(const struct shell *shell, const char *arg, int *slot)
{
	int s = atoi(arg);

	if (s < 1 || s > APP_W1_SLOT_COUNT) {
		shell_error(shell, "slot must be 1..%d", APP_W1_SLOT_COUNT);
		return -EINVAL;
	}
	*slot = s - 1;
	return 0;
}

static int cmd_sensor_teach(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int slot;

	if (parse_slot(shell, argv[1], &slot) != 0) {
		return -EINVAL;
	}

	struct app_w1_scan_entry bound;
	int ret = app_w1_slots_teach(slot, &bound);

	switch (ret) {
	case 0:
		shell_print(shell, "bound slot %d -> %s ROM %012llx (run `config save` to persist)",
			    slot + 1, type_name(bound.type), bound.serial);
		return 0;
	case -EAGAIN:
		shell_error(shell, "no new sensor found — unplug already-bound sensors, or use "
				   "`sensor assign <slot> <rom>`");
		return ret;
	case -E2BIG:
		shell_error(shell, "more than one new sensor on the bus — plug only one, or use "
				   "`sensor assign <slot> <rom>`");
		return ret;
	default:
		shell_error(shell, "teach failed: %d", ret);
		return ret;
	}
}

static int cmd_sensor_assign(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int slot;

	if (parse_slot(shell, argv[1], &slot) != 0) {
		return -EINVAL;
	}

	uint64_t serial = strtoull(argv[2], NULL, 16);
	if (serial == 0) {
		shell_error(shell, "invalid ROM (expected hex serial)");
		return -EINVAL;
	}

	int ret = app_w1_slots_assign(slot, serial);

	switch (ret) {
	case 0:
		shell_print(shell, "assigned ROM %012llx to slot %d (run `config save` to persist)",
			    serial, slot + 1);
		return 0;
	case -ENODEV:
		shell_error(shell, "ROM %012llx not found on the bus (run `sensor scan`)", serial);
		return ret;
	case -EEXIST:
		shell_error(shell, "ROM %012llx already bound to another slot", serial);
		return ret;
	default:
		shell_error(shell, "assign failed: %d", ret);
		return ret;
	}
}

static int cmd_sensor_clear(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	int slot;

	if (parse_slot(shell, argv[1], &slot) != 0) {
		return -EINVAL;
	}

	int ret = app_w1_slots_clear(slot);
	if (ret != 0) {
		shell_error(shell, "clear failed: %d", ret);
		return ret;
	}
	shell_print(shell, "slot %d cleared (run `config save` to persist)", slot + 1);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_sensor,
	SHELL_CMD_ARG(list, NULL, "List slot table (slot -> ROM -> type -> state -> reading).",
		      cmd_sensor_list, 1, 0),
	SHELL_CMD_ARG(scan, NULL, "Scan the 1-Wire bus and list every device.", cmd_sensor_scan, 1,
		      0),
	SHELL_CMD_ARG(teach, NULL, "Bind the single newly-plugged device to <slot> (1..4).",
		      cmd_sensor_teach, 2, 0),
	SHELL_CMD_ARG(assign, NULL, "Bind ROM <rom-hex> to <slot> (1..4).", cmd_sensor_assign, 3,
		      0),
	SHELL_CMD_ARG(clear, NULL, "Forget the sensor bound to <slot> (1..4).", cmd_sensor_clear, 2,
		      0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sensor, &sub_sensor, "1-Wire sensor enrollment (teach / scan / assign).", NULL);

#endif /* defined(CONFIG_SHELL) */
