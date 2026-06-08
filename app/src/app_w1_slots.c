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

/* Standard includes */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_w1_slots, LOG_LEVEL_DBG);

/* Max discovered devices per transport driver (matches the driver arrays). */
#define DRIVER_MAX 2

struct slot_rt {
	uint64_t rom; /* 48-bit serial; 0 = empty */
	enum app_w1_slot_type type;
	uint32_t sht;
	int driver_index; /* index into the type's transport driver; -1 = absent */
	bool present;
	bool replaced; /* configured ROM absent but a same-type device showed up */
};

static struct slot_rt m_slots[APP_W1_SLOT_COUNT];

/* ---- config accessors (flat sensorN_* keys, no array in g_app_config) ---- */

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

static uint32_t *cfg_type(int slot)
{
	switch (slot) {
	case 0:
		return &g_app_config.sensor1_type;
	case 1:
		return &g_app_config.sensor2_type;
	case 2:
		return &g_app_config.sensor3_type;
	case 3:
		return &g_app_config.sensor4_type;
	default:
		return NULL;
	}
}

static uint32_t *cfg_sht(int slot)
{
	switch (slot) {
	case 0:
		return &g_app_config.sensor1_sht;
	case 1:
		return &g_app_config.sensor2_sht;
	case 2:
		return &g_app_config.sensor3_sht;
	case 3:
		return &g_app_config.sensor4_sht;
	default:
		return NULL;
	}
}

/* ROM serial is stored in the 8-byte config field as a little-endian u64
 * (only the low 48 bits are used; upper bytes 0). 0 = empty slot. */
static uint64_t cfg_rom_get(int slot)
{
	return sys_get_le64(cfg_rom(slot));
}

static void cfg_rom_set(int slot, uint64_t serial)
{
	sys_put_le64(serial, cfg_rom(slot));
}

/* ---- type registry: probe each driver's discovered devices for serials ---- */

struct discovered {
	uint64_t serial;
	int driver_index;
	bool claimed;
};

/* Collect the serials currently exposed by the DALLAS transport. Uses the
 * driver read (which also yields the serial); throwaway value is fine at init. */
static int collect_dallas(struct discovered *out, int max)
{
	int n = 0;
	int count = app_ds18b20_get_count();

	for (int i = 0; i < count && n < max; i++) {
		uint64_t serial = 0;
		float temperature;

		if (app_ds18b20_read(i, &serial, &temperature) == 0 && serial != 0) {
			out[n].serial = serial;
			out[n].driver_index = i;
			out[n].claimed = false;
			n++;
		}
	}
	return n;
}

static int collect_machine_probe(struct discovered *out, int max)
{
	int n = 0;
	int count = app_machine_probe_get_count();

	for (int i = 0; i < count && n < max; i++) {
		uint64_t serial = 0;
		float temperature, humidity;

		if (app_machine_probe_read_hygrometer(i, &serial, &temperature, &humidity) == 0 &&
		    serial != 0) {
			out[n].serial = serial;
			out[n].driver_index = i;
			out[n].claimed = false;
			n++;
		}
	}
	return n;
}

/* ---- rebind ------------------------------------------------------------- */

int app_w1_slots_rebind(void)
{
	struct discovered dallas[DRIVER_MAX];
	struct discovered probe[DRIVER_MAX];
	int n_dallas = collect_dallas(dallas, DRIVER_MAX);
	int n_probe = collect_machine_probe(probe, DRIVER_MAX);

	/* Load persisted slot identity and reset runtime binding. */
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		m_slots[s].rom = cfg_rom_get(s);
		m_slots[s].type = (enum app_w1_slot_type) * cfg_type(s);
		m_slots[s].sht = *cfg_sht(s);
		m_slots[s].driver_index = -1;
		m_slots[s].present = false;
		m_slots[s].replaced = false;
	}

	/* Pass 1: bind configured slots to a discovered device with matching ROM. */
	for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
		if (m_slots[s].rom == 0 || m_slots[s].type == APP_W1_SLOT_EMPTY) {
			continue;
		}

		struct discovered *list = NULL;
		int n = 0;
		if (m_slots[s].type == APP_W1_SLOT_DALLAS) {
			list = dallas;
			n = n_dallas;
		} else if (m_slots[s].type == APP_W1_SLOT_MACHINE_PROBE) {
			list = probe;
			n = n_probe;
		}

		for (int i = 0; i < n; i++) {
			if (!list[i].claimed && list[i].serial == m_slots[s].rom) {
				list[i].claimed = true;
				m_slots[s].driver_index = list[i].driver_index;
				m_slots[s].present = true;
				LOG_INF("Slot %d bound to %s ROM %012llx (idx %d)", s + 1,
					m_slots[s].type == APP_W1_SLOT_DALLAS ? "dallas"
									      : "machine-probe",
					m_slots[s].rom, list[i].driver_index);
				break;
			}
		}

		if (!m_slots[s].present) {
			/* Configured but its ROM was not seen. If an unclaimed same-type
			 * device exists, the sensor was likely swapped — flag it, do NOT
			 * silently rebind (alarm correctness). */
			for (int i = 0; i < n; i++) {
				if (!list[i].claimed) {
					m_slots[s].replaced = true;
					break;
				}
			}
			LOG_WRN("Slot %d ROM %012llx absent%s", s + 1, m_slots[s].rom,
				m_slots[s].replaced ? " (REPLACED? different device present)" : "");
		}
	}

	/* Pass 2: auto-enroll unclaimed devices into the lowest empty slot. Written
	 * to the staging config in RAM (idempotent each boot until `config save`
	 * makes it durable); this is the first-bind / migration path. */
	for (int i = 0; i < n_dallas; i++) {
		if (dallas[i].claimed) {
			continue;
		}
		for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
			if (m_slots[s].rom == 0 && m_slots[s].type == APP_W1_SLOT_EMPTY) {
				m_slots[s].rom = dallas[i].serial;
				m_slots[s].type = APP_W1_SLOT_DALLAS;
				m_slots[s].driver_index = dallas[i].driver_index;
				m_slots[s].present = true;
				cfg_rom_set(s, dallas[i].serial);
				*cfg_type(s) = APP_W1_SLOT_DALLAS;
				LOG_INF("Slot %d auto-enrolled dallas ROM %012llx", s + 1,
					dallas[i].serial);
				dallas[i].claimed = true;
				break;
			}
		}
	}
	for (int i = 0; i < n_probe; i++) {
		if (probe[i].claimed) {
			continue;
		}
		for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
			if (m_slots[s].rom == 0 && m_slots[s].type == APP_W1_SLOT_EMPTY) {
				m_slots[s].rom = probe[i].serial;
				m_slots[s].type = APP_W1_SLOT_MACHINE_PROBE;
				m_slots[s].driver_index = probe[i].driver_index;
				m_slots[s].present = true;
				cfg_rom_set(s, probe[i].serial);
				*cfg_type(s) = APP_W1_SLOT_MACHINE_PROBE;
				LOG_INF("Slot %d auto-enrolled machine-probe ROM %012llx", s + 1,
					probe[i].serial);
				probe[i].claimed = true;
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

	if (!m_slots[slot].present || m_slots[slot].driver_index < 0) {
		return 0;
	}

	uint64_t serial;
	int ret = -ENOTSUP;

	switch (m_slots[slot].type) {
	case APP_W1_SLOT_DALLAS: {
		float temperature;
		ret = app_ds18b20_read(m_slots[slot].driver_index, &serial, &temperature);
		if (ret == 0) {
			out->temperature = temperature;
		}
		break;
	}
	case APP_W1_SLOT_MACHINE_PROBE: {
		float temperature, humidity;
		ret = app_machine_probe_read_hygrometer(m_slots[slot].driver_index, &serial,
							&temperature, &humidity);
		if (ret == 0) {
			out->temperature = temperature;
			out->humidity = humidity;
		}
		bool tilt = false;
		if (app_machine_probe_get_tilt_alert(m_slots[slot].driver_index, &serial, &tilt) ==
		    0) {
			out->is_tilt_alert = tilt;
		}
		break;
	}
	default:
		break;
	}

	return ret;
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
