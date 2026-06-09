/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_w1_slots.h"
#include "app_config.h"
#include "app_ds18b20.h"
#include "app_log.h"
#include "app_machine_probe.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_w1_slots, LOG_LEVEL_DBG);

/* ====================================================================== */
/* Sensor-type registry — the extensibility point.                        */
/*                                                                        */
/* Adding a new 1-Wire sensor type is one entry in m_types[] plus its     */
/* thin read wrapper below (and a transport driver + devicetree nodes,    */
/* which any new hardware needs regardless). No changes to the slot       */
/* table, rebind, scan, read dispatch, shell or telemetry plumbing.       */
/* ====================================================================== */

/* Fill *out (temperature/humidity/tilt) for device `index` of this type and
 * return its ROM serial in *serial. Quantities the type doesn't provide stay
 * at the caller-initialised NaN/false. Returns 0 or negative errno. */
typedef int (*w1_read_fn)(int index, uint64_t *serial, struct app_w1_slot_reading *out);

struct app_w1_sensor_type {
	enum app_w1_slot_type type;
	uint8_t family;    /* 1-Wire family code (informational) */
	const char *name;  /* shell / log label */
	int (*scan)(void); /* re-enumerate this transport's devices */
	int (*get_count)(void);
	w1_read_fn read;
};

static int dallas_read(int index, uint64_t *serial, struct app_w1_slot_reading *out)
{
	float temperature;
	int ret = app_ds18b20_read(index, serial, &temperature);

	if (ret == 0) {
		out->temperature = temperature;
	}
	return ret;
}

static int machine_probe_read(int index, uint64_t *serial, struct app_w1_slot_reading *out)
{
	float temperature, humidity;
	int ret = app_machine_probe_read_hygrometer(index, serial, &temperature, &humidity);

	if (ret == 0) {
		out->temperature = temperature;
		out->humidity = humidity;
	}

	bool tilt = false;
	if (app_machine_probe_get_tilt_alert(index, serial, &tilt) == 0) {
		out->is_tilt_alert = tilt;
	}
	return ret;
}

static const struct app_w1_sensor_type m_types[] = {
	{APP_W1_SLOT_DALLAS, 0x28, "dallas", app_ds18b20_scan, app_ds18b20_get_count, dallas_read},
	{APP_W1_SLOT_MACHINE_PROBE, 0x19, "machine-probe", app_machine_probe_scan,
	 app_machine_probe_get_count, machine_probe_read},
};

static const struct app_w1_sensor_type *type_desc(enum app_w1_slot_type type)
{
	for (size_t i = 0; i < ARRAY_SIZE(m_types); i++) {
		if (m_types[i].type == type) {
			return &m_types[i];
		}
	}
	return NULL;
}

const char *app_w1_slot_type_name(enum app_w1_slot_type type)
{
	const struct app_w1_sensor_type *t = type_desc(type);

	return t != NULL ? t->name : "empty";
}

/* Upper bound on devices discoverable across all types in one scan. */
#define DISCOVERED_MAX (APP_W1_SLOT_COUNT * 2)

/* ---- slot table --------------------------------------------------------- */

struct slot_rt {
	uint64_t rom; /* 48-bit serial; 0 = empty */
	enum app_w1_slot_type type;
	uint32_t sht;
	const struct app_w1_sensor_type *desc; /* bound type descriptor, NULL if empty */
	int driver_index;                      /* index into the type's driver; -1 = absent */
	bool present;
	bool replaced; /* configured ROM absent but a same-type device showed up */
};

static struct slot_rt m_slots[APP_W1_SLOT_COUNT];

/* ---- config accessors (flat sensorN_* keys, no array in g_app_config) ---- */

/* The runtime config (g_app_config, read by all modules) and the staging config
 * (app_config(), what `settings save` persists and `config get/set` use) are
 * separate instances — g is memcpy'd from staging at boot. cfg_rom() returns the
 * runtime ROM; cfg_rom_staging() the persisted one. */
static uint8_t *cfg_rom(int slot)
{
	switch (slot) {
	case 0:
		return g_app_config.sensor1_rom;
	case 1:
		return g_app_config.sensor2_rom;
	case 2:
		return g_app_config.sensor3_rom;
	case 3:
		return g_app_config.sensor4_rom;
	default:
		return NULL;
	}
}

static uint8_t *cfg_rom_staging(int slot)
{
	struct app_config *c = app_config();

	switch (slot) {
	case 0:
		return c->sensor1_rom;
	case 1:
		return c->sensor2_rom;
	case 2:
		return c->sensor3_rom;
	case 3:
		return c->sensor4_rom;
	default:
		return NULL;
	}
}

/* Only the ROM is persisted — a slot's type (and SHT variant) are auto-detected
 * at runtime from the discovered device's family code, not stored in config.
 * ROM serial is stored in the 8-byte field as a little-endian u64 (low 48 bits
 * used). 0 = empty slot. */
static uint64_t cfg_rom_get(int slot)
{
	return sys_get_le64(cfg_rom(slot));
}

static void cfg_rom_set(int slot, uint64_t serial)
{
	/* Write both: runtime (for the live rebind below) + staging (so
	 * `settings save` persists it and `config get` shows it). */
	sys_put_le64(serial, cfg_rom(slot));
	sys_put_le64(serial, cfg_rom_staging(slot));
}

/* ---- discovery (registry-driven) --------------------------------------- */

struct discovered {
	uint64_t serial;
	int driver_index;
	const struct app_w1_sensor_type *desc;
	bool claimed;
};

/* Enumerate every device currently exposed by every registered type. Uses each
 * type's read (which also yields the serial); throwaway readings are fine. */
static int collect_all(struct discovered *out, int max)
{
	int n = 0;

	for (size_t ti = 0; ti < ARRAY_SIZE(m_types); ti++) {
		const struct app_w1_sensor_type *t = &m_types[ti];
		int count = t->get_count();

		for (int i = 0; i < count && n < max; i++) {
			uint64_t serial = 0;
			struct app_w1_slot_reading r = {.temperature = NAN, .humidity = NAN};

			if (t->read(i, &serial, &r) == 0 && serial != 0) {
				out[n].serial = serial;
				out[n].driver_index = i;
				out[n].desc = t;
				out[n].claimed = false;
				n++;
			}
		}
	}
	return n;
}

/* ---- rebind ------------------------------------------------------------- */

int app_w1_slots_rebind(void)
{
	struct discovered dev[DISCOVERED_MAX];
	int ndev = collect_all(dev, DISCOVERED_MAX);

	/* Load persisted slot identity (ROM only) and reset runtime state. The
	 * type/driver are resolved from the discovered device's family below. */
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		m_slots[s].rom = cfg_rom_get(s);
		m_slots[s].type = APP_W1_SLOT_EMPTY;
		m_slots[s].sht = 0;
		m_slots[s].desc = NULL;
		m_slots[s].driver_index = -1;
		m_slots[s].present = false;
		m_slots[s].replaced = false;
	}

	/* Pass 1: bind each configured slot (ROM set) to the discovered device with
	 * the matching serial; its type comes from that device's family. */
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		if (m_slots[s].rom == 0) {
			continue;
		}

		for (int d = 0; d < ndev; d++) {
			if (!dev[d].claimed && dev[d].serial == m_slots[s].rom) {
				dev[d].claimed = true;
				m_slots[s].desc = dev[d].desc;
				m_slots[s].type = dev[d].desc->type;
				m_slots[s].driver_index = dev[d].driver_index;
				m_slots[s].present = true;
				LOG_INF("Slot %d bound to %s ROM %012llx (idx %d)", s + 1,
					dev[d].desc->name, m_slots[s].rom, dev[d].driver_index);
				break;
			}
		}

		if (!m_slots[s].present) {
			/* Configured ROM not seen. If any unclaimed device is present the
			 * sensor was likely swapped — flag it, do NOT silently rebind
			 * (alarm correctness). */
			for (int d = 0; d < ndev; d++) {
				if (!dev[d].claimed) {
					m_slots[s].replaced = true;
					break;
				}
			}
			LOG_WRN("Slot %d ROM %012llx absent%s", s + 1, m_slots[s].rom,
				m_slots[s].replaced ? " (REPLACED? different device present)" : "");
		}
	}

	/* Pass 2: auto-enroll unclaimed devices into the lowest empty slot (ROM
	 * persisted; type stays runtime). Idempotent until `config save`. */
	for (int d = 0; d < ndev; d++) {
		if (dev[d].claimed) {
			continue;
		}
		for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
			if (m_slots[s].rom == 0) {
				m_slots[s].rom = dev[d].serial;
				m_slots[s].type = dev[d].desc->type;
				m_slots[s].desc = dev[d].desc;
				m_slots[s].driver_index = dev[d].driver_index;
				m_slots[s].present = true;
				cfg_rom_set(s, dev[d].serial);
				LOG_INF("Slot %d auto-enrolled %s ROM %012llx", s + 1,
					dev[d].desc->name, dev[d].serial);
				dev[d].claimed = true;
				break;
			}
		}
	}

	int present = 0;
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		if (m_slots[s].present) {
			present++;
		}
	}
	return present;
}

/* ---- read --------------------------------------------------------------- */

int app_w1_slots_read(int slot, struct app_w1_slot_reading *out)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT || out == NULL) {
		return -EINVAL;
	}

	out->temperature = NAN;
	out->humidity = NAN;
	out->is_tilt_alert = false;
	out->present = m_slots[slot].present;

	if (!m_slots[slot].present || m_slots[slot].driver_index < 0 ||
	    m_slots[slot].desc == NULL) {
		return 0;
	}

	uint64_t serial;
	return m_slots[slot].desc->read(m_slots[slot].driver_index, &serial, out);
}

/* ---- accessors ---------------------------------------------------------- */

enum app_w1_slot_type app_w1_slot_get_type(int slot)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT) {
		return APP_W1_SLOT_EMPTY;
	}
	return m_slots[slot].type;
}

uint64_t app_w1_slot_get_rom(int slot)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT) {
		return 0;
	}
	return m_slots[slot].rom;
}

bool app_w1_slot_is_present(int slot)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT) {
		return false;
	}
	return m_slots[slot].present;
}

bool app_w1_slot_is_replaced(int slot)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT) {
		return false;
	}
	return m_slots[slot].replaced;
}

/* ---- enrollment (sensor shell) ----------------------------------------- */

/* Which configured slot (staging config) holds this ROM, or -1. */
static int slot_of_rom(uint64_t serial)
{
	if (serial == 0) {
		return -1;
	}
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		if (cfg_rom_get(s) == serial) {
			return s;
		}
	}
	return -1;
}

int app_w1_slots_scan(struct app_w1_scan_entry *out, int max)
{
	if (out == NULL || max <= 0) {
		return -EINVAL;
	}

	/* Re-run every type's driver scan so freshly plugged devices appear. */
	for (size_t ti = 0; ti < ARRAY_SIZE(m_types); ti++) {
		(void)m_types[ti].scan();
	}

	struct discovered dev[DISCOVERED_MAX];
	int ndev = collect_all(dev, DISCOVERED_MAX);
	int n = 0;

	for (int d = 0; d < ndev && n < max; d++) {
		out[n].serial = dev[d].serial;
		out[n].type = dev[d].desc->type;
		out[n].bound_slot = slot_of_rom(dev[d].serial);
		n++;
	}
	return n;
}

int app_w1_slots_teach(int slot, struct app_w1_scan_entry *bound)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT) {
		return -EINVAL;
	}

	struct app_w1_scan_entry e[DISCOVERED_MAX];
	int n = app_w1_slots_scan(e, DISCOVERED_MAX);
	if (n < 0) {
		return n;
	}

	int new_idx = -1;
	int new_count = 0;
	for (int i = 0; i < n; i++) {
		if (e[i].bound_slot < 0) {
			new_count++;
			new_idx = i;
		}
	}

	if (new_count == 0) {
		return -EAGAIN; /* nothing new to teach */
	}
	if (new_count > 1) {
		return -E2BIG; /* ambiguous — caller should steer to assign */
	}

	cfg_rom_set(slot, e[new_idx].serial);
	if (bound != NULL) {
		*bound = e[new_idx];
	}
	(void)app_w1_slots_rebind(); /* apply live for immediate feedback */
	return 0;
}

int app_w1_slots_assign(int slot, uint64_t serial)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT || serial == 0) {
		return -EINVAL;
	}

	int existing = slot_of_rom(serial);
	if (existing >= 0 && existing != slot) {
		return -EEXIST; /* ROM already bound to another slot */
	}

	struct app_w1_scan_entry e[DISCOVERED_MAX];
	int n = app_w1_slots_scan(e, DISCOVERED_MAX);
	if (n < 0) {
		return n;
	}

	for (int i = 0; i < n; i++) {
		if (e[i].serial == serial) {
			cfg_rom_set(slot, serial);
			(void)app_w1_slots_rebind();
			return 0;
		}
	}
	return -ENODEV; /* serial not on the bus */
}

int app_w1_slots_clear(int slot)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT) {
		return -EINVAL;
	}
	cfg_rom_set(slot, 0);
	(void)app_w1_slots_rebind();
	return 0;
}

int app_w1_slots_enroll_all(void)
{
	/* Re-scan the bus, then rebind — pass 2 of rebind auto-enrolls every
	 * unbound device into the lowest free slot. Staged in config; the user
	 * persists with `settings save`. Returns the number of bound slots. */
	for (size_t ti = 0; ti < ARRAY_SIZE(m_types); ti++) {
		(void)m_types[ti].scan();
	}
	return app_w1_slots_rebind();
}

/* ---- `w1` shell (scan / enroll / list / clear) ------------------------- */

#if defined(CONFIG_SHELL)

#include <zephyr/shell/shell.h>

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

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

		shell_print(shell, "%-4d  %-13s  %012llx  %-9s  %s", s + 1,
			    app_w1_slot_type_name(t), app_w1_slot_get_rom(s), state, reading);
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
			shell_print(shell, "%-13s  %012llx  slot %d",
				    app_w1_slot_type_name(e[i].type), e[i].serial,
				    e[i].bound_slot + 1);
		} else {
			shell_print(shell, "%-13s  %012llx  -", app_w1_slot_type_name(e[i].type),
				    e[i].serial);
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

/* enroll               → batch: bind every unbound device into free slots.
 * enroll <slot>        → plug-one: bind the single newly-plugged device.
 * enroll <slot> <rom>  → explicit: bind that ROM (multi-sensor). */
static int cmd_sensor_enroll(const struct shell *shell, size_t argc, char **argv)
{
	int slot;

	if (argc == 1) {
		int n = app_w1_slots_enroll_all();

		shell_print(shell, "enrolled; %d slot(s) bound", n);
		return 0;
	}

	if (parse_slot(shell, argv[1], &slot) != 0) {
		return -EINVAL;
	}

	if (argc >= 3) {
		uint64_t serial = strtoull(argv[2], NULL, 16);

		if (serial == 0) {
			shell_error(shell, "invalid ROM (expected hex serial)");
			return -EINVAL;
		}

		int ret = app_w1_slots_assign(slot, serial);

		switch (ret) {
		case 0:
			shell_print(shell, "enrolled ROM %012llx to slot %d", serial, slot + 1);
			return 0;
		case -ENODEV:
			shell_error(shell, "ROM %012llx not on the bus (run `w1 scan`)", serial);
			return ret;
		case -EEXIST:
			shell_error(shell, "ROM %012llx already bound to another slot", serial);
			return ret;
		default:
			shell_error(shell, "enroll failed: %d", ret);
			return ret;
		}
	}

	struct app_w1_scan_entry bound;
	int ret = app_w1_slots_teach(slot, &bound);

	switch (ret) {
	case 0:
		shell_print(shell, "enrolled slot %d -> %s ROM %012llx", slot + 1,
			    app_w1_slot_type_name(bound.type), bound.serial);
		return 0;
	case -EAGAIN:
		shell_error(shell, "no new sensor found — plug one in, or pass the ROM: "
				   "`w1 enroll <slot> <rom>`");
		return ret;
	case -E2BIG:
		shell_error(shell, "more than one new sensor — pass the ROM: "
				   "`w1 enroll <slot> <rom>`");
		return ret;
	default:
		shell_error(shell, "enroll failed: %d", ret);
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
	shell_print(shell, "slot %d cleared", slot + 1);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_w1,
	SHELL_CMD_ARG(list, NULL, "List slot table (slot -> ROM -> type -> state -> reading).",
		      cmd_sensor_list, 1, 0),
	SHELL_CMD_ARG(scan, NULL, "Scan the 1-Wire bus and list every device.", cmd_sensor_scan, 1,
		      0),
	SHELL_CMD_ARG(enroll, NULL,
		      "Enroll sensors: `enroll` binds all unbound devices into free slots; "
		      "`enroll <slot>` binds the single newly-plugged device; "
		      "`enroll <slot> <rom-hex>` binds a specific ROM.",
		      cmd_sensor_enroll, 1, 2),
	SHELL_CMD_ARG(clear, NULL, "Forget the sensor bound to <slot> (1..4).", cmd_sensor_clear, 2,
		      0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(w1, &sub_w1, "1-Wire sensor management (scan / enroll / list / clear).", NULL);

#endif /* defined(CONFIG_SHELL) */
