/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SENSOR_H_
#define APP_SENSOR_H_

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

/* Standard includes */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Application includes */
#include "app_config.h"
#include "app_sensor_types.h"
#include "app_w1_slots.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GP_A / GP_B pin use (#498), read from the runtime g_app_config. After
 * app_sensor_init() a pin mode other than none implies cap_gpio_mode == inputs
 * (the init clears both pin modes otherwise), so callers check one field. */
static inline bool app_sensor_gpio_pir(void)
{
	return g_app_config.cap_gpio_mode == APP_CONFIG_GPIO_MODE_PIR;
}

static inline bool app_sensor_gpio_buzzer(void)
{
	return g_app_config.cap_gpio_mode == APP_CONFIG_GPIO_MODE_BUZZER;
}

/* Pin read by app_input: a digital input or an S0 pulse counter. */
static inline bool app_sensor_gpio_is_input(enum app_config_gpio_input mode)
{
	return mode == APP_CONFIG_GPIO_INPUT_DIGITAL || mode == APP_CONFIG_GPIO_INPUT_S0;
}

/* One channel value (#430). Counter channels (APP_SENSOR_F_COUNTER) use `u` —
 * an exact uint32 that a float would round above 2^24 — every other channel
 * uses `f` in the channel's physical unit (app_sensor_types.yaml), NaN when absent,
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

/* #485 S2: a failed onboard read keeps the previous value for this many sweeps
 * before its channels go NaN, so one transient I2C error neither raises a
 * no_data alarm nor flaps the threshold rules for a whole sample interval. */
#define APP_SENSOR_MISS_HOLD 1

/* Track one onboard sensor's consecutive failed reads in `*miss`. Returns
 * `mask` (its channels, to keep from the previous sweep) while the streak is
 * within APP_SENSOR_MISS_HOLD, else 0. A successful read clears the streak. */
uint32_t app_sensor_miss_hold(uint8_t *miss, bool failed, uint32_t mask);

/* Copy the channels in `keep` (value and valid bit) from `prev` into `mb`. */
void app_sensor_mb_keep(struct app_sensor_mb *mb, const struct app_sensor_mb *prev, uint32_t keep);

/* Value of channel `c` as sent on the wire (telemetry SensorReading.value):
 * round(phys * wire scale), or the raw integer of a counter channel. */
int32_t app_sensor_wire_value(const struct app_sensor_channel *c, union app_sensor_value v);

/* Transitional (#430 step 2): a 1-Wire slot value addressed by its
 * machine-probe channel number, for the quantity-based readers (alarm rules,
 * history, ATS) until they address channels by the slot's own type (steps
 * 4-6). A dallas slot provides only the temperature (ch 0 in both types); every
 * other channel reads NaN. */
float app_sensor_w1_f(const struct app_sensor_w1 *s, uint8_t mp_ch);

/* Fetch one sample from a Zephyr sensor device and read `n` channels into `out`
 * as floats. Shared fetch/channel-get/convert boilerplate for the onboard I2C
 * sensors (SHT4x, OPT3001, MPL3115A2) so each driver only keeps its own
 * channel list, logging and range checks (#220.C). Returns 0, -ENODEV if the
 * device is not ready, or the first failing sensor API error. */
int app_sensor_read_channels(const struct device *dev, const enum sensor_channel *chans, float *out,
			     size_t n);

extern struct app_sensor_data g_app_sensor_data;
extern struct k_mutex g_app_sensor_data_lock;

int app_sensor_init(void);
void app_sensor_sample(void);

/* Queue one full sample on the sensor work queue now, e.g. after a live 1-Wire
 * slot change, so the no-data watchdog sees the new binding's reading instead
 * of the empty value left until the next sample tick. */
void app_sensor_sample_async(void);

/* Rescan the 1-Wire bus on the sensor work queue (#485 S4), serialized with
 * sampling. Without `force` it is a bare ROM search, and the full scan + rebind
 * runs only if the bus changed since the last rebind; `force` (a host W1Scan)
 * always rebinds and restarts the 2/5/10/15 min backoff that runs while a
 * taught slot is unbound. No-op without cap-w1-sensors or a 1-Wire build. */
void app_sensor_w1_rescan(bool force);

/* Stop the periodic sample timer ahead of a deep-sleep poweroff. */
void app_sensor_suspend(void);

/* True if the I2C bus is currently wedged (the latest sensor sweep saw every
 * I2C read fail). Cleared on the next good sweep or after bus recovery. */
bool app_sensor_i2c_wedged(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SENSOR_H_ */
