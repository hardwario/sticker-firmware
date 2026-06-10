/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stubs for app_compose's app-level dependencies. Tests set the test_* state.
 */

#include "app_hall.h"
#include "app_input.h"
#include "app_w1_slots.h"

#include "src/app_config.pb.h"

#include <math.h>
#include <stdint.h>

#define MP_FLAG_TILT (1u << 0)

uint8_t test_budget = 200;
struct app_hall_data test_hall;
struct app_input_data test_input;

/* Per-slot type the composer reads to decide which slots emit a SensorReading
 * (0 = APP_W1_SLOT_EMPTY = no reading). Tests set this. */
enum app_w1_slot_type test_w1_types[APP_W1_SLOT_COUNT];

enum app_w1_slot_type app_w1_slot_get_type(int slot)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT) {
		return APP_W1_SLOT_EMPTY;
	}
	return test_w1_types[slot];
}

/* Mirror the real per-type encode (app_w1_slots.c dallas_encode/machine_probe_
 * encode) so the composer test exercises the same SensorReading shaping. */
void app_w1_slot_encode(int slot, const struct app_w1_slot_reading *r, SensorReading *sr)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT || r == NULL || sr == NULL) {
		return;
	}
	if (!isnan(r->temperature)) {
		sr->has_temperature = true;
		sr->temperature = (int32_t)(r->temperature * 100.0f);
	}
	if (test_w1_types[slot] != APP_W1_SLOT_MACHINE_PROBE) {
		return;
	}
	if (!isnan(r->humidity)) {
		sr->has_humidity = true;
		sr->humidity = (uint32_t)(r->humidity * 2.0f);
	}
	sr->has_flags = true;
	sr->flags = r->is_tilt_alert ? MP_FLAG_TILT : 0;
	if (!isnan(r->illuminance)) {
		sr->has_illuminance = true;
		sr->illuminance = (uint32_t)r->illuminance;
	}
	if (!isnan(r->magnetic_field)) {
		sr->has_magnetic_field = true;
		sr->magnetic_field = (int32_t)(r->magnetic_field * 1000.0f);
	}
	if (!isnan(r->accel_x) && !isnan(r->accel_y) && !isnan(r->accel_z)) {
		sr->has_accel_x = true;
		sr->accel_x = (int32_t)(r->accel_x * 100.0f);
		sr->has_accel_y = true;
		sr->accel_y = (int32_t)(r->accel_y * 100.0f);
		sr->has_accel_z = true;
		sr->accel_z = (int32_t)(r->accel_z * 100.0f);
	}
}

uint8_t app_lrw_get_max_payload(void)
{
	return test_budget;
}

int app_hall_get_data_and_clear_notify(struct app_hall_data *data)
{
	*data = test_hall;
	return 0;
}

int app_input_get_data_and_clear_notify(struct app_input_data *data)
{
	*data = test_input;
	return 0;
}
