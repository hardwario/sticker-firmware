/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_compose.h"
#include "app_hall.h"
#include "app_input.h"
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

	uint32_t header = boot ? BIT(31) : 0;

	boot = false;

	/* For compatibility reasons (indicates header extension from 16 bits to 32 bits) */
	header |= BIT(20);

	uint8_t orientation = 0xff;
	uint8_t voltage = 0xff;

	int16_t temperature = 0x7fff;
	uint8_t humidity = 0xff;
	uint16_t illuminance = 0xffff;
	int16_t altitude = 0x7fff;
	uint32_t pressure = 0xffffffff;

	struct app_hall_data hall_data;
	uint32_t hall_left_count = 0;
	uint32_t hall_right_count = 0;

	struct app_input_data input_data;
	uint32_t input_a_count = 0;
	uint32_t input_b_count = 0;

	uint32_t motion_count = 0xffffffff;
	int16_t t1_temperature = 0x7fff;
	int16_t t2_temperature = 0x7fff;

	int16_t mp1_temperature = 0x7fff;
	int16_t mp2_temperature = 0x7fff;
	uint8_t mp1_humidity = 0xff;
	uint8_t mp2_humidity = 0xff;

	app_hall_get_data(&hall_data);
	app_input_get_data(&input_data);

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);

	if (g_app_sensor_data.orientation != INT_MAX) {
		orientation = g_app_sensor_data.orientation & 0xf;
		header |= BIT(30);
	}

	if (!isnan(g_app_sensor_data.voltage)) {
		voltage = (uint8_t)(g_app_sensor_data.voltage * 50);
		header |= BIT(29);
	}

	if (!isnan(g_app_sensor_data.temperature)) {
		temperature = (int16_t)(g_app_sensor_data.temperature * 100);
		header |= BIT(28);
	}

	if (!isnan(g_app_sensor_data.humidity)) {
		humidity = (uint8_t)(g_app_sensor_data.humidity * 2);
		header |= BIT(27);
	}

	if (!isnan(g_app_sensor_data.illuminance)) {
		illuminance = (uint16_t)(g_app_sensor_data.illuminance / 2);
		header |= BIT(26);
	}

	if (!isnan(g_app_sensor_data.t1_temperature)) {
		t1_temperature = (int16_t)(g_app_sensor_data.t1_temperature * 100);
		header |= BIT(25);
	}

	if (!isnan(g_app_sensor_data.t2_temperature)) {
		t2_temperature = (int16_t)(g_app_sensor_data.t2_temperature * 100);
		header |= BIT(24);
	}

	motion_count = g_app_sensor_data.motion_count;
	if (motion_count > 0) {
		header |= BIT(23);
	}

	if (!isnan(g_app_sensor_data.altitude)) {
		altitude = (int16_t)(g_app_sensor_data.altitude * 10);
		header |= BIT(22);
	}

	if (!isnan(g_app_sensor_data.pressure)) {
		pressure = (uint32_t)g_app_sensor_data.pressure;
		header |= BIT(21);
	}

	if (!isnan(g_app_sensor_data.mp1_temperature)) {
		mp1_temperature = (int16_t)(g_app_sensor_data.mp1_temperature * 100);
		header |= BIT(19);
	}

	if (!isnan(g_app_sensor_data.mp2_temperature)) {
		mp2_temperature = (int16_t)(g_app_sensor_data.mp2_temperature * 100);
		header |= BIT(18);
	}

	if (!isnan(g_app_sensor_data.mp1_humidity)) {
		mp1_humidity = (uint8_t)(g_app_sensor_data.mp1_humidity * 2);
		header |= BIT(17);
	}

	if (!isnan(g_app_sensor_data.mp2_humidity)) {
		mp2_humidity = (uint8_t)(g_app_sensor_data.mp2_humidity * 2);
		header |= BIT(16);
	}

	if (g_app_sensor_data.mp1_is_tilt_alert) {
		header |= BIT(15);
	}

	if (g_app_sensor_data.mp2_is_tilt_alert) {
		header |= BIT(14);
	}

	hall_left_count = g_app_sensor_data.hall_left_count;
	hall_right_count = g_app_sensor_data.hall_right_count;

	if (hall_left_count > 0) {
		header |= BIT(13);
	}

	if (hall_right_count > 0) {
		header |= BIT(12);
	}

	input_a_count = g_app_sensor_data.input_a_count;
	input_b_count = g_app_sensor_data.input_b_count;

	if (input_a_count > 0 || input_data.input_a_notify_act || input_data.input_a_notify_deact) {
		header |= BIT(5);
	}

	if (input_b_count > 0 || input_data.input_b_notify_act || input_data.input_b_notify_deact) {
		header |= BIT(4);
	}

	k_mutex_unlock(&g_app_sensor_data_lock);

	/* Add hall notify events to header */
	if (hall_data.left_notify_act) {
		header |= BIT(11);
	}
	if (hall_data.left_notify_deact) {
		header |= BIT(10);
	}
	if (hall_data.right_notify_act) {
		header |= BIT(9);
	}
	if (hall_data.right_notify_deact) {
		header |= BIT(8);
	}

	/* Add hall active states to header */
	if (hall_data.left_is_active) {
		header |= BIT(7);
	}
	if (hall_data.right_is_active) {
		header |= BIT(6);
	}

	/* Clear notify flags after reading them */
	app_hall_clear_notify_flags(&hall_data);
	app_input_clear_notify_flags(&input_data);

	header |= orientation;

	LOG_DBG("Header: 0x%08x", header);

#define APPEND_BYTE(exp)                                                                           \
	if (*len >= size) {                                                                        \
		LOG_ERR("Not enough space in provided buffer");                                    \
		return -ENOSPC;                                                                    \
	} else {                                                                                   \
		buf[(*len)++] = exp;                                                               \
	}

	*len = 0;

	memset(buf, 0, size);

	APPEND_BYTE(header >> 24);
	APPEND_BYTE(header >> 16);
	APPEND_BYTE(header >> 8);
	APPEND_BYTE(header);

	if (header & BIT(29)) {
		APPEND_BYTE(voltage);
	}

	if (header & BIT(28)) {
		APPEND_BYTE(temperature >> 8);
		APPEND_BYTE(temperature);
	}

	if (header & BIT(27)) {
		APPEND_BYTE(humidity);
	}

	if (header & BIT(26)) {
		APPEND_BYTE(illuminance >> 8);
		APPEND_BYTE(illuminance);
	}

	if (header & BIT(25)) {
		APPEND_BYTE(t1_temperature >> 8);
		APPEND_BYTE(t1_temperature);
	}

	if (header & BIT(24)) {
		APPEND_BYTE(t2_temperature >> 8);
		APPEND_BYTE(t2_temperature);
	}

	if (header & BIT(23)) {
		APPEND_BYTE(motion_count >> 24);
		APPEND_BYTE(motion_count >> 16);
		APPEND_BYTE(motion_count >> 8);
		APPEND_BYTE(motion_count);
	}

	if (header & BIT(22)) {
		APPEND_BYTE(altitude >> 8);
		APPEND_BYTE(altitude);
	}

	if (header & BIT(21)) {
		APPEND_BYTE(pressure >> 24);
		APPEND_BYTE(pressure >> 16);
		APPEND_BYTE(pressure >> 8);
		APPEND_BYTE(pressure);
	}

	if (header & BIT(19)) {
		APPEND_BYTE(mp1_temperature >> 8);
		APPEND_BYTE(mp1_temperature);
	}

	if (header & BIT(18)) {
		APPEND_BYTE(mp2_temperature >> 8);
		APPEND_BYTE(mp2_temperature);
	}

	if (header & BIT(17)) {
		APPEND_BYTE(mp1_humidity);
	}

	if (header & BIT(16)) {
		APPEND_BYTE(mp2_humidity);
	}

	if (header & BIT(13)) {
		APPEND_BYTE(hall_left_count >> 24);
		APPEND_BYTE(hall_left_count >> 16);
		APPEND_BYTE(hall_left_count >> 8);
		APPEND_BYTE(hall_left_count);
	}

	if (header & BIT(12)) {
		APPEND_BYTE(hall_right_count >> 24);
		APPEND_BYTE(hall_right_count >> 16);
		APPEND_BYTE(hall_right_count >> 8);
		APPEND_BYTE(hall_right_count);
	}

	if (header & BIT(5)) {
		APPEND_BYTE(input_a_count >> 24);
		APPEND_BYTE(input_a_count >> 16);
		APPEND_BYTE(input_a_count >> 8);
		APPEND_BYTE(input_a_count);
		/* Append status byte: bit 3=notify_act, bit 2=notify_deact, bit 1=reserved, bit
		 * 0=is_active */
		uint8_t status_a = 0;
		if (input_data.input_a_notify_act) {
			status_a |= BIT(3);
		}
		if (input_data.input_a_notify_deact) {
			status_a |= BIT(2);
		}
		if (input_data.input_a_is_active) {
			status_a |= BIT(0);
		}
		APPEND_BYTE(status_a);
	}

	if (header & BIT(4)) {
		APPEND_BYTE(input_b_count >> 24);
		APPEND_BYTE(input_b_count >> 16);
		APPEND_BYTE(input_b_count >> 8);
		APPEND_BYTE(input_b_count);
		/* Append status byte: bit 3=notify_act, bit 2=notify_deact, bit 1=reserved, bit
		 * 0=is_active */
		uint8_t status_b = 0;
		if (input_data.input_b_notify_act) {
			status_b |= BIT(3);
		}
		if (input_data.input_b_notify_deact) {
			status_b |= BIT(2);
		}
		if (input_data.input_b_is_active) {
			status_b |= BIT(0);
		}
		APPEND_BYTE(status_b);
	}

#undef APPEND

	LOG_HEXDUMP_DBG(buf, *len, "Composed buffer:");

	return 0;
}
