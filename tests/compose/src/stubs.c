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

uint8_t test_budget = 200;
struct app_hall_data test_hall;
struct app_input_data test_input;

/* Per-slot expected type the composer reads to decide which slots emit a
 * SensorReading (0 = APP_W1_SLOT_EMPTY = no reading), and the slot state that
 * decides whether values are encoded (default 0 = treated as OK). Tests set
 * these. */
enum app_w1_slot_type test_w1_types[APP_W1_SLOT_COUNT];
enum app_w1_slot_state test_w1_states[APP_W1_SLOT_COUNT];

uint8_t app_w1_slot_get_expected_type(int slot)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT) {
		return APP_W1_SLOT_EMPTY;
	}
	return (uint8_t)test_w1_types[slot];
}

enum app_w1_slot_state app_w1_slot_get_state(int slot)
{
	if (slot < 0 || slot >= APP_W1_SLOT_COUNT) {
		return APP_W1_SLOT_STATE_NONE;
	}
	return test_w1_states[slot] != APP_W1_SLOT_STATE_NONE ? test_w1_states[slot]
							      : APP_W1_SLOT_STATE_OK;
}

uint8_t app_radio_get_max_payload(void)
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
