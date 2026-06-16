/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_counters.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

/* Standard includes */
#include <errno.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_counters, LOG_LEVEL_INF);

#define SETTINGS_SUBTREE "counters"
#define SETTINGS_KEY     "counters/totals"
#define BLOB_VERSION     1

/* Serialized form stored in NVS. uint32 mirrors the width used throughout the
 * pipeline (sensor data / history / telemetry). */
struct counters_blob {
	uint8_t version;
	uint8_t _pad[3];
	uint32_t hall_left;
	uint32_t hall_right;
	uint32_t input_a;
	uint32_t input_b;
};

/* Snapshot of the last value written to NVS, used as the dirty-flag baseline. */
static struct counters_blob m_last_saved = {.version = BLOB_VERSION};
static bool m_loaded;

/* Collect the live totalizers from app_hall / app_input into a blob. */
static void gather(struct counters_blob *blob)
{
	struct app_hall_data hall = {0};
	struct app_input_data input = {0};

	(void)app_hall_get_data(&hall);
	(void)app_input_get_data(&input);

	blob->version = BLOB_VERSION;
	blob->_pad[0] = blob->_pad[1] = blob->_pad[2] = 0;
	blob->hall_left = hall.left_count;
	blob->hall_right = hall.right_count;
	blob->input_a = input.input_a_count;
	blob->input_b = input.input_b_count;
}

static int counters_settings_set(const char *name, size_t len, settings_read_cb read_cb,
				 void *cb_arg)
{
	const char *next;
	if (!settings_name_steq(name, "totals", &next) || next) {
		return -ENOENT;
	}

	struct counters_blob blob;
	if (len != sizeof(blob)) {
		LOG_WRN("counters blob ignored (len=%d)", (int)len);
		return 0;
	}
	ssize_t n = read_cb(cb_arg, &blob, len);
	if (n != (ssize_t)sizeof(blob)) {
		LOG_WRN("counters blob short read (n=%d)", (int)n);
		return 0;
	}
	if (blob.version != BLOB_VERSION) {
		LOG_WRN("counters blob ignored (version=%u)", blob.version);
		return 0;
	}

	m_last_saved = blob;
	m_loaded = true;
	return 0;
}

static struct settings_handler m_sh = {
	.name = SETTINGS_SUBTREE,
	.h_set = counters_settings_set,
};

int app_counters_save(bool force)
{
	struct counters_blob blob;
	gather(&blob);

	if (!force && blob.hall_left == m_last_saved.hall_left &&
	    blob.hall_right == m_last_saved.hall_right && blob.input_a == m_last_saved.input_a &&
	    blob.input_b == m_last_saved.input_b) {
		/* Nothing changed since the last save — skip the write (flash wear). */
		return 0;
	}

	int ret = settings_save_one(SETTINGS_KEY, &blob, sizeof(blob));
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save_one", ret);
		return ret;
	}

	m_last_saved = blob;
	LOG_DBG("Counters persisted: hl=%u hr=%u a=%u b=%u", blob.hall_left, blob.hall_right,
		blob.input_a, blob.input_b);
	return 0;
}

int app_counters_init(void)
{
	int ret = settings_register(&m_sh);
	if (ret && ret != -EEXIST) {
		LOG_ERR_CALL_FAILED_INT("settings_register", ret);
		return ret;
	}

	ret = settings_load_subtree(SETTINGS_SUBTREE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_load_subtree", ret);
		return ret;
	}

	if (m_loaded) {
		/* Restore the totalizers into the live modules. Safe because this
		 * runs after app_hall_init / app_input_init. */
		app_hall_set_counts(m_last_saved.hall_left, m_last_saved.hall_right);
		app_input_set_counts(m_last_saved.input_a, m_last_saved.input_b);
		LOG_INF("Counters restored: hl=%u hr=%u a=%u b=%u", m_last_saved.hall_left,
			m_last_saved.hall_right, m_last_saved.input_a, m_last_saved.input_b);
	}

	return 0;
}
