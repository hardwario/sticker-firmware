/*
 * Copyright (c) 2025 HARDWARIO a.s.
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

	uint8_t orientation = 0xff;
	uint8_t voltage = 0xff;
	int16_t temperature = 0x7fff;
	uint8_t humidity = 0xff;
	uint16_t illuminance = 0xffff;
	int16_t ext_temperature_1 = 0x7fff;
	int16_t ext_temperature_2 = 0x7fff;

#if defined(CONFIG_APP_PROFILE_STICKER_MOTION)
	uint32_t motion_count = 0xffffffff;
#endif /* defined(CONFIG_APP_PROFILE_STICKER_MOTION) */

	int16_t altitude = 0x7fff;
	uint32_t pressure = 0xffffffff;

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	if (g_app_sensor_data.orientation != INT_MAX) {
		orientation = g_app_sensor_data.orientation & 0xf;
		header |= BIT(14);
	}

	if (!isnan(g_app_sensor_data.voltage)) {
		voltage = (uint8_t)(g_app_sensor_data.voltage * 50);
		header |= BIT(13);
	}

	if (!isnan(g_app_sensor_data.temperature)) {
		temperature = (int16_t)(g_app_sensor_data.temperature * 100);
		header |= BIT(12);
	}

	if (!isnan(g_app_sensor_data.humidity)) {
		humidity = (uint8_t)(g_app_sensor_data.humidity * 2);
		header |= BIT(11);
	}

	if (!isnan(g_app_sensor_data.illuminance)) {
		illuminance = (uint16_t)(g_app_sensor_data.illuminance / 2);
		header |= BIT(10);
	}

	if (!isnan(g_app_sensor_data.ext_temperature_1)) {
		ext_temperature_1 = (int16_t)(g_app_sensor_data.ext_temperature_1 * 100);
		header |= BIT(9);
	}

	if (!isnan(g_app_sensor_data.ext_temperature_2)) {
		ext_temperature_2 = (int16_t)(g_app_sensor_data.ext_temperature_2 * 100);
		header |= BIT(8);
	}

#if defined(CONFIG_APP_PROFILE_STICKER_MOTION)
	motion_count = g_app_sensor_data.motion_count;
	header |= BIT(7);
#endif /* defined(CONFIG_APP_PROFILE_STICKER_MOTION) */

	if (!isnan(g_app_sensor_data.altitude)) {
		altitude = (int16_t)(g_app_sensor_data.altitude * 10);
		header |= BIT(6);
	}

	if (!isnan(g_app_sensor_data.pressure)) {
		pressure = (uint32_t)g_app_sensor_data.pressure;
		header |= BIT(5);
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

	if (header & BIT(13)) {
		APPEND_BYTE(voltage);
	}

	if (header & BIT(12)) {
		APPEND_BYTE(temperature >> 8);
		APPEND_BYTE(temperature);
	}

	if (header & BIT(11)) {
		APPEND_BYTE(humidity);
	}

	if (header & BIT(10)) {
		APPEND_BYTE(illuminance >> 8);
		APPEND_BYTE(illuminance);
	}

	if (header & BIT(9)) {
		APPEND_BYTE(ext_temperature_1 >> 8);
		APPEND_BYTE(ext_temperature_1);
	}

	if (header & BIT(8)) {
		APPEND_BYTE(ext_temperature_2 >> 8);
		APPEND_BYTE(ext_temperature_2);
	}

#if defined(CONFIG_APP_PROFILE_STICKER_MOTION)
	if (header & BIT(7)) {
		APPEND_BYTE(motion_count >> 24);
		APPEND_BYTE(motion_count >> 16);
		APPEND_BYTE(motion_count >> 8);
		APPEND_BYTE(motion_count);
	}
#endif /* defined(CONFIG_APP_PROFILE_STICKER_MOTION) */

	if (header & BIT(6)) {
		APPEND_BYTE(altitude >> 8);
		APPEND_BYTE(altitude);
	}

	if (header & BIT(5)) {
		APPEND_BYTE(pressure >> 24);
		APPEND_BYTE(pressure >> 16);
		APPEND_BYTE(pressure >> 8);
		APPEND_BYTE(pressure);
	}

#undef APPEND

	LOG_HEXDUMP_DBG(buf, *len, "Composed buffer:");

	return 0;
}
