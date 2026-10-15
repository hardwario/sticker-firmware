/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the generated sensor type registry (app_sensor_types.c,
 * `west sensorgen` from app_sensor_types.yaml, #430): lookups by id / family /
 * name, channel descriptors and the capability gates.
 */

#include "app_config.h"
#include "app_sensor.h"
#include "app_sensor_types.h"

#include <math.h>
#include <stddef.h>

#include <zephyr/ztest.h>

/* app_sensor_channel_cap_on() reads the capability gates from here. */
struct app_config g_app_config;

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
	/* 1-Wire channels are watched per part (chip), not as liveness channels:
	 * the SHT and the TMP112 are separate parts of the probe. */
	zassert_false(sht->flags & APP_SENSOR_F_LIVENESS);
	zassert_false(tmp->flags & APP_SENSOR_F_LIVENESS);
	zassert_not_equal(sht->part, tmp->part);
	zassert_equal(sht->part, app_sensor_channel_get(APP_SENSOR_TYPE_MACHINE_PROBE,
							APP_SENSOR_CH_MACHINE_PROBE_HUMIDITY)
					 ->part);
	zassert_equal(app_sensor_type_get(APP_SENSOR_TYPE_MACHINE_PROBE)->part_count, 5);
	zassert_equal(app_sensor_type_get(APP_SENSOR_TYPE_DALLAS)->part_count, 1);
	zassert_equal(app_sensor_type_get(APP_SENSOR_TYPE_MOTHERBOARD)->part_count, 0);
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
	/* Momentary channels record a 0/1 "fired" column derived from their pulse
	 * counter (#430). */
	zassert_equal(c->hist_enc, APP_SENSOR_HIST_U8);
	zassert_equal(c->pulses_ch, APP_SENSOR_CH_MOTHERBOARD_PIR_COUNT);

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

	/* The onboard SHT4x is gated by cap_sht (#465). */
	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
				   APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE);
	zassert_equal(c->cap_off, offsetof(struct app_config, cap_sht));

	/* Always-on channel and 1-Wire channels carry no gate. */
	c = app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD,
				   APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE);
	zassert_equal(c->cap_off, APP_SENSOR_NO_CAP);
	c = app_sensor_channel_get(APP_SENSOR_TYPE_DALLAS, APP_SENSOR_CH_DALLAS_TEMPERATURE);
	zassert_equal(c->cap_off, APP_SENSOR_NO_CAP);
}

static bool mb_on(uint8_t ch)
{
	return app_sensor_channel_cap_on(app_sensor_channel_get(APP_SENSOR_TYPE_MOTHERBOARD, ch));
}

/* #498: the GP channels are gated by the enum caps cap_gpio_mode /
 * cap_gpio_input_a/_b — a channel is on for its enabling values only. */
ZTEST(sensor_types, test_enum_capability_gates)
{
	g_app_config = (struct app_config){0};
	g_app_config.cap_hall_left = true;
	zassert_true(mb_on(APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE), "bool cap on");
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_HALL_RIGHT_STATE), "bool cap off");
	zassert_true(mb_on(APP_SENSOR_CH_MOTHERBOARD_BATTERY_VOLTAGE), "no cap = always on");

	/* none: nothing on either pin */
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_STATE));
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_COUNT));
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_VOLTAGE));
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION));

	/* digital A: state + count; pulse B: count only (no state); */
	g_app_config.cap_gpio_mode = APP_CONFIG_GPIO_MODE_INPUTS;
	g_app_config.cap_gpio_input_a = APP_CONFIG_GPIO_INPUT_DIGITAL;
	g_app_config.cap_gpio_input_b = APP_CONFIG_GPIO_INPUT_PULSE;
	zassert_true(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_STATE));
	zassert_true(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_COUNT));
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_VOLTAGE));
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_B_STATE), "pulse has no state");
	zassert_true(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_B_COUNT));
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_B_VOLTAGE));
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_PIR_COUNT));

	/* analog A: voltage only */
	g_app_config.cap_gpio_input_a = APP_CONFIG_GPIO_INPUT_ANALOG;
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_STATE));
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_COUNT));
	zassert_true(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_A_VOLTAGE));

	/* pir: the PIR channels (the pin modes are cleared at init, #498) */
	g_app_config.cap_gpio_mode = APP_CONFIG_GPIO_MODE_PIR;
	g_app_config.cap_gpio_input_a = APP_CONFIG_GPIO_INPUT_DISABLED;
	g_app_config.cap_gpio_input_b = APP_CONFIG_GPIO_INPUT_DISABLED;
	zassert_true(mb_on(APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION));
	zassert_true(mb_on(APP_SENSOR_CH_MOTHERBOARD_PIR_COUNT));
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_INPUT_B_COUNT));

	/* buzzer: no sensor channel at all */
	g_app_config.cap_gpio_mode = APP_CONFIG_GPIO_MODE_BUZZER;
	zassert_false(mb_on(APP_SENSOR_CH_MOTHERBOARD_PIR_MOTION));
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

/* ---- channel-vector helpers (app_sensor_types.c) ---------------------- */

ZTEST(sensor_types, test_put_f_valid_and_range)
{
	struct app_sensor_mb mb = {.valid = 0};

	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, mb.v, &mb.valid,
			 APP_SENSOR_CH_MOTHERBOARD_HUMIDITY, 55.0f);
	zassert_equal(mb.v[APP_SENSOR_CH_MOTHERBOARD_HUMIDITY].f, 55.0f);
	zassert_true(mb.valid & BIT(APP_SENSOR_CH_MOTHERBOARD_HUMIDITY));

	/* Out of the registry range [0, 100] %RH -> absent. */
	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, mb.v, &mb.valid,
			 APP_SENSOR_CH_MOTHERBOARD_HUMIDITY, 120.0f);
	zassert_true(isnan(mb.v[APP_SENSOR_CH_MOTHERBOARD_HUMIDITY].f));
	zassert_false(mb.valid & BIT(APP_SENSOR_CH_MOTHERBOARD_HUMIDITY));

	/* NaN / inf -> absent. */
	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, mb.v, &mb.valid,
			 APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE, NAN);
	zassert_false(mb.valid & BIT(APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE));
	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, mb.v, &mb.valid,
			 APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE, INFINITY);
	zassert_true(isnan(mb.v[APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE].f));

	/* A channel without a range (orientation) takes any finite value. */
	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, mb.v, &mb.valid,
			 APP_SENSOR_CH_MOTHERBOARD_ACCEL_ORIENTATION, 6.0f);
	zassert_true(mb.valid & BIT(APP_SENSOR_CH_MOTHERBOARD_ACCEL_ORIENTATION));
}

/* #485 S2: one failed read keeps the previous value, the next one in a row
 * goes absent, and a good read clears the streak. */
ZTEST(sensor_types, test_miss_hold_streak)
{
	const uint32_t mask = BIT(APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE);
	uint8_t miss = 0;

	zassert_equal(app_sensor_miss_hold(&miss, false, mask), 0, "good read keeps nothing");
	zassert_equal(app_sensor_miss_hold(&miss, true, mask), mask, "1st miss holds");
	zassert_equal(app_sensor_miss_hold(&miss, true, mask), 0, "2nd miss in a row goes NaN");
	zassert_equal(app_sensor_miss_hold(&miss, true, mask), 0, "and stays NaN");
	zassert_equal(app_sensor_miss_hold(&miss, false, mask), 0, "recovery");
	zassert_equal(miss, 0, "a good read clears the streak");
	zassert_equal(app_sensor_miss_hold(&miss, true, mask), mask, "a new single miss holds");

	/* The streak saturates instead of wrapping back into the hold window. */
	miss = UINT8_MAX;
	zassert_equal(app_sensor_miss_hold(&miss, true, mask), 0, "saturated streak");
	zassert_equal(miss, UINT8_MAX);
}

ZTEST(sensor_types, test_mb_keep_copies_only_kept_channels)
{
	struct app_sensor_mb prev = {.valid = 0};
	struct app_sensor_mb mb = {.valid = 0};

	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, prev.v, &prev.valid,
			 APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE, 21.5f);
	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, prev.v, &prev.valid,
			 APP_SENSOR_CH_MOTHERBOARD_HUMIDITY, 40.0f);
	/* This sweep: temperature failed (NaN), humidity read fine. */
	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, mb.v, &mb.valid,
			 APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE, NAN);
	app_sensor_put_f(APP_SENSOR_TYPE_MOTHERBOARD, mb.v, &mb.valid,
			 APP_SENSOR_CH_MOTHERBOARD_HUMIDITY, 45.0f);

	app_sensor_mb_keep(&mb, &prev, BIT(APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE));
	zassert_equal(mb.v[APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE].f, 21.5f, "kept value");
	zassert_true(mb.valid & BIT(APP_SENSOR_CH_MOTHERBOARD_TEMPERATURE), "kept valid bit");
	zassert_equal(mb.v[APP_SENSOR_CH_MOTHERBOARD_HUMIDITY].f, 45.0f, "fresh value untouched");

	/* Keeping a channel that was absent before keeps it absent. */
	struct app_sensor_mb empty = {.valid = 0};

	empty.v[APP_SENSOR_CH_MOTHERBOARD_HUMIDITY].f = NAN;
	app_sensor_mb_keep(&mb, &empty, BIT(APP_SENSOR_CH_MOTHERBOARD_HUMIDITY));
	zassert_false(mb.valid & BIT(APP_SENSOR_CH_MOTHERBOARD_HUMIDITY));
	zassert_true(isnan(mb.v[APP_SENSOR_CH_MOTHERBOARD_HUMIDITY].f));
}

ZTEST(sensor_types, test_put_f_unknown_channel_is_ignored)
{
	struct app_sensor_w1 w;

	app_sensor_w1_clear(&w, APP_SENSOR_TYPE_DALLAS);
	/* Dallas has only ch 0; ch 1 must not be written. */
	app_sensor_put_f(APP_SENSOR_TYPE_DALLAS, w.v, &w.valid, 1, 42.0f);
	zassert_true(isnan(w.v[1].f));
	zassert_equal(w.valid, 0);
}

ZTEST(sensor_types, test_w1_clear)
{
	struct app_sensor_w1 w = {.type = 9, .present = true, .valid = 0xff};

	app_sensor_w1_clear(&w, APP_SENSOR_TYPE_MACHINE_PROBE);
	zassert_equal(w.type, APP_SENSOR_TYPE_MACHINE_PROBE);
	zassert_false(w.present);
	zassert_equal(w.valid, 0);
	for (int ch = 0; ch < APP_SENSOR_W1_CH_MAX; ch++) {
		zassert_true(isnan(w.v[ch].f), "ch %d", ch);
	}
}

ZTEST(sensor_types, test_w1_f_maps_dallas_onto_machine_probe_numbers)
{
	struct app_sensor_w1 w;

	app_sensor_w1_clear(&w, APP_SENSOR_TYPE_DALLAS);
	w.v[APP_SENSOR_CH_DALLAS_TEMPERATURE].f = 21.5f;
	w.v[1].f = 99.0f; /* stray value: dallas has no ch 1 */
	zassert_equal(app_sensor_w1_f(&w, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE), 21.5f);
	zassert_true(isnan(app_sensor_w1_f(&w, APP_SENSOR_CH_MACHINE_PROBE_HUMIDITY)));

	app_sensor_w1_clear(&w, APP_SENSOR_TYPE_MACHINE_PROBE);
	w.v[APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX].f = 19.25f;
	zassert_equal(app_sensor_w1_f(&w, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX), 19.25f);
	zassert_true(isnan(app_sensor_w1_f(&w, APP_SENSOR_CH_MACHINE_PROBE_COUNT)));

	app_sensor_w1_clear(&w, APP_SENSOR_TYPE_NONE);
	zassert_true(isnan(app_sensor_w1_f(&w, APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE)));
}

ZTEST_SUITE(sensor_types, NULL, NULL, NULL, NULL, NULL);
