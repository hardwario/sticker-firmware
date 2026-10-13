/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake 1-Wire bus for the app_w1_slots.c host tests: the config store plus the
 * DS18B20 / machine-probe driver calls. Cases put devices on the bus through
 * test_bus_set(); every driver read succeeds for a device on the bus.
 */

#include "app_config.h"
#include "app_ds18b20.h"
#include "app_machine_probe.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

struct app_config g_app_config;
static struct app_config m_staging;

struct app_config *app_config(void)
{
	return &m_staging;
}

#define BUS_MAX 4

static uint64_t m_dallas[BUS_MAX];
static int m_dallas_n;
static uint64_t m_mp[BUS_MAX];
static int m_mp_n;

void test_bus_set(const uint64_t *dallas, int n_dallas, const uint64_t *mp, int n_mp)
{
	m_dallas_n = n_dallas;
	m_mp_n = n_mp;
	for (int i = 0; i < n_dallas; i++) {
		m_dallas[i] = dallas[i];
	}
	for (int i = 0; i < n_mp; i++) {
		m_mp[i] = mp[i];
	}
}

/* ---- DS18B20 ---- */

int app_ds18b20_scan(void)
{
	return 0;
}

int app_ds18b20_get_count(void)
{
	return m_dallas_n;
}

int app_ds18b20_read(int index, uint64_t *serial_number, float *temperature)
{
	if (index < 0 || index >= m_dallas_n) {
		return -ENODEV;
	}
	*serial_number = m_dallas[index];
	*temperature = 21.0f;
	return 0;
}

/* ---- machine probe ---- */

static int mp_serial(int index, uint64_t *serial_number)
{
	if (index < 0 || index >= m_mp_n) {
		return -ENODEV;
	}
	*serial_number = m_mp[index];
	return 0;
}

int app_machine_probe_scan(void)
{
	return 0;
}

int app_machine_probe_get_count(void)
{
	return m_mp_n;
}

int app_machine_probe_read_thermometer(int index, uint64_t *serial_number, float *temperature)
{
	*temperature = 22.0f;
	return mp_serial(index, serial_number);
}

int app_machine_probe_read_hygrometer(int index, uint64_t *serial_number, float *temperature,
				      float *humidity)
{
	*temperature = 23.0f;
	*humidity = 45.0f;
	return mp_serial(index, serial_number);
}

int app_machine_probe_read_lux_meter(int index, uint64_t *serial_number, float *illuminance)
{
	*illuminance = 100.0f;
	return mp_serial(index, serial_number);
}

int app_machine_probe_read_magnetometer(int index, uint64_t *serial_number, float *magnetic_field)
{
	*magnetic_field = 0.1f;
	return mp_serial(index, serial_number);
}

int app_machine_probe_read_accelerometer(int index, uint64_t *serial_number, float *accel_x,
					 float *accel_y, float *accel_z, int *orientation)
{
	*accel_x = 0.0f;
	*accel_y = 0.0f;
	*accel_z = 9.81f;
	*orientation = 0;
	return mp_serial(index, serial_number);
}

int app_machine_probe_get_tilt_alert(int index, uint64_t *serial_number, bool *is_active)
{
	*is_active = false;
	return mp_serial(index, serial_number);
}
