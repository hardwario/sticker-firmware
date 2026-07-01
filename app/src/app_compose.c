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

/* "Value not available" sentinels (mirrored in ttn.js → null). An enabled analog
 * sensor is always present on the wire so the configured-sensor list is stable
 * across reports; a missing/NaN reading is sent as the sentinel rather than
 * dropping the field (which would be indistinguishable from a disabled sensor). */
#define TM_S32_NA INT32_MIN  /* sint32 fields: temperature, altitude */
#define TM_U32_NA UINT32_MAX /* uint32 fields: humidity, pressure, illuminance */

/* Per-group flag bit positions (mirrored in ttn.js). */
#define SYSTEM_FLAG_BOOT BIT(0)
/* MP_FLAG_TILT moved to app_w1_slots.c with the per-type SensorReading encode. */
/* Counter flag bits 0/1 (notify act/deact) retired with the dynamic-alarms
 * migration — notify is now an alarm rule, not a per-counter telemetry flag.
 * ACTIVE stays at bit 2 to keep the wire bit position stable. */
#define CNT_FLAG_ACTIVE  BIT(2)

/* Sensor groups, in priority order (packed into frames first → last). A group
 * is the atomic unit: all its fields go into one frame, or none. */
enum tlm_group {
	G_INTERNAL = 0, /* temperature, humidity */
	G_SYSTEM,       /* voltage, system_flags */
	G_BAROMETER,    /* pressure, altitude */
	G_LIGHT,        /* illuminance */
	G_ACCEL,        /* orientation */
	G_PIR,          /* motion_count */
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
	/* Compose runs solely on m_work_q; static keeps this large struct off the
	 * tight work-queue stack (it grew with the w1_sensors array). */
	static Telemetry probe;

	memset(&probe, 0, sizeof(probe));
	apply_group(&probe, s, g, true);

	/* Re-encode the probe: empty (no has_ set) → 0 bytes. */
	size_t sz = 0;
	pb_get_encoded_size(&sz, Telemetry_fields, &probe);
	return sz > 0;
}

/* Snapshot held across the frames of one report (consistency). */
static Telemetry m_snapshot;
static uint16_t m_pending;  /* bitmask of enum tlm_group still to send */
static pb_size_t m_w1_sent; /* repeated w1_sensors already emitted (split cursor) */
static bool m_active;

/* Map the current sensor + counter readings into `t`. Pure mapping with no
 * compose state-machine side effects, so it backs both the LoRaWAN snapshot
 * (fill_snapshot) and the synchronous Sample response (app_compose_snapshot).
 * `boot` sets the one-shot system boot flag. */
static void fill_telemetry(Telemetry *t, bool boot)
{
	memset(t, 0, sizeof(*t));
	uint32_t system_flags = boot ? SYSTEM_FLAG_BOOT : 0;

	struct app_hall_data hall;
	struct app_input_data input;
	app_hall_get_data(&hall);
	app_input_get_data(&input);

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	struct app_sensor_data d = g_app_sensor_data;
	k_mutex_unlock(&g_app_sensor_data_lock);

	/* system — always sent as one group; boot=false is encoded explicitly.
	 * voltage uses 0 as a "no sample" sentinel (only the pre-sample case). */
	t->has_voltage = true;
	t->voltage = isnan(d.voltage) ? 0 : (uint32_t)CLAMP(d.voltage * 50.0f, 0.0f, 255.0f);
	t->has_system_flags = true;
	t->system_flags = system_flags;

	/* internal — onboard SHT4x is always present, so temperature/humidity are
	 * always on the wire; a NaN reading (sensor fault) goes out as the sentinel
	 * (decoder → null) instead of dropping the field. */
	t->has_temperature = true;
	t->temperature = isnan(d.temperature) ? TM_S32_NA : (int32_t)(d.temperature * 100.0f);
	t->has_humidity = true;
	/* Clamp before the unsigned cast: the SHT4x formula can yield a slightly
	 * negative %RH, and a negative float->uint cast is UB. */
	t->humidity =
		isnan(d.humidity) ? TM_U32_NA : (uint32_t)CLAMP(d.humidity * 2.0f, 0.0f, 200.0f);

	/* barometer — sent whenever enabled (sentinel on NaN). */
	if (g_app_config.cap_barometer) {
		t->has_pressure = true;
		/* d.pressure is kPa from the driver; the wire unit is hPa x10
		 * (0.1 hPa resolution). hPa = kPa x10, so hPa x10 = kPa x100. */
		t->pressure = isnan(d.pressure)
				      ? TM_U32_NA
				      : (uint32_t)CLAMP(d.pressure * 100.0f, 0.0f, 200000.0f);
		t->has_altitude = true;
		t->altitude = isnan(d.altitude)
				      ? TM_S32_NA
				      : (int32_t)CLAMP(d.altitude * 10.0f, (float)INT16_MIN,
						       (float)INT16_MAX);
	}

	/* light — sent whenever enabled (sentinel on NaN). */
	if (g_app_config.cap_light_sensor) {
		t->has_illuminance = true;
		t->illuminance = isnan(d.illuminance)
					 ? TM_U32_NA
					 : (uint32_t)CLAMP(d.illuminance / 2.0f, 0.0f, 1000000.0f);
	}

	/* accel (gated by the accelerometer capability) */
	if (g_app_config.cap_accelerometer && d.orientation != INT_MAX) {
		t->has_orientation = true;
		t->orientation = (uint32_t)(d.orientation & 0xf);
	}
	/* Always send the count when the accelerometer is enabled (0 included) —
	 * the #78/#80 "whole group every report" policy that the other digital
	 * counters already follow; this was the lone holdout. */
	if (g_app_config.cap_accelerometer) {
		t->has_accel_motion_count = true;
		t->accel_motion_count = d.accel_motion_count;
	}

	/* pir — whole group sent whenever the detector is enabled (0 is valid) */
	if (g_app_config.cap_pir_detector) {
		t->has_motion_count = true;
		t->motion_count = d.motion_count;
	}

	/* 1-wire ROM-bound slots → one repeated SensorReading per populated slot.
	 * The composer owns the slot index, type and the repeated array; the
	 * per-type value fields are filled by the slot's driver via the registry
	 * vtable (app_w1_slot_encode), so adding a sensor type needs no change here.
	 * type travels with the reading; the composer may split the list across
	 * frames (each reading is indivisible). Absent quantities stay omitted. */
	if (g_app_config.cap_w1_sensors) {
		for (int i = 0; i < APP_W1_SLOT_COUNT; i++) {
			enum app_w1_slot_type type = app_w1_slot_get_type(i);
			if (type == APP_W1_SLOT_EMPTY) {
				continue; /* unconfigured slot → no reading */
			}
			SensorReading *sr = &t->w1_sensors[t->w1_sensors_count];
			*sr = (SensorReading)SensorReading_init_zero;
			/* 1-based on the wire to match the sensorN config keys and
			 * `w1 list` (which both number slots from 1); the array index i
			 * stays 0-based internally. */
			sr->slot = i + 1;
			sr->type = type;
			app_w1_slot_encode(i, &d.w1[i], sr);
			t->w1_sensors_count++;
		}
	}

	/* hall left / right */
	if (g_app_config.cap_hall_left) {
		uint32_t f = 0;
		if (hall.left_is_active) {
			f |= CNT_FLAG_ACTIVE;
		}
		t->has_hall_left_count = true;
		t->hall_left_count = hall.left_count;
		t->has_hall_left_flags = true;
		t->hall_left_flags = f;
	}
	if (g_app_config.cap_hall_right) {
		uint32_t f = 0;
		if (hall.right_is_active) {
			f |= CNT_FLAG_ACTIVE;
		}
		t->has_hall_right_count = true;
		t->hall_right_count = hall.right_count;
		t->has_hall_right_flags = true;
		t->hall_right_flags = f;
	}

	/* input A / B */
	if (g_app_config.cap_input_a) {
		uint32_t f = 0;
		if (input.input_a_is_active) {
			f |= CNT_FLAG_ACTIVE;
		}
		t->has_input_a_count = true;
		t->input_a_count = input.input_a_count;
		t->has_input_a_flags = true;
		t->input_a_flags = f;
	}
	if (g_app_config.cap_input_b) {
		uint32_t f = 0;
		if (input.input_b_is_active) {
			f |= CNT_FLAG_ACTIVE;
		}
		t->has_input_b_count = true;
		t->input_b_count = input.input_b_count;
		t->has_input_b_flags = true;
		t->input_b_flags = f;
	}
}

/* Take a fresh snapshot into m_snapshot and arm the multi-frame packer. Runs
 * solely on m_work_q. */
static void fill_snapshot(void)
{
	static bool boot = true;

	fill_telemetry(&m_snapshot, boot);

	m_pending = 0;
	for (enum tlm_group g = 0; g < G_COUNT; g++) {
		if (group_present(&m_snapshot, g)) {
			m_pending |= BIT(g);
		}
	}
	m_w1_sent = 0;
	m_active = true;
	boot = false;
}

void app_compose_reset(void)
{
	/* Drop the in-progress snapshot; the next app_compose() takes a fresh one.
	 * These run solely on m_work_q (as does the join path that calls this), so
	 * no lock is needed. */
	m_active = false;
	m_pending = 0;
	m_w1_sent = 0;
}

void app_compose_snapshot(Telemetry *out)
{
	if (!out) {
		return;
	}
	/* Full reading (all groups) for a synchronous response — e.g. the Sample
	 * command over NFC, where the whole Telemetry fits in one frame and
	 * there is no DR budget to bin-pack against. Pure mapping: it neither
	 * disturbs the in-progress LoRaWAN snapshot nor consumes the boot flag. */
	fill_telemetry(out, false);
}

int app_compose(uint8_t *buf, size_t size, size_t *len, bool *more)
{
	return app_compose_ex(buf, size, len, more, app_lrw_get_max_payload());
}

int app_compose_ex(uint8_t *buf, size_t size, size_t *len, bool *more, uint8_t budget)
{
	if (budget == 0) {
		return -EAGAIN;
	}

	if (!m_active) {
		fill_snapshot();
		if (m_pending == 0 && m_snapshot.w1_sensors_count == 0) {
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

	/* Greedily pack whole pending groups, highest priority first, that fit.
	 * Static for the same reason as the snapshot: app_compose runs solely on
	 * m_work_q and the struct is too big for that stack. */
	static Telemetry frame;
	memset(&frame, 0, sizeof(frame));
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

	/* Append pending 1-Wire readings one at a time (lowest priority, after the
	 * whole-group scalars). The repeated list may split across frames: stop at
	 * the first reading that no longer fits and carry the rest (kept contiguous
	 * from frame.w1_sensors[0]). */
	pb_size_t w1_added = 0;
	for (pb_size_t i = m_w1_sent; i < m_snapshot.w1_sensors_count; i++) {
		pb_size_t at = frame.w1_sensors_count;
		frame.w1_sensors[at] = m_snapshot.w1_sensors[i];
		frame.w1_sensors_count = at + 1;
		size_t sz = 0;
		pb_get_encoded_size(&sz, Telemetry_fields, &frame);
		if (sz <= cap) {
			w1_added++;
		} else {
			frame.w1_sensors_count = at; /* revert; stop, keep order */
			break;
		}
	}

	/* A single unit bigger than the budget would stall forever: force the
	 * highest-priority pending group — or, if none, the next 1-Wire reading —
	 * out alone and log it. */
	if (frame_groups == 0 && w1_added == 0) {
		bool forced = false;
		for (enum tlm_group g = 0; g < G_COUNT; g++) {
			if (m_pending & BIT(g)) {
				apply_group(&frame, &m_snapshot, g, true);
				frame_groups = BIT(g);
				LOG_WRN("Group %d exceeds budget %uB, sending alone", (int)g,
					budget);
				forced = true;
				break;
			}
		}
		if (!forced && m_w1_sent < m_snapshot.w1_sensors_count) {
			frame.w1_sensors[0] = m_snapshot.w1_sensors[m_w1_sent];
			frame.w1_sensors_count = 1;
			w1_added = 1;
			LOG_WRN("w1 reading slot=%u exceeds budget %uB, sending alone",
				(unsigned)m_snapshot.w1_sensors[m_w1_sent].slot, budget);
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
	m_w1_sent += w1_added;
	*len = os.bytes_written + 1;
	*more = (m_pending != 0) || (m_w1_sent < m_snapshot.w1_sensors_count);
	if (!*more) {
		m_active = false;
	}

	LOG_INF("TX: budget=%uB (from system), frame=%zuB, groups=0x%04x, w1=%u/%u, more=%d",
		budget, *len, frame_groups, (unsigned)m_w1_sent,
		(unsigned)m_snapshot.w1_sensors_count, (int)*more);
	LOG_HEXDUMP_DBG(buf, *len, "Telemetry frame:");

	return 0;
}
