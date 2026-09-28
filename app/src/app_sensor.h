/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SENSOR_H_
#define APP_SENSOR_H_

/* Zephyr includes */
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

/* Application includes */
#include "app_sensor_types.h"
#include "app_w1_slots.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One channel value (#430). Counter channels (APP_SENSOR_F_COUNTER) use `u` —
 * an exact uint32 that a float would round above 2^24 — every other channel
 * uses `f` in the channel's physical unit (app_w1_slots.yaml), NaN when absent,
 * state channels as 0.0f / 1.0f. */
union app_sensor_value {
	float f;
	uint32_t u;
};

/* Slot 0: the motherboard channel vector, indexed by APP_SENSOR_CH_MOTHERBOARD_*.
 * Bit ch of `valid` is set when channel ch holds a value (always consistent with
 * the NaN of a float channel). */
struct app_sensor_mb {
	uint32_t valid;
	union app_sensor_value v[APP_SENSOR_CH_MOTHERBOARD_COUNT];
};

/* Slots 1..4: one 1-Wire slot. `type` is the registry type id of the bound
 * device (APP_SENSOR_TYPE_NONE when unbound); `v` is indexed by that type's
 * channel numbers. Channels the type does not provide are NaN. present=false
 * when the bound ROM was not seen on the last scan. */
struct app_sensor_w1 {
	uint8_t type;
	bool present;
	uint32_t valid;
	union app_sensor_value v[APP_SENSOR_W1_CH_MAX];
};

struct app_sensor_data {
	struct app_sensor_mb mb;
	struct app_sensor_w1 w1[APP_W1_SLOT_COUNT];
};

/* Motherboard channel by name: APP_SENSOR_MB_F(d, TEMPERATURE) (float view),
 * APP_SENSOR_MB_U(d, HALL_LEFT_COUNT) (counter view). */
#define APP_SENSOR_MB_F(d, NAME) ((d)->mb.v[APP_SENSOR_CH_MOTHERBOARD_##NAME].f)
#define APP_SENSOR_MB_U(d, NAME) ((d)->mb.v[APP_SENSOR_CH_MOTHERBOARD_##NAME].u)

/* Reset a 1-Wire slot vector to "no values" for a slot bound to `type`. */
void app_sensor_w1_clear(struct app_sensor_w1 *s, uint8_t type);

/* Store a float reading into channel `ch` of type `type`. A NaN, a non-finite
 * value or one outside the channel's registry range stores NaN and clears the
 * valid bit. */
void app_sensor_put_f(uint8_t type, union app_sensor_value *v, uint32_t *valid, uint8_t ch,
		      float value);

/* Transitional (#430 step 2): a 1-Wire slot value addressed by its
 * machine-probe channel number, for the quantity-based readers (alarm rules,
 * history, ATS) until they address channels by the slot's own type (steps
 * 4-6). A dallas slot provides only the temperature (ch 0 in both types); every
 * other channel reads NaN. */
float app_sensor_w1_f(const struct app_sensor_w1 *s, uint8_t mp_ch);

extern struct app_sensor_data g_app_sensor_data;
extern struct k_mutex g_app_sensor_data_lock;

int app_sensor_init(void);
void app_sensor_sample(void);

/* Stop the periodic sample timer ahead of a deep-sleep poweroff. */
void app_sensor_suspend(void);

/* True if the I2C bus is currently wedged (the latest sensor sweep saw every
 * I2C read fail). Cleared on the next good sweep or after bus recovery. */
bool app_sensor_i2c_wedged(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SENSOR_H_ */
