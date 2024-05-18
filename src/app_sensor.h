/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SENSOR_H_
#define APP_SENSOR_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

struct app_sensor_data {
	float temperature;
	float humidity;
	float illuminance;
	int orientation;
};

extern struct app_sensor_data g_app_sensor_data;
extern struct k_mutex g_app_sensor_data_lock;

#ifdef __cplusplus
}
#endif

#endif /* APP_SENSOR_H_ */
