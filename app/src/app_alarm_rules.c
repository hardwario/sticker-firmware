/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm_rules.h"
#include "app_config.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_alarm_rules, LOG_LEVEL_INF);

/* Packed wire/storage layout of one rule, mirrored in app_config.yml and the
 * manager-app. 17 bytes, little-endian. */
#define RULE_PACK_LEN     17
#define RULE_FLAG_PRESENT 0x01 /* slot occupied */
#define RULE_FLAG_ENABLED 0x02 /* rule evaluated */

struct app_alarm_slot {
	bool used;
	struct app_alarm_rule rule;
};

static struct app_alarm_slot m_slots[APP_ALARM_SLOT_COUNT];
static K_MUTEX_DEFINE(m_lock);

/* ---- names -------------------------------------------------------------- */

static const char *const m_source_names[APP_ALARM_SRC_COUNT] = {
	[APP_ALARM_SRC_ONBOARD] = "onboard",
	[APP_ALARM_SRC_SLOT1] = "s1",
	[APP_ALARM_SRC_SLOT2] = "s2",
	[APP_ALARM_SRC_SLOT3] = "s3",
	[APP_ALARM_SRC_SLOT4] = "s4",
	[APP_ALARM_SRC_HALL_LEFT] = "hall-left",
	[APP_ALARM_SRC_HALL_RIGHT] = "hall-right",
	[APP_ALARM_SRC_INPUT_A] = "input-a",
	[APP_ALARM_SRC_INPUT_B] = "input-b",
	[APP_ALARM_SRC_PIR] = "pir",
	[APP_ALARM_SRC_ACCEL] = "accel",
};

static const char *const m_quantity_names[APP_ALARM_Q_QUANTITY_COUNT] = {
	[APP_ALARM_Q_TEMPERATURE] = "temperature",
	[APP_ALARM_Q_HUMIDITY] = "humidity",
	[APP_ALARM_Q_PRESSURE] = "pressure",
	[APP_ALARM_Q_ILLUMINANCE] = "illuminance",
	[APP_ALARM_Q_MAGNETIC_FIELD] = "magnetic-field",
	[APP_ALARM_Q_TILT] = "tilt",
	[APP_ALARM_Q_STATE] = "state",
	[APP_ALARM_Q_COUNT] = "count",
};

const char *app_alarm_source_name(enum app_alarm_source source)
{
	if ((unsigned)source >= APP_ALARM_SRC_COUNT || !m_source_names[source]) {
		return "?";
	}
	return m_source_names[source];
}

const char *app_alarm_quantity_name(enum app_alarm_quantity quantity)
{
	if ((unsigned)quantity >= APP_ALARM_Q_QUANTITY_COUNT || !m_quantity_names[quantity]) {
		return "?";
	}
	return m_quantity_names[quantity];
}

int app_alarm_source_by_name(const char *name)
{
	for (int i = 0; i < APP_ALARM_SRC_COUNT; i++) {
		if (m_source_names[i] && strcmp(name, m_source_names[i]) == 0) {
			return i;
		}
	}
	return -1;
}

int app_alarm_quantity_by_name(const char *name)
{
	for (int i = 0; i < APP_ALARM_Q_QUANTITY_COUNT; i++) {
		if (m_quantity_names[i] && strcmp(name, m_quantity_names[i]) == 0) {
			return i;
		}
	}
	return -1;
}

/* ---- kind / validity ---------------------------------------------------- */

enum app_alarm_kind app_alarm_quantity_kind(enum app_alarm_quantity q)
{
	switch (q) {
	case APP_ALARM_Q_TILT:
	case APP_ALARM_Q_STATE:
		return APP_ALARM_KIND_STATE;
	case APP_ALARM_Q_COUNT:
		return APP_ALARM_KIND_RATE;
	default:
		return APP_ALARM_KIND_THRESHOLD;
	}
}

bool app_alarm_rule_valid(enum app_alarm_source source, enum app_alarm_quantity quantity)
{
	switch (source) {
	case APP_ALARM_SRC_ONBOARD:
		return quantity == APP_ALARM_Q_TEMPERATURE || quantity == APP_ALARM_Q_HUMIDITY ||
		       quantity == APP_ALARM_Q_PRESSURE;
	case APP_ALARM_SRC_SLOT1:
	case APP_ALARM_SRC_SLOT2:
	case APP_ALARM_SRC_SLOT3:
	case APP_ALARM_SRC_SLOT4:
		/* Structural: any quantity a 1-Wire slot could provide. The sensor need
		 * not be enrolled yet; eval yields NaN (inert) if it isn't present or
		 * its type doesn't supply the quantity. */
		return quantity == APP_ALARM_Q_TEMPERATURE || quantity == APP_ALARM_Q_HUMIDITY ||
		       quantity == APP_ALARM_Q_ILLUMINANCE ||
		       quantity == APP_ALARM_Q_MAGNETIC_FIELD || quantity == APP_ALARM_Q_TILT;
	case APP_ALARM_SRC_HALL_LEFT:
	case APP_ALARM_SRC_HALL_RIGHT:
	case APP_ALARM_SRC_INPUT_A:
	case APP_ALARM_SRC_INPUT_B:
	case APP_ALARM_SRC_PIR:
	case APP_ALARM_SRC_ACCEL:
		return quantity == APP_ALARM_Q_STATE || quantity == APP_ALARM_Q_COUNT;
	default:
		return false;
	}
}

/* A STATE rule on a momentary source (PIR/ACCEL) is only meaningful as an edge
 * (from != to): those sources only ever assert — they never report a steady
 * level — so a level rule (from == to) cannot deactivate and is silently treated
 * as a one-shot by eval_state. Reject it at validation time so the
 * misconfiguration surfaces instead of behaving unexpectedly (#203). */
static bool rule_state_shape_valid(const struct app_alarm_rule *r)
{
	bool momentary = (r->source == APP_ALARM_SRC_PIR || r->source == APP_ALARM_SRC_ACCEL);

	if (r->quantity == APP_ALARM_Q_STATE && momentary && r->from_state == r->to_state) {
		return false;
	}
	return true;
}

/* ---- app_config storage (per-slot bytes) -------------------------------- */

/* The 16 alarm_N config fields are separate `uint8_t[RULE_PACK_LEN]` members;
 * map a slot index to its field. Returns NULL for an out-of-range slot. */
static uint8_t *slot_field(struct app_config *c, uint8_t slot)
{
	switch (slot) {
	case 0:
		return c->alarm_0;
	case 1:
		return c->alarm_1;
	case 2:
		return c->alarm_2;
	case 3:
		return c->alarm_3;
	case 4:
		return c->alarm_4;
	case 5:
		return c->alarm_5;
	case 6:
		return c->alarm_6;
	case 7:
		return c->alarm_7;
	case 8:
		return c->alarm_8;
	case 9:
		return c->alarm_9;
	case 10:
		return c->alarm_10;
	case 11:
		return c->alarm_11;
	case 12:
		return c->alarm_12;
	case 13:
		return c->alarm_13;
	case 14:
		return c->alarm_14;
	case 15:
		return c->alarm_15;
	default:
		return NULL;
	}
}

BUILD_ASSERT(sizeof(((struct app_config *)0)->alarm_0) == RULE_PACK_LEN,
	     "alarm slot config field must be RULE_PACK_LEN bytes");

static void pack_rule(const struct app_alarm_rule *r, uint8_t out[RULE_PACK_LEN])
{
	out[0] = RULE_FLAG_PRESENT | (r->enabled ? RULE_FLAG_ENABLED : 0);
	out[1] = r->source;
	out[2] = r->quantity;
	out[3] = r->from_state;
	out[4] = r->to_state;
	memcpy(&out[5], &r->lo, sizeof(float));
	memcpy(&out[9], &r->hi, sizeof(float));
	memcpy(&out[13], &r->hst, sizeof(float));
}

/* Decode `in` into `*r`. Returns true if the slot is present (occupied). */
static bool unpack_rule(const uint8_t in[RULE_PACK_LEN], struct app_alarm_rule *r)
{
	if (!(in[0] & RULE_FLAG_PRESENT)) {
		return false;
	}
	r->enabled = (in[0] & RULE_FLAG_ENABLED) ? 1 : 0;
	r->source = in[1];
	r->quantity = in[2];
	r->from_state = in[3];
	r->to_state = in[4];
	memcpy(&r->lo, &in[5], sizeof(float));
	memcpy(&r->hi, &in[9], sizeof(float));
	memcpy(&r->hst, &in[13], sizeof(float));
	return true;
}

/* ---- CRUD (slot-addressed) ---------------------------------------------- */

uint8_t app_alarm_rules_count(void)
{
	uint8_t n = 0;
	for (int i = 0; i < APP_ALARM_SLOT_COUNT; i++) {
		if (m_slots[i].used) {
			n++;
		}
	}
	return n;
}

const struct app_alarm_rule *app_alarm_rules_get(uint8_t slot)
{
	if (slot >= APP_ALARM_SLOT_COUNT || !m_slots[slot].used) {
		return NULL;
	}
	return &m_slots[slot].rule;
}

bool app_alarm_rules_occupied(uint8_t slot)
{
	return slot < APP_ALARM_SLOT_COUNT && m_slots[slot].used;
}

int app_alarm_rules_first_free(void)
{
	for (int i = 0; i < APP_ALARM_SLOT_COUNT; i++) {
		if (!m_slots[i].used) {
			return i;
		}
	}
	return -1;
}

int app_alarm_rules_set(uint8_t slot, const struct app_alarm_rule *rule)
{
	if (slot >= APP_ALARM_SLOT_COUNT || rule == NULL ||
	    !app_alarm_rule_valid((enum app_alarm_source)rule->source,
				  (enum app_alarm_quantity)rule->quantity) ||
	    !rule_state_shape_valid(rule)) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	pack_rule(rule, slot_field(app_config(), slot));
	m_slots[slot].rule = *rule;
	m_slots[slot].used = true;
	k_mutex_unlock(&m_lock);
	return 0;
}

int app_alarm_rules_clear(uint8_t slot)
{
	if (slot >= APP_ALARM_SLOT_COUNT) {
		return -EINVAL;
	}
	k_mutex_lock(&m_lock, K_FOREVER);
	int ret = m_slots[slot].used ? 0 : -ENOENT;
	memset(slot_field(app_config(), slot), 0, RULE_PACK_LEN);
	m_slots[slot].used = false;
	k_mutex_unlock(&m_lock);
	return ret;
}

void app_alarm_rules_clear_all(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	struct app_config *c = app_config();
	for (int i = 0; i < APP_ALARM_SLOT_COUNT; i++) {
		memset(slot_field(c, (uint8_t)i), 0, RULE_PACK_LEN);
		m_slots[i].used = false;
	}
	k_mutex_unlock(&m_lock);
}

/* ---- persistence (app_config storage) ----------------------------------- */

void app_alarm_rules_reload_from_config(void)
{
	struct app_config *c = app_config();

	k_mutex_lock(&m_lock, K_FOREVER);
	for (uint8_t s = 0; s < APP_ALARM_SLOT_COUNT; s++) {
		struct app_alarm_rule r;
		uint8_t *field = slot_field(c, s);
		/* Drop a slot that decodes to a pair that no longer validates
		 * (e.g. enum changed across FW, or a host wrote garbage). */
		if (unpack_rule(field, &r) &&
		    app_alarm_rule_valid((enum app_alarm_source)r.source,
					 (enum app_alarm_quantity)r.quantity) &&
		    rule_state_shape_valid(&r)) {
			m_slots[s].rule = r;
			m_slots[s].used = true;
		} else {
			/* Zero the persisted bytes too, not just the live cache, so a
			 * rejected slot doesn't linger in NVS and get echoed by
			 * GetParam/dump — keeping stored state consistent with live
			 * state (#197). Non-empty-but-invalid is the case worth a log. */
			if (field[0] & RULE_FLAG_PRESENT) {
				LOG_WRN("Alarm slot %u: invalid persisted rule sanitized", s);
			}
			memset(field, 0, RULE_PACK_LEN);
			m_slots[s].used = false;
		}
	}
	k_mutex_unlock(&m_lock);

	LOG_INF("Loaded %u alarm rule(s)", app_alarm_rules_count());
}

int app_alarm_rules_save(void)
{
	/* Rules live in the app_config slots; persist the config (no reboot). */
	int ret = settings_save();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save", ret);
	}
	return ret;
}

int app_alarm_rules_init(void)
{
	app_alarm_rules_reload_from_config();
	return 0;
}
