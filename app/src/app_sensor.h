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
#include "app_w1_slots.h"

#ifdef __cplusplus
extern "C" {
#endif

struct app_sensor_data {
	int orientation;
	float voltage;
	float temperature;
	float humidity;
	float illuminance;
	float altitude;
	float pressure;
	/* 1-Wire sensor slots, ROM-bound (see app_w1_slots). Each slot's reading
	 * carries temperature/humidity/tilt + present; unbound/absent → NaN/false. */
	struct app_w1_slot_reading w1[APP_W1_SLOT_COUNT];
	uint32_t hall_left_count;
	uint32_t hall_right_count;
	bool hall_left_is_active;
	bool hall_right_is_active;
	uint32_t input_a_count;
	uint32_t input_b_count;
	bool input_a_is_active;
	bool input_b_is_active;
	/* Analog voltage on GP_A/GP_B (#396), NaN when the capability is off or the
	 * pin is owned by the digital input / PIR / buzzer instead (app_sensor.c). */
	float input_a_voltage;
	float input_b_voltage;
	uint32_t motion_count;       /* PIR (PYQ1648) motion events */
	uint32_t accel_motion_count; /* accelerometer (LIS2DH) any-motion events */
};

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
