/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_compose.h"
#include "app_sensor.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_compose, LOG_LEVEL_DBG);

int app_compose(uint8_t *buf, size_t size, size_t *len)
{
	static bool boot = true;

	uint16_t header = boot ? BIT(15) : 0;

	boot = false;

	uint8_t voltage;
	uint8_t orientation;
	uint16_t temperature;
	uint8_t humidity;
	uint16_t illuminance;
	uint16_t ext_temperature_1;
	uint16_t ext_temperature_2;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	/* TODO Implement */
	if (1) {
		voltage = 0xff;
	} else {
		voltage = 0xff;
		header |= BIT(14);
	}

	if (g_app_sensor_data.orientation == INT_MAX) {
		orientation = 0xf;
	} else {
		orientation = g_app_sensor_data.orientation & 0xf;
		header |= BIT(13);
	}

	if (isnan(g_app_sensor_data.temperature)) {
		temperature = 0x7fff;
	} else {
		temperature = (uint16_t)(g_app_sensor_data.temperature * 100);
		header |= BIT(12);
	}

	if (isnan(g_app_sensor_data.humidity)) {
		humidity = 0xff;
	} else {
		humidity = (uint8_t)(g_app_sensor_data.humidity * 2);
		header |= BIT(11);
	}

	if (isnan(g_app_sensor_data.illuminance)) {
		illuminance = 0xffff;
	} else {
		illuminance = (uint16_t)(g_app_sensor_data.illuminance * 2);
		header |= BIT(10);
	}

	if (isnan(g_app_sensor_data.ext_temperature_1)) {
		ext_temperature_1 = 0x7fff;
	} else {
		ext_temperature_1 = (uint16_t)(g_app_sensor_data.ext_temperature_1 * 100);
		header |= BIT(9);
	}

	if (isnan(g_app_sensor_data.ext_temperature_2)) {
		ext_temperature_2 = 0x7fff;
	} else {
		ext_temperature_2 = (uint16_t)(g_app_sensor_data.ext_temperature_2 * 100);
		header |= BIT(8);
	}

	k_mutex_unlock(&g_app_sensor_data_lock);

	header |= orientation;

#define APPEND_BYTE(exp)                                                                           \
	if (*len >= size) {                                                                        \
		LOG_ERR("Not enough space in provided buffer");                                    \
		return -ENOSPC;                                                                    \
	} else {                                                                                   \
		buf[(*len)++] = exp;                                                               \
	}

	*len = 0;

	memset(buf, 0, size);

	APPEND_BYTE(header >> 8);
	APPEND_BYTE(header);
	APPEND_BYTE(voltage);
	APPEND_BYTE(temperature >> 8);
	APPEND_BYTE(temperature);
	APPEND_BYTE(humidity);
	APPEND_BYTE(illuminance >> 8);
	APPEND_BYTE(illuminance);
	APPEND_BYTE(ext_temperature_1 >> 8);
	APPEND_BYTE(ext_temperature_1);
	APPEND_BYTE(ext_temperature_2 >> 8);
	APPEND_BYTE(ext_temperature_2);

#undef APPEND

	LOG_HEXDUMP_DBG(buf, *len, "Composed buffer:");

	return 0;
}
