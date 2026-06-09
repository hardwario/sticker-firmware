/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stubs for app_compose's app-level dependencies. Tests set the test_* state.
 */

#include "app_hall.h"
#include "app_input.h"
#include "app_w1_slots.h"

#include <stdint.h>

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
