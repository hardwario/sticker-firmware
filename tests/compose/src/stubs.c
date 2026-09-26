/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stubs for app_compose's app-level dependencies. Tests set the test_* state.
 */

#include "app_hall.h"
#include "app_input.h"
#include "app_sensor.h"
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
void app_w1_slot_encode(int slot, const struct app_sensor_w1 *r, SensorReading *sr)
{
#define MP(NAME) (r->v[APP_SENSOR_CH_MACHINE_PROBE_##NAME].f)
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT || r == NULL || sr == NULL) {
		return;
	}
	if (!isnan(MP(TEMPERATURE))) {
		sr->has_temperature = true;
		sr->temperature = (int32_t)(MP(TEMPERATURE) * 100.0f);
	}
	if (test_w1_types[slot] != APP_W1_SLOT_MACHINE_PROBE) {
		return;
	}
	if (!isnan(MP(HUMIDITY))) {
		sr->has_humidity = true;
		sr->humidity = (uint32_t)(MP(HUMIDITY) * 2.0f);
	}
	sr->has_flags = true;
	sr->flags = MP(TILT) == 1.0f ? MP_FLAG_TILT : 0;
	if (!isnan(MP(ILLUMINANCE))) {
		sr->has_illuminance = true;
		sr->illuminance = (uint32_t)MP(ILLUMINANCE);
	}
	if (!isnan(MP(MAGNETIC_FIELD))) {
		sr->has_magnetic_field = true;
		sr->magnetic_field = (int32_t)(MP(MAGNETIC_FIELD) * 1000.0f);
	}
	if (!isnan(MP(ACCEL_X)) && !isnan(MP(ACCEL_Y)) && !isnan(MP(ACCEL_Z))) {
		sr->has_accel_x = true;
		sr->accel_x = (int32_t)(MP(ACCEL_X) * 100.0f);
		sr->has_accel_y = true;
		sr->accel_y = (int32_t)(MP(ACCEL_Y) * 100.0f);
		sr->has_accel_z = true;
		sr->accel_z = (int32_t)(MP(ACCEL_Z) * 100.0f);
	}
#undef MP
}

uint8_t app_lrw_get_max_payload(void)
{
	return test_budget;
}

/* device_status alarm byte the composer mirrors into system_flags bits 1..8. */
uint32_t test_alarm_flags;

uint32_t app_alarm_status_flags(void)
{
	return test_alarm_flags;
}

int app_hall_get_data(struct app_hall_data *data)
{
	*data = test_hall;
	return 0;
}

int app_input_get_data(struct app_input_data *data)
{
	*data = test_input;
	return 0;
}
