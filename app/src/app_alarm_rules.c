/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm_rules.h"
#include "app_config.h"
#include "app_log.h"
#include "app_radio.h"

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
 * manager-app. 18 bytes, little-endian (#430). */
#define RULE_PACK_LEN     18
#define RULE_FLAG_PRESENT 0x01 /* rule occupied */
#define RULE_FLAG_ENABLED 0x02 /* rule evaluated */

struct app_alarm_entry {
	bool used;
	struct app_alarm_rule rule;
};

static struct app_alarm_entry m_rules[APP_ALARM_RULE_COUNT];
static K_MUTEX_DEFINE(m_lock);

/* ---- slot names ---------------------------------------------------------- */

static const char *const m_slot_names[APP_ALARM_SLOT_MAX + 1] = {"mb", "s1", "s2", "s3", "s4"};

const char *app_alarm_slot_name(uint8_t slot)
{
	return slot <= APP_ALARM_SLOT_MAX ? m_slot_names[slot] : "?";
}

int app_alarm_slot_by_name(const char *name)
{
	for (int i = 0; i <= APP_ALARM_SLOT_MAX; i++) {
		if (strcmp(name, m_slot_names[i]) == 0) {
			return i;
		}
	}
	return -1;
}

/* ---- slot type / validity ------------------------------------------------ */

/* Registry type of `slot` under config `c`: the motherboard for slot 0, the
 * configured sensorN_type for a 1-Wire slot. */
static uint8_t slot_type_in(const struct app_config *c, uint8_t slot)
{
	switch (slot) {
	case 0:
		return APP_SENSOR_TYPE_MOTHERBOARD;
	case 1:
		return c->sensor1_type;
	case 2:
		return c->sensor2_type;
	case 3:
		return c->sensor3_type;
	case 4:
		return c->sensor4_type;
	default:
		return APP_SENSOR_TYPE_NONE;
	}
}

uint8_t app_alarm_slot_type(uint8_t slot)
{
	return slot_type_in(&g_app_config, slot);
}

bool app_alarm_rule_valid(uint8_t slot, uint8_t channel, uint8_t sensor_type)
{
	const struct app_sensor_type *t = app_sensor_type_get(sensor_type);

	if (t == NULL || slot > APP_ALARM_SLOT_MAX) {
		return false;
	}
	/* Slot 0 is the motherboard; slots 1..4 take 1-Wire types only. */
	if ((slot == APP_ALARM_SLOT_MB) != (sensor_type == APP_SENSOR_TYPE_MOTHERBOARD) ||
	    (slot != APP_ALARM_SLOT_MB && t->w1_family == 0)) {
		return false;
	}

	const struct app_sensor_channel *c = app_sensor_channel_get(sensor_type, channel);

	return c != NULL && c->kind != APP_SENSOR_KIND_NONE &&
	       !(c->flags & (APP_SENSOR_F_RETIRED | APP_SENSOR_F_WATCHDOG_ONLY));
}

const struct app_sensor_channel *app_alarm_rule_channel(const struct app_alarm_rule *r)
{
	if (!app_alarm_rule_valid(r->slot, r->channel, r->sensor_type)) {
		return NULL;
	}
	return app_sensor_channel_get(r->sensor_type, r->channel);
}

static bool rule_stale_in(const struct app_config *c, const struct app_alarm_rule *r)
{
	return r->sensor_type != slot_type_in(c, r->slot);
}

bool app_alarm_rule_stale(const struct app_alarm_rule *r)
{
	return rule_stale_in(&g_app_config, r);
}

bool app_alarm_rule_armed(const struct app_alarm_rule *r)
{
	const struct app_sensor_channel *c = app_alarm_rule_channel(r);

	if (c == NULL || !r->enabled || app_alarm_rule_stale(r)) {
		return false;
	}
	/* A motherboard rule may be provisioned before its capability is on; it
	 * stays inert until then. */
	return c->cap_off == APP_SENSOR_NO_CAP ||
	       *(const bool *)((const char *)&g_app_config + c->cap_off);
}

/* A STATE rule on a momentary channel (PIR / accelerometer motion) is only
 * meaningful as an edge (from != to): those channels only ever assert — they
 * never report a steady level — so a level rule (from == to) could never
 * deactivate. Reject it so the misconfiguration surfaces (#203). */
static bool rule_state_shape_valid(const struct app_alarm_rule *r,
				   const struct app_sensor_channel *c)
{
	return !(c->kind == APP_SENSOR_KIND_STATE && (c->flags & APP_SENSOR_F_MOMENTARY) &&
		 r->from_state == r->to_state);
}

/* `dwell` (#348) is a plain dwell/hold duration in seconds across every kind
 * that uses it — bound it to a sane range regardless of kind (same cap as the
 * old alarm_light_confirm_delay it replaces). */
#define RULE_DWELL_MAX_S 3600.0f

static bool rule_dwell_range_valid(const struct app_alarm_rule *r)
{
	return r->dwell >= 0.0f && r->dwell <= RULE_DWELL_MAX_S;
}

/* eval_threshold() activates outside [lo, hi] and deactivates only inside it
 * (value >= lo && value <= hi). hi <= lo (empty or inverted band, or a NaN
 * bound making both comparisons false) makes that deactivate condition
 * unsatisfiable by any real reading, so a rule that ever activates would
 * latch forever (#203). Non-THRESHOLD kinds don't use the band. */
static bool rule_threshold_band_valid(const struct app_alarm_rule *r,
				      const struct app_sensor_channel *c)
{
	return c->kind != APP_SENSOR_KIND_THRESHOLD || r->hi > r->lo;
}

/* Target + shape checks a decoded rule must pass before it is stored. */
static bool rule_valid(const struct app_alarm_rule *r)
{
	const struct app_sensor_channel *c = app_alarm_rule_channel(r);

	return c != NULL && rule_state_shape_valid(r, c) && rule_dwell_range_valid(r) &&
	       rule_threshold_band_valid(r, c);
}

/* ---- app_config storage (per-rule bytes) -------------------------------- */

/* The 16 alarm_N config fields are separate `uint8_t[RULE_PACK_LEN]` members;
 * map a rule index to its field. Returns NULL for an out-of-range index. */
static uint8_t *rule_field(struct app_config *c, uint8_t rule)
{
	switch (rule) {
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
	     "alarm rule config field must be RULE_PACK_LEN bytes");

static void pack_rule(const struct app_alarm_rule *r, uint8_t out[RULE_PACK_LEN])
{
	out[0] = RULE_FLAG_PRESENT | (r->enabled ? RULE_FLAG_ENABLED : 0);
	out[1] = r->slot;
	out[2] = r->channel;
	out[3] = r->sensor_type;
	out[4] = r->from_state;
	out[5] = r->to_state;
	memcpy(&out[6], &r->lo, sizeof(float));
	memcpy(&out[10], &r->hi, sizeof(float));
	memcpy(&out[14], &r->dwell, sizeof(float));
}

/* Decode `in` into `*r`. Returns true if the rule is present (occupied). */
static bool unpack_rule(const uint8_t in[RULE_PACK_LEN], struct app_alarm_rule *r)
{
	if (!(in[0] & RULE_FLAG_PRESENT)) {
		return false;
	}
	*r = (struct app_alarm_rule){0};
	r->enabled = (in[0] & RULE_FLAG_ENABLED) ? 1 : 0;
	r->slot = in[1];
	r->channel = in[2];
	r->sensor_type = in[3];
	r->from_state = in[4];
	r->to_state = in[5];
	memcpy(&r->lo, &in[6], sizeof(float));
	memcpy(&r->hi, &in[10], sizeof(float));
	memcpy(&r->dwell, &in[14], sizeof(float));
	return true;
}

/* ---- CRUD (rule-addressed) ---------------------------------------------- */

uint8_t app_alarm_rules_count(void)
{
	uint8_t n = 0;
	for (int i = 0; i < APP_ALARM_RULE_COUNT; i++) {
		if (m_rules[i].used) {
			n++;
		}
	}
	return n;
}

bool app_alarm_rules_get(uint8_t rule, struct app_alarm_rule *out)
{
	bool ok = false;

	/* M-6: copy the rule out under the rules lock instead of returning a raw
	 * pointer. The alarm poll otherwise read the cache holding only
	 * app_alarm.c's lock, racing reload_from_config() which rewrites the rule
	 * under this lock — a torn read that could evaluate a half-written rule. */
	k_mutex_lock(&m_lock, K_FOREVER);
	if (rule < APP_ALARM_RULE_COUNT && m_rules[rule].used) {
		*out = m_rules[rule].rule;
		ok = true;
	}
	k_mutex_unlock(&m_lock);
	return ok;
}

bool app_alarm_rules_occupied(uint8_t rule)
{
	return rule < APP_ALARM_RULE_COUNT && m_rules[rule].used;
}

int app_alarm_rules_first_free(void)
{
	for (int i = 0; i < APP_ALARM_RULE_COUNT; i++) {
		if (!m_rules[i].used) {
			return i;
		}
	}
	return -1;
}

int app_alarm_rules_set(uint8_t rule, const struct app_alarm_rule *r)
{
	if (rule >= APP_ALARM_RULE_COUNT || r == NULL || !rule_valid(r)) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	pack_rule(r, rule_field(app_config(), rule));
	/* Normalized through the packed form, so padding and the enabled flag
	 * compare equal to a reload of the same bytes. */
	(void)unpack_rule(rule_field(app_config(), rule), &m_rules[rule].rule);
	m_rules[rule].used = true;
	k_mutex_unlock(&m_lock);
	return 0;
}

int app_alarm_rules_clear(uint8_t rule)
{
	if (rule >= APP_ALARM_RULE_COUNT) {
		return -EINVAL;
	}
	k_mutex_lock(&m_lock, K_FOREVER);
	int ret = m_rules[rule].used ? 0 : -ENOENT;
	memset(rule_field(app_config(), rule), 0, RULE_PACK_LEN);
	m_rules[rule].used = false;
	k_mutex_unlock(&m_lock);
	return ret;
}

void app_alarm_rules_clear_all(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	struct app_config *c = app_config();
	for (int i = 0; i < APP_ALARM_RULE_COUNT; i++) {
		memset(rule_field(c, (uint8_t)i), 0, RULE_PACK_LEN);
		m_rules[i].used = false;
	}
	k_mutex_unlock(&m_lock);
}

/* ---- persistence (app_config storage) ----------------------------------- */

int app_alarm_rules_stale_count(void)
{
	const struct app_config *c = app_config();
	int n = 0;

	k_mutex_lock(&m_lock, K_FOREVER);
	for (int i = 0; i < APP_ALARM_RULE_COUNT; i++) {
		if (m_rules[i].used && rule_stale_in(c, &m_rules[i].rule)) {
			n++;
		}
	}
	k_mutex_unlock(&m_lock);
	return n;
}

int app_alarm_rules_reload_from_config(void)
{
	struct app_config *c = app_config();
	int faults = 0; /* invalid rules sanitized + stale rules kept */

	k_mutex_lock(&m_lock, K_FOREVER);
	for (uint8_t i = 0; i < APP_ALARM_RULE_COUNT; i++) {
		struct app_alarm_rule r;
		uint8_t *field = rule_field(c, i);

		if (unpack_rule(field, &r) && rule_valid(&r)) {
			/* A stale rule (#430: its sensor type is not the slot's staged
			 * type) is kept — the type may be provisioned or put back —
			 * but reported, so a SetParam does not ACK a rule that cannot
			 * fire. */
			if (rule_stale_in(c, &r)) {
				LOG_WRN("Alarm rule %u: stale (type %u, slot %u has %u)", i,
					r.sensor_type, r.slot, slot_type_in(c, r.slot));
				faults++;
			}
			m_rules[i].rule = r;
			m_rules[i].used = true;
		} else {
			/* Zero the persisted bytes too, not just the live cache, so a
			 * rejected rule doesn't linger in NVS and get echoed by
			 * GetParam/dump (#197). Non-empty-but-invalid is the case worth
			 * a log (and worth reporting to the caller — H-10). */
			if (field[0] & RULE_FLAG_PRESENT) {
				LOG_WRN("Alarm rule %u: invalid persisted rule sanitized", i);
				faults++;
			}
			memset(field, 0, RULE_PACK_LEN);
			m_rules[i].used = false;
		}
	}
	k_mutex_unlock(&m_lock);

	LOG_INF("Loaded %u alarm rule(s)", app_alarm_rules_count());
	return faults;
}

int app_alarm_rules_save(void)
{
	/* Rules live in the app_config entries; persist the config (no reboot),
	 * clear of any radio exchange. */
	app_radio_flash_hold();
	int ret = settings_save();
	app_radio_flash_release();
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
