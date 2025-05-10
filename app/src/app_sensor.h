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
	float ext_temperature_1;
	float ext_temperature_2;

#if defined(CONFIG_APP_PROFILE_STICKER_MOTION)
	uint32_t motion_count;
#endif /* defined(CONFIG_APP_PROFILE_STICKER_MOTION) */

	float altitude;
	float pressure;
};

extern struct app_sensor_data g_app_sensor_data;
extern struct k_mutex g_app_sensor_data_lock;

void app_sensor_sample(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SENSOR_H_ */
