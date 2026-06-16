/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm_rules.h"
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

#define SETTINGS_SUBTREE "alarm"
#define SETTINGS_KEY     "alarm/rules"
#define BLOB_VERSION_V1  1 /* legacy: count + compacted list of rules */
#define BLOB_VERSION     2 /* current: sparse (slot, rule) entries */

struct app_alarm_slot {
	bool used;
	struct app_alarm_rule rule;
};

static struct app_alarm_slot m_slots[APP_ALARM_SLOT_COUNT];
static struct k_mutex m_lock;

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
				  (enum app_alarm_quantity)rule->quantity)) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
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
	m_slots[slot].used = false;
	k_mutex_unlock(&m_lock);
	return ret;
}

void app_alarm_rules_clear_all(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	for (int i = 0; i < APP_ALARM_SLOT_COUNT; i++) {
		m_slots[i].used = false;
	}
	k_mutex_unlock(&m_lock);
}

/* ---- persistence (settings blob) --------------------------------------- */

/* v1 (legacy): version + count + a compacted list of rules. Migrated on load
 * into slots 0..count-1. */
struct rules_blob_v1 {
	uint8_t version;
	uint8_t count;
	struct app_alarm_rule rules[APP_ALARM_SLOT_COUNT];
};

/* v2 (current): version + count + a sparse list of (slot, rule) entries. Only
 * occupied slots are stored, so an empty table is 2 bytes and the blob never
 * exceeds the v1 size. */
struct rules_entry_v2 {
	uint8_t slot;
	struct app_alarm_rule rule;
};

struct rules_blob_v2 {
	uint8_t version;
	uint8_t count;
	struct rules_entry_v2 entries[APP_ALARM_SLOT_COUNT];
};

/* Adopt one (slot, rule) into m_slots if the slot and pair are valid (drops
 * anything that no longer validates, e.g. enum changed across FW). */
static void load_one(uint8_t slot, const struct app_alarm_rule *r)
{
	if (slot < APP_ALARM_SLOT_COUNT &&
	    app_alarm_rule_valid((enum app_alarm_source)r->source,
				 (enum app_alarm_quantity)r->quantity)) {
		m_slots[slot].rule = *r;
		m_slots[slot].used = true;
	}
}

static int rules_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	if (!settings_name_steq(name, "rules", &next) || next) {
		return -ENOENT;
	}

	union {
		uint8_t version;
		struct rules_blob_v1 v1;
		struct rules_blob_v2 v2;
	} blob;
	if (len > sizeof(blob) || len < 2) {
		LOG_WRN("alarm rules blob ignored (len=%d)", (int)len);
		return 0;
	}
	ssize_t n = read_cb(cb_arg, &blob, len);
	if (n < 2) {
		LOG_WRN("alarm rules blob short read (n=%d)", (int)n);
		return 0;
	}

	for (int i = 0; i < APP_ALARM_SLOT_COUNT; i++) {
		m_slots[i].used = false;
	}

	if (blob.version == BLOB_VERSION) {
		uint8_t count = MIN(blob.v2.count, (uint8_t)APP_ALARM_SLOT_COUNT);
		for (int i = 0; i < count; i++) {
			load_one(blob.v2.entries[i].slot, &blob.v2.entries[i].rule);
		}
	} else if (blob.version == BLOB_VERSION_V1) {
		uint8_t count = MIN(blob.v1.count, (uint8_t)APP_ALARM_SLOT_COUNT);
		for (int i = 0; i < count; i++) {
			load_one((uint8_t)i, &blob.v1.rules[i]);
		}
	} else {
		LOG_WRN("alarm rules blob ignored (version=%u)", blob.version);
		return 0;
	}

	LOG_INF("Loaded %u alarm rule(s)", app_alarm_rules_count());
	return 0;
}

static struct settings_handler m_sh = {
	.name = SETTINGS_SUBTREE,
	.h_set = rules_settings_set,
};

int app_alarm_rules_save(void)
{
	struct rules_blob_v2 blob = {.version = BLOB_VERSION, .count = 0};

	k_mutex_lock(&m_lock, K_FOREVER);
	for (uint8_t s = 0; s < APP_ALARM_SLOT_COUNT; s++) {
		if (m_slots[s].used) {
			blob.entries[blob.count].slot = s;
			blob.entries[blob.count].rule = m_slots[s].rule;
			blob.count++;
		}
	}
	size_t len = offsetof(struct rules_blob_v2, entries) +
		     (size_t)blob.count * sizeof(blob.entries[0]);
	k_mutex_unlock(&m_lock);

	int ret = settings_save_one(SETTINGS_KEY, &blob, len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save_one", ret);
	}
	return ret;
}

int app_alarm_rules_init(void)
{
	k_mutex_init(&m_lock);

	int ret = settings_register(&m_sh);
	if (ret && ret != -EEXIST) {
		LOG_ERR_CALL_FAILED_INT("settings_register", ret);
		return ret;
	}
	ret = settings_load_subtree(SETTINGS_SUBTREE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_load_subtree", ret);
	}
	return ret;
}
