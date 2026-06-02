/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config_ingest.h"
#include "app_config.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_config_ingest, LOG_LEVEL_DBG);

/* Record the first offending proto field tag and mark the result invalid. The
 * apply still processes the remaining fields (best effort) so NFC ingest keeps
 * its "skip invalid, apply the rest" behaviour; SetParam inspects fault_field. */
#define FAULT(tag)                                                                                 \
	do {                                                                                       \
		if (fault_field && *fault_field == 0) {                                            \
			*fault_field = (tag);                                                      \
		}                                                                                  \
		ret = -EINVAL;                                                                     \
	} while (0)

#define APPLY_BOOL(field)                                                                          \
	if (src->has_##field) {                                                                    \
		config->field = src->field;                                                        \
	}

#define APPLY_FLOAT(field, lo, hi, tag)                                                            \
	if (src->has_##field) {                                                                    \
		if (src->field >= (lo) && src->field <= (hi)) {                                    \
			config->field = src->field;                                                \
		} else {                                                                           \
			LOG_WRN("Invalid " #field);                                                 \
			FAULT(tag);                                                                \
		}                                                                                  \
	}

static bool parse_hex_string(const char *hex_str, uint8_t *buf, size_t buf_len)
{
	if (!hex_str || !buf) {
		return false;
	}

	size_t str_len = strlen(hex_str);
	if (str_len != 2 * buf_len) {
		LOG_ERR("Invalid hex string length: expected %zu, got %zu", 2 * buf_len, str_len);
		return false;
	}

	size_t ret = hex2bin(hex_str, str_len, buf, buf_len);
	if (!ret) {
		LOG_ERR_CALL_FAILED("hex2bin");
		return false;
	}

	return true;
}

int app_config_apply_lorawan(const AppConfigMessage_Lorawan *src, uint32_t *fault_field)
{
	struct app_config *config = app_config();
	int ret = 0;

	if (fault_field) {
		*fault_field = 0;
	}

	if (src->has_region) {
		if ((int)src->region >= 0 && (int)src->region <= (int)APP_CONFIG_LRW_REGION_AU915) {
			config->lrw_region = (enum app_config_lrw_region)src->region;
		} else {
			FAULT(1);
		}
	}
	if (src->has_network) {
		if ((int)src->network >= 0 &&
		    (int)src->network <= (int)APP_CONFIG_LRW_NETWORK_PRIVATE) {
			config->lrw_network = (enum app_config_lrw_network)src->network;
		} else {
			FAULT(2);
		}
	}
	if (src->has_adr) {
		config->lrw_adr = src->adr;
	}
	if (src->has_activation) {
		if ((int)src->activation >= 0 &&
		    (int)src->activation <= (int)APP_CONFIG_LRW_ACTIVATION_ABP) {
			config->lrw_activation = (enum app_config_lrw_activation)src->activation;
		} else {
			FAULT(4);
		}
	}
	if (src->has_deveui &&
	    !parse_hex_string(src->deveui, config->lrw_deveui, sizeof(config->lrw_deveui))) {
		FAULT(5);
	}
	if (src->has_joineui &&
	    !parse_hex_string(src->joineui, config->lrw_joineui, sizeof(config->lrw_joineui))) {
		FAULT(6);
	}
	if (src->has_nwkkey &&
	    !parse_hex_string(src->nwkkey, config->lrw_nwkkey, sizeof(config->lrw_nwkkey))) {
		FAULT(7);
	}
	if (src->has_appkey &&
	    !parse_hex_string(src->appkey, config->lrw_appkey, sizeof(config->lrw_appkey))) {
		FAULT(8);
	}
	if (src->has_devaddr &&
	    !parse_hex_string(src->devaddr, config->lrw_devaddr, sizeof(config->lrw_devaddr))) {
		FAULT(9);
	}
	if (src->has_nwkskey &&
	    !parse_hex_string(src->nwkskey, config->lrw_nwkskey, sizeof(config->lrw_nwkskey))) {
		FAULT(10);
	}
	if (src->has_appskey &&
	    !parse_hex_string(src->appskey, config->lrw_appskey, sizeof(config->lrw_appskey))) {
		FAULT(11);
	}
	if (src->has_sub_band) {
		if (src->sub_band <= 8) {
			config->lrw_sub_band = (int)src->sub_band;
		} else {
			FAULT(12);
		}
	}

	return ret;
}

int app_config_apply_application(const AppConfigMessage_Application *src, uint32_t *fault_field)
{
	struct app_config *config = app_config();
	int ret = 0;

	if (fault_field) {
		*fault_field = 0;
	}

	APPLY_BOOL(calibration);

	if (src->has_interval_sample) {
		int val = src->interval_sample;

		if (val == 0 || (val >= 5 && val <= 3600)) {
			config->interval_sample = val;
		} else {
			FAULT(2);
		}
	}
	/* interval_aggreg (tag 3) has no app_config counterpart — unsupported */
	if (src->has_interval_report) {
		int val = src->interval_report;

		if (val >= 60 && val <= 86400) {
			config->interval_report = val;
		} else {
			FAULT(4);
		}
	}

	APPLY_BOOL(alarm_temperature_enabled);
	APPLY_FLOAT(alarm_temperature_lo, -30.0f, 70.0f, 6);
	APPLY_FLOAT(alarm_temperature_hi, -30.0f, 70.0f, 7);
	APPLY_FLOAT(alarm_temperature_hst, 0.0f, 5.0f, 8);
	APPLY_BOOL(alarm_humidity_enabled);
	APPLY_FLOAT(alarm_humidity_lo, 0.0f, 100.0f, 10);
	APPLY_FLOAT(alarm_humidity_hi, 0.0f, 100.0f, 11);
	APPLY_FLOAT(alarm_humidity_hst, 0.0f, 20.0f, 12);
	APPLY_BOOL(alarm_pressure_enabled);
	APPLY_FLOAT(alarm_pressure_lo, 500.0f, 1200.0f, 14);
	APPLY_FLOAT(alarm_pressure_hi, 500.0f, 1200.0f, 15);
	APPLY_FLOAT(alarm_pressure_hst, 0.0f, 50.0f, 16);
	APPLY_BOOL(alarm_t1_temperature_enabled);
	APPLY_FLOAT(alarm_t1_temperature_lo, -30.0f, 70.0f, 18);
	APPLY_FLOAT(alarm_t1_temperature_hi, -30.0f, 70.0f, 19);
	APPLY_FLOAT(alarm_t1_temperature_hst, 0.0f, 5.0f, 20);
	APPLY_BOOL(alarm_t2_temperature_enabled);
	APPLY_FLOAT(alarm_t2_temperature_lo, -30.0f, 70.0f, 22);
	APPLY_FLOAT(alarm_t2_temperature_hi, -30.0f, 70.0f, 23);
	APPLY_FLOAT(alarm_t2_temperature_hst, 0.0f, 5.0f, 24);

	APPLY_BOOL(hall_left_counter);
	APPLY_BOOL(hall_left_notify_act);
	APPLY_BOOL(hall_left_notify_deact);
	APPLY_BOOL(hall_right_counter);
	APPLY_BOOL(hall_right_notify_act);
	APPLY_BOOL(hall_right_notify_deact);
	APPLY_BOOL(input_a_counter);
	APPLY_BOOL(input_a_notify_act);
	APPLY_BOOL(input_a_notify_deact);
	APPLY_BOOL(input_b_counter);
	APPLY_BOOL(input_b_notify_act);
	APPLY_BOOL(input_b_notify_deact);

	APPLY_FLOAT(corr_temperature, -5.0f, 5.0f, 37);
	APPLY_FLOAT(corr_t1_temperature, -5.0f, 5.0f, 38);
	APPLY_FLOAT(corr_t2_temperature, -5.0f, 5.0f, 39);

	APPLY_BOOL(cap_hall_left);
	APPLY_BOOL(cap_hall_right);
	APPLY_BOOL(cap_input_a);
	APPLY_BOOL(cap_input_b);
	APPLY_BOOL(cap_light_sensor);
	APPLY_BOOL(cap_barometer);
	APPLY_BOOL(cap_pir_detector);
	APPLY_BOOL(cap_1w_thermometer);
	APPLY_BOOL(cap_1w_machine_probe);

	/* Cross-validate alarm lo/hi pairs — disable the alarm if lo >= hi */
	if (config->alarm_temperature_lo >= config->alarm_temperature_hi) {
		config->alarm_temperature_enabled = false;
	}
	if (config->alarm_humidity_lo >= config->alarm_humidity_hi) {
		config->alarm_humidity_enabled = false;
	}
	if (config->alarm_pressure_lo >= config->alarm_pressure_hi) {
		config->alarm_pressure_enabled = false;
	}
	if (config->alarm_t1_temperature_lo >= config->alarm_t1_temperature_hi) {
		config->alarm_t1_temperature_enabled = false;
	}
	if (config->alarm_t2_temperature_lo >= config->alarm_t2_temperature_hi) {
		config->alarm_t2_temperature_enabled = false;
	}

	return ret;
}

static bool requested(const uint32_t *ids, size_t n, uint32_t tag)
{
	for (size_t i = 0; i < n; i++) {
		if (ids[i] == tag) {
			return true;
		}
	}
	return false;
}

void app_config_fill_lorawan(AppConfigMessage_Lorawan *dst, const uint32_t *ids, size_t n)
{
	const struct app_config *c = app_config();

	if (requested(ids, n, 1)) {
		dst->has_region = true;
		dst->region = (AppConfigMessage_Lorawan_Region)c->lrw_region;
	}
	if (requested(ids, n, 2)) {
		dst->has_network = true;
		dst->network = (AppConfigMessage_Lorawan_Network)c->lrw_network;
	}
	if (requested(ids, n, 3)) {
		dst->has_adr = true;
		dst->adr = c->lrw_adr;
	}
	if (requested(ids, n, 4)) {
		dst->has_activation = true;
		dst->activation = (AppConfigMessage_Lorawan_Activation)c->lrw_activation;
	}
	if (requested(ids, n, 5)) {
		dst->has_deveui = true;
		bin2hex(c->lrw_deveui, sizeof(c->lrw_deveui), dst->deveui, sizeof(dst->deveui));
	}
	if (requested(ids, n, 6)) {
		dst->has_joineui = true;
		bin2hex(c->lrw_joineui, sizeof(c->lrw_joineui), dst->joineui, sizeof(dst->joineui));
	}
	/* nwkkey/appkey/nwkskey/appskey (tags 7,8,10,11) are secrets — not dumped */
	if (requested(ids, n, 9)) {
		dst->has_devaddr = true;
		bin2hex(c->lrw_devaddr, sizeof(c->lrw_devaddr), dst->devaddr, sizeof(dst->devaddr));
	}
	if (requested(ids, n, 12)) {
		dst->has_sub_band = true;
		dst->sub_band = (uint32_t)c->lrw_sub_band;
	}
}

#define FILL_BOOL(tag, field)                                                                      \
	if (requested(ids, n, tag)) {                                                              \
		dst->has_##field = true;                                                            \
		dst->field = c->field;                                                             \
	}
#define FILL_NUM(tag, field)                                                                       \
	if (requested(ids, n, tag)) {                                                              \
		dst->has_##field = true;                                                            \
		dst->field = c->field;                                                             \
	}

void app_config_fill_application(AppConfigMessage_Application *dst, const uint32_t *ids, size_t n)
{
	const struct app_config *c = app_config();

	FILL_BOOL(1, calibration);
	FILL_NUM(2, interval_sample);
	FILL_NUM(4, interval_report);
	FILL_BOOL(5, alarm_temperature_enabled);
	FILL_NUM(6, alarm_temperature_lo);
	FILL_NUM(7, alarm_temperature_hi);
	FILL_NUM(8, alarm_temperature_hst);
	FILL_BOOL(9, alarm_humidity_enabled);
	FILL_NUM(10, alarm_humidity_lo);
	FILL_NUM(11, alarm_humidity_hi);
	FILL_NUM(12, alarm_humidity_hst);
	FILL_BOOL(13, alarm_pressure_enabled);
	FILL_NUM(14, alarm_pressure_lo);
	FILL_NUM(15, alarm_pressure_hi);
	FILL_NUM(16, alarm_pressure_hst);
	FILL_BOOL(17, alarm_t1_temperature_enabled);
	FILL_NUM(18, alarm_t1_temperature_lo);
	FILL_NUM(19, alarm_t1_temperature_hi);
	FILL_NUM(20, alarm_t1_temperature_hst);
	FILL_BOOL(21, alarm_t2_temperature_enabled);
	FILL_NUM(22, alarm_t2_temperature_lo);
	FILL_NUM(23, alarm_t2_temperature_hi);
	FILL_NUM(24, alarm_t2_temperature_hst);
	FILL_BOOL(25, hall_left_counter);
	FILL_BOOL(26, hall_left_notify_act);
	FILL_BOOL(27, hall_left_notify_deact);
	FILL_BOOL(28, hall_right_counter);
	FILL_BOOL(29, hall_right_notify_act);
	FILL_BOOL(30, hall_right_notify_deact);
	FILL_BOOL(31, input_a_counter);
	FILL_BOOL(32, input_a_notify_act);
	FILL_BOOL(33, input_a_notify_deact);
	FILL_BOOL(34, input_b_counter);
	FILL_BOOL(35, input_b_notify_act);
	FILL_BOOL(36, input_b_notify_deact);
	FILL_NUM(37, corr_temperature);
	FILL_NUM(38, corr_t1_temperature);
	FILL_NUM(39, corr_t2_temperature);
	FILL_BOOL(40, cap_hall_left);
	FILL_BOOL(41, cap_hall_right);
	FILL_BOOL(42, cap_input_a);
	FILL_BOOL(43, cap_input_b);
	FILL_BOOL(44, cap_light_sensor);
	FILL_BOOL(45, cap_barometer);
	FILL_BOOL(46, cap_pir_detector);
	FILL_BOOL(47, cap_1w_thermometer);
	FILL_BOOL(48, cap_1w_machine_probe);
}

bool app_config_ingest(const AppConfigMessage *message)
{
	if (message->has_factory && message->factory) {
		LOG_INF("Factory reset requested via NFC");
		return true;
	}

	/* NFC keeps best-effort semantics: apply valid fields, ignore invalid. */
	if (message->has_lorawan) {
		app_config_apply_lorawan(&message->lorawan, NULL);
	}
	if (message->has_application) {
		app_config_apply_application(&message->application, NULL);
	}

	return false;
}
