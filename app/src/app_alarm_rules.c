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

/* Standard includes */
#include <errno.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_alarm_rules, LOG_LEVEL_INF);

#define SETTINGS_SUBTREE "alarm"
#define SETTINGS_KEY     "alarm/rules"
#define BLOB_VERSION     1

static struct app_alarm_rule m_rules[APP_ALARM_RULE_MAX];
static uint8_t m_count;
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

/* ---- CRUD --------------------------------------------------------------- */

uint8_t app_alarm_rules_count(void)
{
	return m_count;
}

const struct app_alarm_rule *app_alarm_rules_get(uint8_t idx)
{
	return (idx < m_count) ? &m_rules[idx] : NULL;
}

static int find_idx(enum app_alarm_source source, enum app_alarm_quantity quantity)
{
	for (int i = 0; i < m_count; i++) {
		if (m_rules[i].source == source && m_rules[i].quantity == quantity) {
			return i;
		}
	}
	return -1;
}

const struct app_alarm_rule *app_alarm_rules_find(enum app_alarm_source source,
						  enum app_alarm_quantity quantity)
{
	int i = find_idx(source, quantity);
	return (i >= 0) ? &m_rules[i] : NULL;
}

int app_alarm_rules_set(const struct app_alarm_rule *rule)
{
	if (rule == NULL || !app_alarm_rule_valid((enum app_alarm_source)rule->source,
						  (enum app_alarm_quantity)rule->quantity)) {
		return -EINVAL;
	}

	k_mutex_lock(&m_lock, K_FOREVER);
	int i = find_idx((enum app_alarm_source)rule->source,
			 (enum app_alarm_quantity)rule->quantity);
	if (i < 0) {
		if (m_count >= APP_ALARM_RULE_MAX) {
			k_mutex_unlock(&m_lock);
			return -ENOSPC;
		}
		i = m_count++;
	}
	m_rules[i] = *rule;
	k_mutex_unlock(&m_lock);
	return 0;
}

int app_alarm_rules_clear(enum app_alarm_source source, enum app_alarm_quantity quantity)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	int i = find_idx(source, quantity);
	if (i < 0) {
		k_mutex_unlock(&m_lock);
		return -ENOENT;
	}
	/* Compact: move the last rule into the hole. */
	m_rules[i] = m_rules[--m_count];
	k_mutex_unlock(&m_lock);
	return 0;
}

void app_alarm_rules_clear_all(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	m_count = 0;
	k_mutex_unlock(&m_lock);
}

/* ---- persistence (settings blob) --------------------------------------- */

struct rules_blob {
	uint8_t version;
	uint8_t count;
	struct app_alarm_rule rules[APP_ALARM_RULE_MAX];
};

static int rules_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	if (!settings_name_steq(name, "rules", &next) || next) {
		return -ENOENT;
	}

	struct rules_blob blob;
	if (len > sizeof(blob)) {
		return -EINVAL;
	}
	ssize_t n = read_cb(cb_arg, &blob, len);
	if (n < (ssize_t)offsetof(struct rules_blob, rules) || blob.version != BLOB_VERSION) {
		LOG_WRN("alarm rules blob ignored (len=%d, version=%u)", (int)n, blob.version);
		return 0;
	}
	if (blob.count > APP_ALARM_RULE_MAX) {
		blob.count = APP_ALARM_RULE_MAX;
	}
	/* Drop any rule that no longer validates (e.g. enum changed across FW). */
	m_count = 0;
	for (int i = 0; i < blob.count; i++) {
		if (app_alarm_rule_valid((enum app_alarm_source)blob.rules[i].source,
					 (enum app_alarm_quantity)blob.rules[i].quantity)) {
			m_rules[m_count++] = blob.rules[i];
		}
	}
	LOG_INF("Loaded %u alarm rule(s)", m_count);
	return 0;
}

static struct settings_handler m_sh = {
	.name = SETTINGS_SUBTREE,
	.h_set = rules_settings_set,
};

int app_alarm_rules_save(void)
{
	k_mutex_lock(&m_lock, K_FOREVER);
	struct rules_blob blob = {.version = BLOB_VERSION, .count = m_count};
	memcpy(blob.rules, m_rules, (size_t)m_count * sizeof(m_rules[0]));
	size_t len = offsetof(struct rules_blob, rules) + (size_t)m_count * sizeof(m_rules[0]);
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
