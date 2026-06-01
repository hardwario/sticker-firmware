/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_compose.h"
#include "app_config.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_lrw.h"
#include "app_sensor.h"

/* Nanopb includes */
#include <pb_encode.h>
#include "src/nfc_config.pb.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_compose, LOG_LEVEL_DBG);

/* Telemetry.flags bit positions (see nfc_config.proto + ttn.js). */
#define FLAG_BOOT               BIT(0)
#define FLAG_MP1_TILT           BIT(1)
#define FLAG_MP2_TILT           BIT(2)
#define FLAG_HALL_L_NOTIFY_ACT  BIT(3)
#define FLAG_HALL_L_NOTIFY_DEACT BIT(4)
#define FLAG_HALL_L_ACTIVE      BIT(5)
#define FLAG_HALL_R_NOTIFY_ACT  BIT(6)
#define FLAG_HALL_R_NOTIFY_DEACT BIT(7)
#define FLAG_HALL_R_ACTIVE      BIT(8)
#define FLAG_INPUT_A_NOTIFY_ACT BIT(9)
#define FLAG_INPUT_A_NOTIFY_DEACT BIT(10)
#define FLAG_INPUT_A_ACTIVE     BIT(11)
#define FLAG_INPUT_B_NOTIFY_ACT BIT(12)
#define FLAG_INPUT_B_NOTIFY_DEACT BIT(13)
#define FLAG_INPUT_B_ACTIVE     BIT(14)

/*
 * Build a protobuf Telemetry message from the current sensor data and encode it
 * into `buf`, dropping low-priority fields until it fits the LoRaWAN payload
 * budget reported by the stack. Sent on fPort 2 (the legacy bitmap used fPort 1).
 *
 * Returns 0 on success (*len = encoded length), -EAGAIN when the budget is not
 * yet known (pre-join), or -EMSGSIZE if even the core fields don't fit / encode
 * fails.
 */
int app_compose(uint8_t *buf, size_t size, size_t *len)
{
	static bool boot = true;

	uint8_t budget = app_lrw_get_max_payload();
	if (budget == 0) {
		return -EAGAIN;
	}

	Telemetry t = Telemetry_init_zero;
	uint32_t flags = boot ? FLAG_BOOT : 0;

	/* Snapshot hall/input (clears notify edges) before taking the sensor lock. */
	struct app_hall_data hall;
	struct app_input_data input;
	app_hall_get_data_and_clear_notify(&hall);
	app_input_get_data_and_clear_notify(&input);

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	struct app_sensor_data d = g_app_sensor_data;
	k_mutex_unlock(&g_app_sensor_data_lock);

	/* --- always-present onboard channels --- */
	if (!isnan(d.voltage)) {
		float v = CLAMP(d.voltage * 50.0f, 0.0f, 255.0f);
		t.has_voltage = true;
		t.voltage = (uint32_t)v;
	}
	if (!isnan(d.temperature)) {
		t.has_temperature = true;
		t.temperature = (int32_t)(d.temperature * 100.0f);
	}
	if (!isnan(d.humidity)) {
		t.has_humidity = true;
		t.humidity = (uint32_t)(d.humidity * 2.0f);
	}
	if (d.orientation != INT_MAX) {
		t.has_orientation = true;
		t.orientation = (uint32_t)(d.orientation & 0xf);
	}

	/* --- capability-gated analog channels --- */
	if (g_app_config.cap_light_sensor && !isnan(d.illuminance)) {
		t.has_illuminance = true;
		t.illuminance = (uint32_t)(d.illuminance / 2.0f);
	}
	if (g_app_config.cap_barometer && !isnan(d.pressure)) {
		t.has_pressure = true;
		t.pressure = (uint32_t)(d.pressure * 1000.0f);
	}
	if (g_app_config.cap_barometer && !isnan(d.altitude)) {
		float a = CLAMP(d.altitude * 10.0f, (float)INT16_MIN, (float)INT16_MAX);
		t.has_altitude = true;
		t.altitude = (int32_t)a;
	}
	if (g_app_config.cap_1w_thermometer && !isnan(d.t1_temperature)) {
		t.has_ext1_temperature = true;
		t.ext1_temperature = (int32_t)(d.t1_temperature * 100.0f);
	}
	if (g_app_config.cap_1w_thermometer && !isnan(d.t2_temperature)) {
		t.has_ext2_temperature = true;
		t.ext2_temperature = (int32_t)(d.t2_temperature * 100.0f);
	}
	if (g_app_config.cap_1w_machine_probe && !isnan(d.mp1_temperature)) {
		t.has_mp1_temperature = true;
		t.mp1_temperature = (int32_t)(d.mp1_temperature * 100.0f);
	}
	if (g_app_config.cap_1w_machine_probe && !isnan(d.mp2_temperature)) {
		t.has_mp2_temperature = true;
		t.mp2_temperature = (int32_t)(d.mp2_temperature * 100.0f);
	}
	if (g_app_config.cap_1w_machine_probe && !isnan(d.mp1_humidity)) {
		t.has_mp1_humidity = true;
		t.mp1_humidity = (uint32_t)(d.mp1_humidity * 2.0f);
	}
	if (g_app_config.cap_1w_machine_probe && !isnan(d.mp2_humidity)) {
		t.has_mp2_humidity = true;
		t.mp2_humidity = (uint32_t)(d.mp2_humidity * 2.0f);
	}

	/* --- counters (capability-gated, sent when non-zero) --- */
	if (g_app_config.cap_pir_detector && d.motion_count > 0) {
		t.has_motion_count = true;
		t.motion_count = d.motion_count;
	}
	if (g_app_config.cap_hall_left && hall.left_count > 0) {
		t.has_hall_left_count = true;
		t.hall_left_count = hall.left_count;
	}
	if (g_app_config.cap_hall_right && hall.right_count > 0) {
		t.has_hall_right_count = true;
		t.hall_right_count = hall.right_count;
	}
	if (g_app_config.cap_input_a && input.input_a_count > 0) {
		t.has_input_a_count = true;
		t.input_a_count = input.input_a_count;
	}
	if (g_app_config.cap_input_b && input.input_b_count > 0) {
		t.has_input_b_count = true;
		t.input_b_count = input.input_b_count;
	}

	/* --- boolean state into the flags bitfield --- */
	if (g_app_config.cap_1w_machine_probe) {
		if (d.mp1_is_tilt_alert) {
			flags |= FLAG_MP1_TILT;
		}
		if (d.mp2_is_tilt_alert) {
			flags |= FLAG_MP2_TILT;
		}
	}
	if (g_app_config.cap_hall_left) {
		if (hall.left_notify_act) {
			flags |= FLAG_HALL_L_NOTIFY_ACT;
		}
		if (hall.left_notify_deact) {
			flags |= FLAG_HALL_L_NOTIFY_DEACT;
		}
		if (hall.left_is_active) {
			flags |= FLAG_HALL_L_ACTIVE;
		}
	}
	if (g_app_config.cap_hall_right) {
		if (hall.right_notify_act) {
			flags |= FLAG_HALL_R_NOTIFY_ACT;
		}
		if (hall.right_notify_deact) {
			flags |= FLAG_HALL_R_NOTIFY_DEACT;
		}
		if (hall.right_is_active) {
			flags |= FLAG_HALL_R_ACTIVE;
		}
	}
	if (g_app_config.cap_input_a) {
		if (input.input_a_notify_act) {
			flags |= FLAG_INPUT_A_NOTIFY_ACT;
		}
		if (input.input_a_notify_deact) {
			flags |= FLAG_INPUT_A_NOTIFY_DEACT;
		}
		if (input.input_a_is_active) {
			flags |= FLAG_INPUT_A_ACTIVE;
		}
	}
	if (g_app_config.cap_input_b) {
		if (input.input_b_notify_act) {
			flags |= FLAG_INPUT_B_NOTIFY_ACT;
		}
		if (input.input_b_notify_deact) {
			flags |= FLAG_INPUT_B_NOTIFY_DEACT;
		}
		if (input.input_b_is_active) {
			flags |= FLAG_INPUT_B_ACTIVE;
		}
	}
	if (flags) {
		t.has_flags = true;
		t.flags = flags;
	}

	/* Priority-ordered drop list (lowest priority first). Core voltage/
	 * temperature/humidity are never dropped. When the message exceeds the
	 * budget, clear the lowest-priority set fields until it fits. */
	bool *drop[] = {
		&t.has_input_b_count, &t.has_input_a_count,
		&t.has_hall_right_count, &t.has_hall_left_count,
		&t.has_motion_count, &t.has_orientation,
		&t.has_mp2_humidity, &t.has_mp2_temperature,
		&t.has_mp1_humidity, &t.has_mp1_temperature,
		&t.has_ext2_temperature, &t.has_ext1_temperature,
		&t.has_illuminance, &t.has_altitude, &t.has_pressure,
		&t.has_flags,
	};

	size_t cap = MIN(size, (size_t)budget);
	size_t needed = 0;
	uint8_t dropped = 0;

	pb_get_encoded_size(&needed, Telemetry_fields, &t);
	for (size_t i = 0; needed > cap && i < ARRAY_SIZE(drop); i++) {
		if (*drop[i]) {
			*drop[i] = false;
			dropped++;
			pb_get_encoded_size(&needed, Telemetry_fields, &t);
		}
	}

	pb_ostream_t os = pb_ostream_from_buffer(buf, cap);
	if (!pb_encode(&os, Telemetry_fields, &t)) {
		LOG_ERR("pb_encode failed: %s", PB_GET_ERROR(&os));
		return -EMSGSIZE;
	}

	*len = os.bytes_written;
	boot = false;

	LOG_INF("TX: DR budget=%uB (from system), payload=%zuB, %u field(s) dropped", budget,
		*len, dropped);
	LOG_HEXDUMP_DBG(buf, *len, "Telemetry:");

	return 0;
}
