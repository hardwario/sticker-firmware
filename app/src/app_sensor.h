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
	float t1_temperature;
	float t2_temperature;
	float machine_probe_temperature_1;
	float machine_probe_temperature_2;
	float machine_probe_humidity_1;
	float machine_probe_humidity_2;
	bool machine_probe_is_tilt_alert_1;
	bool machine_probe_is_tilt_alert_2;
	uint32_t hall_left_count;
	uint32_t hall_right_count;
	bool hall_left_is_active;
	bool hall_right_is_active;
	uint32_t motion_count;
};

extern struct app_sensor_data g_app_sensor_data;
extern struct k_mutex g_app_sensor_data_lock;

void app_sensor_sample(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SENSOR_H_ */
