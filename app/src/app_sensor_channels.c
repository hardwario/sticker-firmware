/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Channel-vector helpers for struct app_sensor_data (#430). Kept apart from
 * app_sensor.c (the sampling loop and its drivers) so the alarm / compose /
 * history host tests can link them without the hardware layer. */

#include "app_sensor.h"
#include "app_sensor_types.h"

/* Zephyr includes */
#include <zephyr/sys/util.h>

/* Standard includes */
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* app_sensor_w1_f() addresses a dallas slot by the machine-probe numbering. */
BUILD_ASSERT((int)APP_SENSOR_CH_DALLAS_TEMPERATURE == (int)APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE,
	     "dallas and machine-probe primary temperature must share ch 0");

void app_sensor_w1_clear(struct app_sensor_w1 *s, uint8_t type)
{
	s->type = type;
	s->present = false;
	s->valid = 0;
	for (size_t ch = 0; ch < ARRAY_SIZE(s->v); ch++) {
		s->v[ch].f = NAN;
	}
}

void app_sensor_put_f(uint8_t type, union app_sensor_value *v, uint32_t *valid, uint8_t ch,
		      float value)
{
	const struct app_sensor_channel *c = app_sensor_channel_get(type, ch);

	if (c == NULL) {
		return;
	}
	if (!isfinite(value) ||
	    ((c->flags & APP_SENSOR_F_RANGE) && (value < c->range_min || value > c->range_max))) {
		v[ch].f = NAN;
		*valid &= ~BIT(ch);
		return;
	}
	v[ch].f = value;
	*valid |= BIT(ch);
}

float app_sensor_w1_f(const struct app_sensor_w1 *s, uint8_t mp_ch)
{
	if (s->type == APP_SENSOR_TYPE_MACHINE_PROBE && mp_ch < APP_SENSOR_CH_MACHINE_PROBE_COUNT) {
		return s->v[mp_ch].f;
	}
	if (s->type == APP_SENSOR_TYPE_DALLAS && mp_ch == APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE) {
		return s->v[APP_SENSOR_CH_DALLAS_TEMPERATURE].f;
	}
	return NAN;
}
