/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_compose.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_lrw.h"
#include "app_sensor.h"

/* Nanopb includes */
#include <pb_encode.h>
#include "src/app_config.pb.h"

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
#include <string.h>

LOG_MODULE_REGISTER(app_compose, LOG_LEVEL_DBG);

/* Per-group flag bit positions (mirrored in ttn.js). */
#define SYSTEM_FLAG_BOOT      BIT(0)
#define MP_FLAG_TILT          BIT(0)
#define CNT_FLAG_NOTIFY_ACT   BIT(0)
#define CNT_FLAG_NOTIFY_DEACT BIT(1)
#define CNT_FLAG_ACTIVE       BIT(2)

/* Sensor groups, in priority order (packed into frames first → last). A group
 * is the atomic unit: all its fields go into one frame, or none. */
enum tlm_group {
	G_INTERNAL = 0, /* temperature, humidity */
	G_SYSTEM,       /* voltage, system_flags */
	G_BAROMETER,    /* pressure, altitude */
	G_LIGHT,        /* illuminance */
	G_ACCEL,        /* orientation */
	G_PIR,          /* motion_count */
	G_EXT1,         /* ext1_temperature */
	G_EXT2,         /* ext2_temperature */
	G_MP1,          /* mp1_temperature, mp1_humidity, mp1_flags */
	G_MP2,          /* mp2_temperature, mp2_humidity, mp2_flags */
	G_HALL_L,       /* hall_left_count, hall_left_flags */
	G_HALL_R,       /* hall_right_count, hall_right_flags */
	G_INPUT_A,      /* input_a_count, input_a_flags */
	G_INPUT_B,      /* input_b_count, input_b_flags */
	G_COUNT,
};

/* Copy (on=true) or clear (on=false) a group's fields from src into dst. With a
 * frozen snapshot src this both selects a group into a frame and reverts it. */
static void apply_group(Telemetry *dst, const Telemetry *src, enum tlm_group g, bool on)
{
#define SEL(field)                                                                                 \
	do {                                                                                       \
		dst->has_##field = on && src->has_##field;                                         \
		if (on) {                                                                          \
			dst->field = src->field;                                                   \
		}                                                                                  \
	} while (0)

	switch (g) {
	case G_INTERNAL:
		SEL(temperature);
		SEL(humidity);
		break;
	case G_SYSTEM:
		SEL(voltage);
		SEL(system_flags);
		break;
	case G_BAROMETER:
		SEL(pressure);
		SEL(altitude);
		break;
	case G_LIGHT:
		SEL(illuminance);
		break;
	case G_ACCEL:
		SEL(orientation);
		SEL(accel_motion_count);
		break;
	case G_PIR:
		SEL(motion_count);
		break;
	case G_EXT1:
		SEL(ext1_temperature);
		break;
	case G_EXT2:
		SEL(ext2_temperature);
		break;
	case G_MP1:
		SEL(mp1_temperature);
		SEL(mp1_humidity);
		SEL(mp1_flags);
		break;
	case G_MP2:
		SEL(mp2_temperature);
		SEL(mp2_humidity);
		SEL(mp2_flags);
		break;
	case G_HALL_L:
		SEL(hall_left_count);
		SEL(hall_left_flags);
		break;
	case G_HALL_R:
		SEL(hall_right_count);
		SEL(hall_right_flags);
		break;
	case G_INPUT_A:
		SEL(input_a_count);
		SEL(input_a_flags);
		break;
	case G_INPUT_B:
		SEL(input_b_count);
		SEL(input_b_flags);
		break;
	default:
		break;
	}
#undef SEL
}

/* True if a group carries any data in the snapshot (any of its fields present). */
static bool group_present(const Telemetry *s, enum tlm_group g)
{
	Telemetry probe = Telemetry_init_zero;

	apply_group(&probe, s, g, true);

	/* Re-encode the probe: empty (no has_ set) → 0 bytes. */
	size_t sz = 0;
	pb_get_encoded_size(&sz, Telemetry_fields, &probe);
	return sz > 0;
}

/* Snapshot held across the frames of one report (consistency). */
static Telemetry m_snapshot;
static uint16_t m_pending; /* bitmask of enum tlm_group still to send */
static bool m_active;

static void fill_snapshot(void)
{
	static bool boot = true;

	Telemetry t = Telemetry_init_zero;
	uint32_t system_flags = boot ? SYSTEM_FLAG_BOOT : 0;

	struct app_hall_data hall;
	struct app_input_data input;
	app_hall_get_data_and_clear_notify(&hall);
	app_input_get_data_and_clear_notify(&input);

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	struct app_sensor_data d = g_app_sensor_data;
	k_mutex_unlock(&g_app_sensor_data_lock);

	/* system — always sent as one group; boot=false is encoded explicitly.
	 * voltage uses 0 as a "no sample" sentinel (only the pre-sample case). */
	t.has_voltage = true;
	t.voltage = isnan(d.voltage) ? 0 : (uint32_t)CLAMP(d.voltage * 50.0f, 0.0f, 255.0f);
	t.has_system_flags = true;
	t.system_flags = system_flags;

	/* internal */
	if (!isnan(d.temperature)) {
		t.has_temperature = true;
		t.temperature = (int32_t)(d.temperature * 100.0f);
	}
	if (!isnan(d.humidity)) {
		t.has_humidity = true;
		t.humidity = (uint32_t)(d.humidity * 2.0f);
	}

	/* barometer */
	if (g_app_config.cap_barometer && !isnan(d.pressure)) {
		t.has_pressure = true;
		t.pressure = (uint32_t)(d.pressure * 1000.0f);
	}
	if (g_app_config.cap_barometer && !isnan(d.altitude)) {
		float a = CLAMP(d.altitude * 10.0f, (float)INT16_MIN, (float)INT16_MAX);
		t.has_altitude = true;
		t.altitude = (int32_t)a;
	}

	/* light */
	if (g_app_config.cap_light_sensor && !isnan(d.illuminance)) {
		t.has_illuminance = true;
		t.illuminance = (uint32_t)(d.illuminance / 2.0f);
	}

	/* accel */
	if (d.orientation != INT_MAX) {
		t.has_orientation = true;
		t.orientation = (uint32_t)(d.orientation & 0xf);
	}
	if (d.accel_motion_count > 0) {
		t.has_accel_motion_count = true;
		t.accel_motion_count = d.accel_motion_count;
	}

	/* pir — whole group sent whenever the detector is enabled (0 is valid) */
	if (g_app_config.cap_pir_detector) {
		t.has_motion_count = true;
		t.motion_count = d.motion_count;
	}

	/* 1-wire ext */
	if (g_app_config.cap_1w_thermometer && !isnan(d.t1_temperature)) {
		t.has_ext1_temperature = true;
		t.ext1_temperature = (int32_t)(d.t1_temperature * 100.0f);
	}
	if (g_app_config.cap_1w_thermometer && !isnan(d.t2_temperature)) {
		t.has_ext2_temperature = true;
		t.ext2_temperature = (int32_t)(d.t2_temperature * 100.0f);
	}

	/* machine probe 1 / 2 */
	if (g_app_config.cap_1w_machine_probe) {
		if (!isnan(d.mp1_temperature)) {
			t.has_mp1_temperature = true;
			t.mp1_temperature = (int32_t)(d.mp1_temperature * 100.0f);
		}
		if (!isnan(d.mp1_humidity)) {
			t.has_mp1_humidity = true;
			t.mp1_humidity = (uint32_t)(d.mp1_humidity * 2.0f);
		}
		t.has_mp1_flags = true;
		t.mp1_flags = d.mp1_is_tilt_alert ? MP_FLAG_TILT : 0;
		if (!isnan(d.mp2_temperature)) {
			t.has_mp2_temperature = true;
			t.mp2_temperature = (int32_t)(d.mp2_temperature * 100.0f);
		}
		if (!isnan(d.mp2_humidity)) {
			t.has_mp2_humidity = true;
			t.mp2_humidity = (uint32_t)(d.mp2_humidity * 2.0f);
		}
		t.has_mp2_flags = true;
		t.mp2_flags = d.mp2_is_tilt_alert ? MP_FLAG_TILT : 0;
	}

	/* hall left / right */
	if (g_app_config.cap_hall_left) {
		uint32_t f = 0;
		if (hall.left_notify_act) {
			f |= CNT_FLAG_NOTIFY_ACT;
		}
		if (hall.left_notify_deact) {
			f |= CNT_FLAG_NOTIFY_DEACT;
		}
		if (hall.left_is_active) {
			f |= CNT_FLAG_ACTIVE;
		}
		t.has_hall_left_count = true;
		t.hall_left_count = hall.left_count;
		t.has_hall_left_flags = true;
		t.hall_left_flags = f;
	}
	if (g_app_config.cap_hall_right) {
		uint32_t f = 0;
		if (hall.right_notify_act) {
			f |= CNT_FLAG_NOTIFY_ACT;
		}
		if (hall.right_notify_deact) {
			f |= CNT_FLAG_NOTIFY_DEACT;
		}
		if (hall.right_is_active) {
			f |= CNT_FLAG_ACTIVE;
		}
		t.has_hall_right_count = true;
		t.hall_right_count = hall.right_count;
		t.has_hall_right_flags = true;
		t.hall_right_flags = f;
	}

	/* input A / B */
	if (g_app_config.cap_input_a) {
		uint32_t f = 0;
		if (input.input_a_notify_act) {
			f |= CNT_FLAG_NOTIFY_ACT;
		}
		if (input.input_a_notify_deact) {
			f |= CNT_FLAG_NOTIFY_DEACT;
		}
		if (input.input_a_is_active) {
			f |= CNT_FLAG_ACTIVE;
		}
		t.has_input_a_count = true;
		t.input_a_count = input.input_a_count;
		t.has_input_a_flags = true;
		t.input_a_flags = f;
	}
	if (g_app_config.cap_input_b) {
		uint32_t f = 0;
		if (input.input_b_notify_act) {
			f |= CNT_FLAG_NOTIFY_ACT;
		}
		if (input.input_b_notify_deact) {
			f |= CNT_FLAG_NOTIFY_DEACT;
		}
		if (input.input_b_is_active) {
			f |= CNT_FLAG_ACTIVE;
		}
		t.has_input_b_count = true;
		t.input_b_count = input.input_b_count;
		t.has_input_b_flags = true;
		t.input_b_flags = f;
	}

	m_snapshot = t;
	m_pending = 0;
	for (enum tlm_group g = 0; g < G_COUNT; g++) {
		if (group_present(&m_snapshot, g)) {
			m_pending |= BIT(g);
		}
	}
	m_active = true;
	boot = false;
}

int app_compose(uint8_t *buf, size_t size, size_t *len, bool *more)
{
	uint8_t budget = app_lrw_get_max_payload();
	if (budget == 0) {
		return -EAGAIN;
	}

	if (!m_active) {
		fill_snapshot();
		if (m_pending == 0) {
			/* Nothing to report (e.g. all sensors NaN pre-sample). */
			*len = 0;
			*more = false;
			m_active = false;
			return 0;
		}
	}

	/* Reserve 1 byte for the version prefix (buf[0]); the protobuf is encoded
	 * from buf+1, so the group-packing budget loses that byte. */
	size_t cap = MIN(size, (size_t)budget) - 1;

	/* Greedily pack whole pending groups, highest priority first, that fit. */
	Telemetry frame = Telemetry_init_zero;
	uint16_t frame_groups = 0;

	for (enum tlm_group g = 0; g < G_COUNT; g++) {
		if (!(m_pending & BIT(g))) {
			continue;
		}
		apply_group(&frame, &m_snapshot, g, true); /* tentatively add */
		size_t sz = 0;
		pb_get_encoded_size(&sz, Telemetry_fields, &frame);
		if (sz <= cap) {
			frame_groups |= BIT(g);
		} else {
			apply_group(&frame, &m_snapshot, g, false); /* revert */
		}
	}

	/* A single group bigger than the budget would stall forever: force the
	 * highest-priority pending group out alone and log it. */
	if (frame_groups == 0) {
		for (enum tlm_group g = 0; g < G_COUNT; g++) {
			if (m_pending & BIT(g)) {
				apply_group(&frame, &m_snapshot, g, true);
				frame_groups = BIT(g);
				LOG_WRN("Group %d exceeds budget %uB, sending alone", (int)g,
					budget);
				break;
			}
		}
	}

	buf[0] = APP_PROTO_VERSION;
	pb_ostream_t os = pb_ostream_from_buffer(buf + 1, size - 1);
	if (!pb_encode(&os, Telemetry_fields, &frame)) {
		LOG_ERR("pb_encode failed: %s", PB_GET_ERROR(&os));
		m_active = false;
		return -EMSGSIZE;
	}

	m_pending &= ~frame_groups;
	*len = os.bytes_written + 1;
	*more = (m_pending != 0);
	if (!*more) {
		m_active = false;
	}

	LOG_INF("TX: budget=%uB (from system), frame=%zuB, groups=0x%04x, more=%d", budget, *len,
		frame_groups, (int)*more);
	LOG_HEXDUMP_DBG(buf, *len, "Telemetry frame:");

	return 0;
}
