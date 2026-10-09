/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_compose.h"
#include "app_alarm.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_radio.h"
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
#define SYSTEM_FLAG_BOOT        BIT(0)
/* Bits 1..8: the device_status alarm byte (APP_DEVICE_STATUS_ALARM_*, bits 0..7)
 * shifted up by one (#409 A5a). Telemetry is the only uplink that still gets
 * through at the 11 B budget tier, where no fPort 3 AlarmReport fits, so the
 * alarm state rides here. Bits 1..6 are in use today (varint stays 1 B). */
#define SYSTEM_FLAG_ALARM_SHIFT 1
#define SYSTEM_FLAG_ALARM_MASK  0xFFu
/* Counter flag bits 0/1 (notify act/deact) retired with the dynamic-alarms
 * migration — notify is now an alarm rule, not a per-counter telemetry flag.
 * ACTIVE stays at bit 2 to keep the wire bit position stable. */
#define CNT_FLAG_ACTIVE         BIT(2)

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
	/* Compose runs solely on the radio work queue; static keeps this large struct off the
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

/* Packing cursor of a report: what is still to send. */
/* Upper bound of page_index / page_count while laying pages out: keeps both
 * varints at one byte, so the real values never make a page grow. */
#define PAGE_COUNT_BOUND 127

struct tlm_cursor {
	uint16_t pending; /* bitmask of enum tlm_group still to send */
	pb_size_t w1;     /* next w1_sensors reading of the snapshot */
	uint8_t ch;       /* next channel of reading `w1` when it is split, else 0 */
};

static struct tlm_cursor m_cur;
static bool m_active;
static uint8_t m_cap;        /* protobuf budget captured at the report start */
static uint8_t m_page_index; /* next page to send */
static uint8_t m_page_count; /* pages of this report (1 = not paged) */

/* Map the current sensor + counter readings into `t`. Pure mapping with no
 * compose state-machine side effects, so it backs both the LoRaWAN snapshot
 * (fill_snapshot) and the synchronous Sample response (app_compose_snapshot).
 * `boot` sets the one-shot system boot flag. */
#if defined(CONFIG_W1)
BUILD_ASSERT(ARRAY_SIZE(((SensorReading *)0)->value) >= APP_SENSOR_W1_CH_MAX,
	     "SensorReading.value max_count must cover APP_SENSOR_W1_CH_MAX");

/* A slot's reading as the channel model on the wire (#430, D2 = c): bit ch of
 * `valid` per channel holding a value, and the values of those channels in
 * ascending ch order, each scaled by its registry wire scale. */
static void encode_sensor_reading(SensorReading *sr, const struct app_sensor_w1 *r)
{
	const struct app_sensor_type *type = app_sensor_type_get(sr->type);

	if (type == NULL || r->type != sr->type) {
		return;
	}
	for (uint8_t ch = 0; ch < type->channel_count && ch < ARRAY_SIZE(sr->value); ch++) {
		const struct app_sensor_channel *c = &type->channels[ch];

		if (!(r->valid & BIT(ch)) || (c->flags & APP_SENSOR_F_RETIRED)) {
			continue;
		}
		sr->valid |= BIT(ch);
		sr->value[sr->value_count++] = app_sensor_wire_value(c, r->v[ch]);
	}
}
#endif /* defined(CONFIG_W1) */

static void fill_telemetry(Telemetry *t, bool boot)
{
	memset(t, 0, sizeof(*t));
	uint32_t system_flags = boot ? SYSTEM_FLAG_BOOT : 0;

	system_flags |= (app_alarm_status_flags() & SYSTEM_FLAG_ALARM_MASK)
			<< SYSTEM_FLAG_ALARM_SHIFT;

	struct app_hall_data hall;
	struct app_input_data input;
	app_hall_get_data(&hall);
	app_input_get_data(&input);

	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	struct app_sensor_data d = g_app_sensor_data;
	k_mutex_unlock(&g_app_sensor_data_lock);

	const float voltage = APP_SENSOR_MB_F(&d, BATTERY_VOLTAGE);
	const float temperature = APP_SENSOR_MB_F(&d, TEMPERATURE);
	const float humidity = APP_SENSOR_MB_F(&d, HUMIDITY);
	const float pressure = APP_SENSOR_MB_F(&d, PRESSURE); /* hPa */
	const float altitude = APP_SENSOR_MB_F(&d, ALTITUDE);
	const float illuminance = APP_SENSOR_MB_F(&d, ILLUMINANCE);
	const float orientation = APP_SENSOR_MB_F(&d, ACCEL_ORIENTATION);

	/* system — always sent as one group; boot=false is encoded explicitly.
	 * voltage uses 0 as a "no sample" sentinel (only the pre-sample case). */
	t->has_voltage = true;
	t->voltage = isnan(voltage) ? 0 : (uint32_t)CLAMP(voltage * 50.0f, 0.0f, 255.0f);
	t->has_system_flags = true;
	t->system_flags = system_flags;

	/* internal — onboard SHT4x, sent whenever enabled (#465 cap_sht); a NaN
	 * reading (sensor fault) goes out as the sentinel (decoder → null) instead
	 * of dropping the field. */
	if (g_app_config.cap_sht) {
		t->has_temperature = true;
		t->temperature = isnan(temperature) ? TM_S32_NA : (int32_t)(temperature * 100.0f);
		t->has_humidity = true;
		/* Clamp before the unsigned cast: the SHT4x formula can yield a
		 * slightly negative %RH, and a negative float->uint cast is UB. */
		t->humidity = isnan(humidity) ? TM_U32_NA
					      : (uint32_t)CLAMP(humidity * 2.0f, 0.0f, 200.0f);
	}

	/* barometer — sent whenever enabled (sentinel on NaN). */
	if (g_app_config.cap_barometer) {
		t->has_pressure = true;
		/* The pressure channel is hPa; the wire unit is hPa x10 (0.1 hPa). */
		t->pressure = isnan(pressure) ? TM_U32_NA
					      : (uint32_t)CLAMP(pressure * 10.0f, 0.0f, 200000.0f);
		t->has_altitude = true;
		t->altitude = isnan(altitude) ? TM_S32_NA
					      : (int32_t)CLAMP(altitude * 10.0f, (float)INT16_MIN,
							       (float)INT16_MAX);
	}

	/* light — sent whenever enabled (sentinel on NaN). */
	if (g_app_config.cap_light_sensor) {
		t->has_illuminance = true;
		t->illuminance = isnan(illuminance)
					 ? TM_U32_NA
					 : (uint32_t)CLAMP(illuminance / 2.0f, 0.0f, 1000000.0f);
	}

	/* accel (gated by the accelerometer capability) */
	if (g_app_config.cap_accelerometer && !isnan(orientation)) {
		t->has_orientation = true;
		t->orientation = (uint32_t)((int)orientation & 0xf);
	}
	/* Always send the count when the accelerometer is enabled (0 included) —
	 * the #78/#80 "whole group every report" policy that the other digital
	 * counters already follow; this was the lone holdout. */
	if (g_app_config.cap_accelerometer) {
		t->has_accel_motion_count = true;
		t->accel_motion_count = APP_SENSOR_MB_U(&d, ACCEL_COUNT);
	}

	/* pir — whole group sent whenever the detector is enabled (0 is valid) */
	if (g_app_config.cap_pir_detector) {
		t->has_motion_count = true;
		t->motion_count = APP_SENSOR_MB_U(&d, PIR_COUNT);
	}

	/* 1-wire ROM-bound slots → one repeated SensorReading per slot with an
	 * expected type (sensorN_type, #430): the valid mask plus the values of the
	 * present channels, scaled by the registry (encode_sensor_reading), so
	 * adding a sensor type needs no change here. A slot whose probe is absent
	 * or mismatched is still sent with valid = 0, so the decoder emits null.
	 * The composer may split the list across frames (each reading is
	 * indivisible). */
#if defined(CONFIG_W1)
	if (g_app_config.cap_w1_sensors) {
		for (int i = 0; i < APP_W1_SLOT_COUNT; i++) {
			uint8_t type = app_w1_slot_get_expected_type(i);
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
			if (app_w1_slot_get_state(i) == APP_W1_SLOT_STATE_OK) {
				encode_sensor_reading(sr, &d.w1[i]);
			}
			t->w1_sensors_count++;
		}
	}
#endif /* defined(CONFIG_W1) */

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

/* #340 M16: the one-shot "first uplink after boot" marker. Module-level (not a
 * fill_snapshot()-local static) so app_compose_ex()'s debug/test callers (e.g.
 * `ats radio compose`) can read it without being the ones who clear it — only a
 * real report (app_compose(), consume_boot=true below) may consume it. Without
 * this split, a bench tech running `ats radio compose` before the real first
 * post-boot cycle silently stole the marker: the debug dump got
 * SYSTEM_FLAG_BOOT and the real first uplink went out with system_flags=0. */
static bool m_boot_pending = true;

/* True while the in-progress snapshot belongs to the shell's debug probe
 * (app_compose_ex()) rather than the real TX path. Both paths run their
 * multi-frame sessions as separate per-frame work items on the radio work queue, so they
 * can interleave — without this tag the real report would silently drain the
 * remainder of a debug session's snapshot over the air (and vice versa). */
static bool m_active_debug;

/* Take a fresh snapshot into m_snapshot and arm the multi-frame packer. Runs
 * solely on the radio work queue. `consume_boot` is true only for the real report path
 * (app_compose()) — a debug/test probe (app_compose_ex()) must not clear the
 * one-shot marker for the real uplink that hasn't happened yet. */
static void fill_snapshot(bool consume_boot)
{
	fill_telemetry(&m_snapshot, m_boot_pending);

	m_cur = (struct tlm_cursor){0};
	for (enum tlm_group g = 0; g < G_COUNT; g++) {
		if (group_present(&m_snapshot, g)) {
			m_cur.pending |= BIT(g);
		}
	}
	m_active = true;
	if (consume_boot) {
		m_boot_pending = false;
	}
}

void app_compose_reset(void)
{
	/* Drop the in-progress snapshot; the next app_compose() takes a fresh one.
	 * These run solely on the radio work queue (as does the join path that calls this), so
	 * no lock is needed. */
	m_active = false;
	m_cur = (struct tlm_cursor){0};
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

static bool cursor_done(const struct tlm_cursor *c)
{
	return c->pending == 0 && c->w1 >= m_snapshot.w1_sensors_count;
}

static bool frame_fits(const Telemetry *f, size_t cap)
{
	size_t sz = 0;

	pb_get_encoded_size(&sz, Telemetry_fields, f);
	return sz <= cap;
}

/* Append to `f` a part of reading `r` (#430): its present channels from c->ch
 * on, as many as fit `cap`, in a SensorReading with the same slot / type and
 * the part's own `valid` bits. `force` sends the first channel even when it
 * does not fit (a page that would otherwise stay empty). Advances the cursor;
 * false when nothing was added. */
static bool add_w1_part(Telemetry *f, const SensorReading *r, struct tlm_cursor *c, size_t cap,
			bool force)
{
	pb_size_t at = f->w1_sensors_count;
	SensorReading *p = &f->w1_sensors[at];
	pb_size_t k = 0; /* index of the channel's value in r->value */
	uint8_t next = 0;

	*p = (SensorReading)SensorReading_init_zero;
	p->slot = r->slot;
	p->type = r->type;
	f->w1_sensors_count = at + 1;

	for (uint8_t ch = 0; ch < 32 && k < r->value_count; ch++) {
		if (!(r->valid & BIT(ch))) {
			continue;
		}
		if (ch < c->ch) {
			k++;
			continue;
		}
		p->valid |= BIT(ch);
		p->value[p->value_count++] = r->value[k++];
		if (!frame_fits(f, cap) && !(force && p->value_count == 1)) {
			p->valid &= ~BIT(ch);
			p->value_count--;
			next = ch;
			break;
		}
	}

	if (p->value_count == 0) {
		f->w1_sensors_count = at;
		return false;
	}
	if (!frame_fits(f, cap)) {
		LOG_WRN("w1 slot=%u channel exceeds budget %zuB, sending alone", (unsigned)r->slot,
			cap);
	}
	if (next == 0) {
		c->w1++;
		c->ch = 0;
	} else {
		c->ch = next;
	}
	return true;
}

/* Fill `f` with the next page from cursor `c` and advance it: whole pending
 * groups first (highest priority first), then 1-Wire readings in order. A
 * reading that does not fit an otherwise empty page is split by channel
 * across pages (add_w1_part); one that does not fit beside other content
 * waits for the next page. A single unit bigger than the budget is sent alone
 * so the report never stalls. `paged` reserves the page_index / page_count
 * fields (index exact, count at its one-byte bound) so the layout matches the
 * frames sent later. */
static void pack_page(Telemetry *f, struct tlm_cursor *c, size_t cap, bool paged, uint8_t index)
{
	uint16_t frame_groups = 0;
	pb_size_t w1_added = 0;

	memset(f, 0, sizeof(*f));
	if (paged) {
		f->page_index = index;
		f->page_count = PAGE_COUNT_BOUND;
	}

	for (enum tlm_group g = 0; g < G_COUNT; g++) {
		if (!(c->pending & BIT(g))) {
			continue;
		}
		apply_group(f, &m_snapshot, g, true); /* tentatively add */
		if (frame_fits(f, cap)) {
			frame_groups |= BIT(g);
		} else {
			apply_group(f, &m_snapshot, g, false); /* revert */
		}
	}

	while (c->w1 < m_snapshot.w1_sensors_count) {
		const SensorReading *r = &m_snapshot.w1_sensors[c->w1];
		bool empty = frame_groups == 0 && w1_added == 0;

		if (c->ch == 0) {
			pb_size_t at = f->w1_sensors_count;

			f->w1_sensors[at] = *r;
			f->w1_sensors_count = at + 1;
			if (frame_fits(f, cap)) {
				c->w1++;
				w1_added++;
				continue;
			}
			f->w1_sensors_count = at; /* revert */
			if (r->valid == 0 && empty) {
				/* No channel to split by (mismatch / absent slot). */
				f->w1_sensors[at] = *r;
				f->w1_sensors_count = at + 1;
				c->w1++;
				w1_added++;
				LOG_WRN("w1 slot=%u exceeds budget %zuB, sending alone",
					(unsigned)r->slot, cap);
				break;
			}
		}
		if ((empty || c->ch != 0) && add_w1_part(f, r, c, cap, empty)) {
			w1_added++;
		}
		break; /* a split reading ends the page */
	}

	if (frame_groups == 0 && w1_added == 0) {
		for (enum tlm_group g = 0; g < G_COUNT; g++) {
			if (c->pending & BIT(g)) {
				apply_group(f, &m_snapshot, g, true);
				frame_groups = BIT(g);
				LOG_WRN("Group %d exceeds budget %zuB, sending alone", (int)g, cap);
				break;
			}
		}
	}

	c->pending &= ~frame_groups;
}

/* Lay the report out for `cap` (#425 paging, as AlarmReport / Response): the
 * number of pages, or 1 when everything fits one frame without page fields.
 * `probe` is scratch space for the trial pages. */
static uint8_t layout_pages(Telemetry *probe, size_t cap)
{
	struct tlm_cursor c = m_cur;

	pack_page(probe, &c, cap, false, 0);
	if (cursor_done(&c)) {
		return 1;
	}

	uint8_t pages = 0;

	c = m_cur;
	while (!cursor_done(&c) && pages < PAGE_COUNT_BOUND) {
		pack_page(probe, &c, cap, true, pages);
		pages++;
	}
	return pages;
}

static int compose_ex_impl(uint8_t *buf, size_t size, size_t *len, bool *more, uint8_t budget,
			   bool consume_boot)
{
	/* Static: app_compose runs solely on the radio work queue and the struct
	 * is too big for that stack. Also the layout pass's scratch frame. */
	static Telemetry frame;

	if (budget == 0) {
		return -EAGAIN;
	}

	bool debug_probe = !consume_boot;

	/* Cross-session guard: the real path drops a leftover debug snapshot and
	 * composes fresh (the debug probe simply restarts on its next call); the
	 * debug probe must never steal frames from an in-flight real report. */
	if (m_active && m_active_debug != debug_probe) {
		if (debug_probe) {
			return -EBUSY;
		}
		app_compose_reset();
	}

	if (!m_active) {
		fill_snapshot(consume_boot);
		m_active_debug = debug_probe;
		if (cursor_done(&m_cur)) {
			/* Nothing to report (e.g. all sensors NaN pre-sample). */
			*len = 0;
			*more = false;
			m_active = false;
			return 0;
		}
		/* Reserve 1 byte for the version prefix (buf[0]); the protobuf is
		 * encoded from buf+1. The pages are laid out once, for the budget at
		 * the report start, so page_count stays exact for every frame. */
		m_cap = (uint8_t)(MIN(size, (size_t)budget) - 1);
		m_page_index = 0;
		m_page_count = layout_pages(&frame, m_cap);
	}

	bool paged = m_page_count > 1;

	pack_page(&frame, &m_cur, m_cap, paged, m_page_index);
	if (paged) {
		frame.page_index = m_page_index;
		frame.page_count = m_page_count;
	}

	buf[0] = APP_PROTO_VERSION;
	pb_ostream_t os = pb_ostream_from_buffer(buf + 1, size - 1);
	if (!pb_encode(&os, Telemetry_fields, &frame)) {
		LOG_ERR("pb_encode failed: %s", PB_GET_ERROR(&os));
		m_active = false;
		return -EMSGSIZE;
	}

	m_page_index++;
	*len = os.bytes_written + 1;
	*more = !cursor_done(&m_cur);
	if (!*more) {
		m_active = false;
	}

	LOG_INF("TX: budget=%uB, frame=%zuB, page %u/%u, w1=%u/%u, more=%d", budget, *len,
		(unsigned)m_page_index, (unsigned)m_page_count, (unsigned)m_cur.w1,
		(unsigned)m_snapshot.w1_sensors_count, (int)*more);
	LOG_HEXDUMP_DBG(buf, *len, "Telemetry frame:");

	return 0;
}

int app_compose(uint8_t *buf, size_t size, size_t *len, bool *more)
{
	return compose_ex_impl(buf, size, len, more, app_radio_get_max_payload(), true);
}

int app_compose_ex(uint8_t *buf, size_t size, size_t *len, bool *more, uint8_t budget)
{
	return compose_ex_impl(buf, size, len, more, budget, false);
}

int app_compose_budget(uint8_t *buf, size_t size, size_t *len, bool *more, uint8_t budget)
{
	return compose_ex_impl(buf, size, len, more, budget, true);
}
