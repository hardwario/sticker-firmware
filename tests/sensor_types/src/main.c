/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the generated sensor type registry (app_sensor_types.c,
 * `west sensorgen` from app_w1_slots.yaml, #430): lookups by id / family /
 * name, channel descriptors and the capability gates.
 */

#include "app_config.h"
#include "app_sensor_types.h"

#include <stddef.h>

#include <zephyr/ztest.h>

ZTEST(sensor_types, test_type_lookup)
{
	const struct app_sensor_type *mb = app_sensor_type_get(APP_SENSOR_TYPE_MOTHERBOARD);

	zassert_not_null(mb);
	zassert_str_equal(mb->name, "motherboard");
	zassert_equal(mb->id, 1);
	zassert_equal(mb->w1_family, 0);
	zassert_equal(mb->channel_count, APP_SENSOR_CH_MOTHERBOARD_COUNT);

	zassert_equal(app_sensor_type_get(APP_SENSOR_TYPE_DALLAS)->id, 2);
	zassert_equal(app_sensor_type_get(APP_SENSOR_TYPE_MACHINE_PROBE)->id, 3);

	zassert_is_null(app_sensor_type_get(APP_SENSOR_TYPE_NONE));
	zassert_is_null(app_sensor_type_get(200));
}

ZTEST(sensor_types, test_type_by_family_and_name)
{
	zassert_equal(app_sensor_type_by_family(0x28)->id, APP_SENSOR_TYPE_DALLAS);
	zassert_equal(app_sensor_type_by_family(0x19)->id, APP_SENSOR_TYPE_MACHINE_PROBE);
	/* The motherboard has no family; 0 must not match it. */
	zassert_is_null(app_sensor_type_by_family(0));
	zassert_is_null(app_sensor_type_by_family(0x42));

	zassert_equal(app_sensor_type_by_name("machine-probe")->id, APP_SENSOR_TYPE_MACHINE_PROBE);
	zassert_is_null(app_sensor_type_by_name("nope"));
}

ZTEST(sensor_types, test_two_temperatures_on_one_probe)
{
	const struct app_sensor_channel *sht = app_sensor_channel_get(
		APP_SENSOR_TYPE_MACHINE_PROBE, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE);
	const struct app_sensor_channel *tmp = app_sensor_channel_get(
		APP_SENSOR_TYPE_MACHINE_PROBE, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX);

	zassert_not_null(sht);
	zassert_not_null(tmp);
	zassert_str_equal(tmp->name, "temperature-aux");
	zassert_equal(sht->kind, APP_SENSOR_KIND_THRESHOLD);
	zassert_equal(tmp->kind, APP_SENSOR_KIND_THRESHOLD);
	/* Only the primary temperature is the no-data liveness channel. */
	zassert_true(sht->flags & APP_SENSOR_F_LIVENESS);
	zassert_false(tmp->flags & APP_SENSOR_F_LIVENESS);
}

ZTEST(sensor_types, test_channel_by_name)
{
	zassert_equal(app_sensor_channel_by_name(APP_SENSOR_TYPE_MACHINE_PROBE, "temperature-aux"),
		      APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX);
	zassert_equal(app_sensor_channel_by_name(APP_SENSOR_TYPE_MOTHERBOARD, "hall-left-count"),
		      APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT);
	zassert_equal(app_sensor_channel_by_name(APP_SENSOR_TYPE_DALLAS, "humidity"), -1);
	zassert_equal(app_sensor_channel_by_name(200, "temperature"), -1);
}

ZTEST(sensor_types, test_channel_out_of_range)
{
	zassert_is_null(app_sensor_channel_get(APP_SENSOR_TYPE_DALLAS, 1));
	zassert_is_null(app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
					       APP_SENSOR_CH_MOTHERBOARD_COUNT));
	zassert_is_null(app_sensor_channel_get(APP_SENSOR_TYPE_NONE, 0));
}

ZTEST(sensor_types, test_descriptor_fields)
{
	const struct app_sensor_channel *c;

	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD, APP_SENSOR_CH_MOTHERBOARD_HUMIDITY);
	zassert_equal(c->wire_type, APP_SENSOR_WIRE_UINT32);
	zassert_equal(c->wire_scale, 2.0f);
	zassert_equal(c->hist_enc, APP_SENSOR_HIST_U8);
	zassert_true(c->flags & APP_SENSOR_F_RANGE);
	zassert_equal(c->range_min, 0.0f);
	zassert_equal(c->range_max, 100.0f);

	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
				   APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION);
	zassert_equal(c->kind, APP_SENSOR_KIND_STATE);
	zassert_true(c->flags & APP_SENSOR_F_MOMENTARY);
	zassert_equal(c->hist_enc, APP_SENSOR_HIST_NONE);

	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
				   APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_COUNT);
	zassert_equal(c->kind, APP_SENSOR_KIND_RATE);
	zassert_true(c->flags & APP_SENSOR_F_COUNTER);
	zassert_false(c->flags & APP_SENSOR_F_RANGE);

	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
				   APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE);
	zassert_true(c->flags & APP_SENSOR_F_WATCHDOG_ONLY);

	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
				   APP_SENSOR_CH_MOTHERBOARD_ACCEL_ORIENTATION);
	zassert_equal(c->kind, APP_SENSOR_KIND_NONE);
}

ZTEST(sensor_types, test_capability_gates)
{
	const struct app_sensor_channel *c;

	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
				   APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE);
	zassert_equal(c->cap_off, offsetof(struct app_config, cap_hall_left));

	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD, APP_SENSOR_CH_MOTHERBOARD_PRESSURE);
	zassert_equal(c->cap_off, offsetof(struct app_config, cap_barometer));

	/* Always-on channel and 1-Wire channels carry no gate. */
	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
				   APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE);
	zassert_equal(c->cap_off, APP_SENSOR_NO_CAP);
	c = app_sensor_channel_get(APP_SENSOR_TYPE_DALLAS, APP_SENSOR_CH_DALLAS_TEMPERATURE);
	zassert_equal(c->cap_off, APP_SENSOR_NO_CAP);
}

ZTEST(sensor_types, test_every_channel_is_consistent)
{
	for (uint8_t id = 1; id < 255; id++) {
		const struct app_sensor_type *t = app_sensor_type_get(id);

		if (t == NULL) {
			continue;
		}
		zassert_true(t->channel_count <=
			     (t->w1_family ? APP_SENSOR_W1_CH_MAX : APP_SENSOR_MB_CH_MAX));
		for (uint8_t ch = 0; ch < t->channel_count; ch++) {
			const struct app_sensor_channel *c = &t->channels[ch];

			zassert_not_null(c->name, "%s ch %u", t->name, ch);
			zassert_true(c->wire_scale > 0.0f, "%s/%s", t->name, c->name);
			zassert_equal(app_sensor_channel_by_name(id, c->name), ch);
			if (c->kind == APP_SENSOR_KIND_THRESHOLD) {
				zassert_true(c->flags & APP_SENSOR_F_RANGE, "%s/%s", t->name,
					     c->name);
			}
		}
	}
}

ZTEST_SUITE(sensor_types, NULL, NULL, NULL, NULL, NULL);
