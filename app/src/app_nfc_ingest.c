/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_nfc_ingest.h"
#include "app_config.h"
#include "app_log.h"
#include "app_settings.h"

/* Zephyr includes */
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_nfc_ingest, LOG_LEVEL_DBG);

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

void app_nfc_ingest(const NfcConfigMessage *message)
{
	struct app_config *config = app_config();

	if (message->has_factory) {
		LOG_INF_PARAM_BOOL("factory", message->factory);
		if (message->factory) {
			LOG_INF("Factory reset requested via NFC");
			app_settings_reset();
		}
	}

	if (message->has_lorawan) {
		if (message->lorawan.has_region) {
			LOG_INF_PARAM_INT("lorawan.region", message->lorawan.region);
			config->lrw_region = (enum app_config_lrw_region)message->lorawan.region;
		}

		if (message->lorawan.has_network) {
			LOG_INF_PARAM_INT("lorawan.network", message->lorawan.network);
			config->lrw_network = (enum app_config_lrw_network)message->lorawan.network;
		}

		if (message->lorawan.has_adr) {
			LOG_INF_PARAM_BOOL("lorawan.adr", message->lorawan.adr);
			config->lrw_adr = message->lorawan.adr;
		}

		if (message->lorawan.has_activation) {
			LOG_INF_PARAM_INT("lorawan.activation", message->lorawan.activation);
			config->lrw_activation =
				(enum app_config_lrw_activation)message->lorawan.activation;
		}

		if (message->lorawan.has_deveui) {
			LOG_INF_PARAM_STR("lorawan.deveui", message->lorawan.deveui);
			if (!parse_hex_string(message->lorawan.deveui, config->lrw_deveui,
					      sizeof(config->lrw_deveui))) {
				LOG_ERR("Failed to parse lorawan.deveui");
			}
		}

		if (message->lorawan.has_joineui) {
			LOG_INF_PARAM_STR("lorawan.joineui", message->lorawan.joineui);
			if (!parse_hex_string(message->lorawan.joineui, config->lrw_joineui,
					      sizeof(config->lrw_joineui))) {
				LOG_ERR("Failed to parse lorawan.joineui");
			}
		}

		if (message->lorawan.has_nwkkey) {
			LOG_INF_PARAM_STR("lorawan.nwkkey", message->lorawan.nwkkey);
			if (!parse_hex_string(message->lorawan.nwkkey, config->lrw_nwkkey,
					      sizeof(config->lrw_nwkkey))) {
				LOG_ERR("Failed to parse lorawan.nwkkey");
			}
		}

		if (message->lorawan.has_appkey) {
			LOG_INF_PARAM_STR("lorawan.appkey", message->lorawan.appkey);
			if (!parse_hex_string(message->lorawan.appkey, config->lrw_appkey,
					      sizeof(config->lrw_appkey))) {
				LOG_ERR("Failed to parse lorawan.appkey");
			}
		}

		if (message->lorawan.has_devaddr) {
			LOG_INF_PARAM_STR("lorawan.devaddr", message->lorawan.devaddr);
			if (!parse_hex_string(message->lorawan.devaddr, config->lrw_devaddr,
					      sizeof(config->lrw_devaddr))) {
				LOG_ERR("Failed to parse lorawan.devaddr");
			}
		}

		if (message->lorawan.has_nwkskey) {
			LOG_INF_PARAM_STR("lorawan.nwkskey", message->lorawan.nwkskey);
			if (!parse_hex_string(message->lorawan.nwkskey, config->lrw_nwkskey,
					      sizeof(config->lrw_nwkskey))) {
				LOG_ERR("Failed to parse lorawan.nwkskey");
			}
		}

		if (message->lorawan.has_appskey) {
			LOG_INF_PARAM_STR("lorawan.appskey", message->lorawan.appskey);
			if (!parse_hex_string(message->lorawan.appskey, config->lrw_appskey,
					      sizeof(config->lrw_appskey))) {
				LOG_ERR("Failed to parse lorawan.appskey");
			}
		}
	}

	if (message->has_application) {
		if (message->application.has_calibration) {
			LOG_INF_PARAM_BOOL("application.calibration",
					   message->application.calibration);
			config->calibration = message->application.calibration;
		}

		if (message->application.has_interval_sample) {
			LOG_INF_PARAM_INT("application.interval_sample",
					  message->application.interval_sample);
			config->interval_sample = message->application.interval_sample;
		}

		if (message->application.has_interval_report) {
			LOG_INF_PARAM_INT("application.interval_report",
					  message->application.interval_report);
			config->interval_report = message->application.interval_report;
		}

		if (message->application.has_alarm_temperature_enabled) {
			LOG_INF_PARAM_BOOL("application.alarm_temperature_enabled",
					   message->application.alarm_temperature_enabled);
			config->alarm_temperature_enabled =
				message->application.alarm_temperature_enabled;
		}

		if (message->application.has_alarm_temperature_lo) {
			LOG_INF_PARAM_FLOAT("application.alarm_temperature_lo",
					    message->application.alarm_temperature_lo);
			config->alarm_temperature_lo = message->application.alarm_temperature_lo;
		}

		if (message->application.has_alarm_temperature_hi) {
			LOG_INF_PARAM_FLOAT("application.alarm_temperature_hi",
					    message->application.alarm_temperature_hi);
			config->alarm_temperature_hi = message->application.alarm_temperature_hi;
		}

		if (message->application.has_alarm_temperature_hst) {
			LOG_INF_PARAM_FLOAT("application.alarm_temperature_hst",
					    message->application.alarm_temperature_hst);
			config->alarm_temperature_hst = message->application.alarm_temperature_hst;
		}

		if (message->application.has_alarm_humidity_enabled) {
			LOG_INF_PARAM_BOOL("application.alarm_humidity_enabled",
					   message->application.alarm_humidity_enabled);
			config->alarm_humidity_enabled =
				message->application.alarm_humidity_enabled;
		}

		if (message->application.has_alarm_humidity_lo) {
			LOG_INF_PARAM_FLOAT("application.alarm_humidity_lo",
					    message->application.alarm_humidity_lo);
			config->alarm_humidity_lo = message->application.alarm_humidity_lo;
		}

		if (message->application.has_alarm_humidity_hi) {
			LOG_INF_PARAM_FLOAT("application.alarm_humidity_hi",
					    message->application.alarm_humidity_hi);
			config->alarm_humidity_hi = message->application.alarm_humidity_hi;
		}

		if (message->application.has_alarm_humidity_hst) {
			LOG_INF_PARAM_FLOAT("application.alarm_humidity_hst",
					    message->application.alarm_humidity_hst);
			config->alarm_humidity_hst = message->application.alarm_humidity_hst;
		}

		if (message->application.has_alarm_pressure_enabled) {
			LOG_INF_PARAM_BOOL("application.alarm_pressure_enabled",
					   message->application.alarm_pressure_enabled);
			config->alarm_pressure_enabled =
				message->application.alarm_pressure_enabled;
		}

		if (message->application.has_alarm_pressure_lo) {
			LOG_INF_PARAM_FLOAT("application.alarm_pressure_lo",
					    message->application.alarm_pressure_lo);
			config->alarm_pressure_lo = message->application.alarm_pressure_lo;
		}

		if (message->application.has_alarm_pressure_hi) {
			LOG_INF_PARAM_FLOAT("application.alarm_pressure_hi",
					    message->application.alarm_pressure_hi);
			config->alarm_pressure_hi = message->application.alarm_pressure_hi;
		}

		if (message->application.has_alarm_pressure_hst) {
			LOG_INF_PARAM_FLOAT("application.alarm_pressure_hst",
					    message->application.alarm_pressure_hst);
			config->alarm_pressure_hst = message->application.alarm_pressure_hst;
		}

		if (message->application.has_alarm_t1_temperature_enabled) {
			LOG_INF_PARAM_BOOL("application.alarm_t1_temperature_enabled",
					   message->application.alarm_t1_temperature_enabled);
			config->alarm_t1_temperature_enabled =
				message->application.alarm_t1_temperature_enabled;
		}

		if (message->application.has_alarm_t1_temperature_lo) {
			LOG_INF_PARAM_FLOAT("application.alarm_t1_temperature_lo",
					    message->application.alarm_t1_temperature_lo);
			config->alarm_t1_temperature_lo =
				message->application.alarm_t1_temperature_lo;
		}

		if (message->application.has_alarm_t1_temperature_hi) {
			LOG_INF_PARAM_FLOAT("application.alarm_t1_temperature_hi",
					    message->application.alarm_t1_temperature_hi);
			config->alarm_t1_temperature_hi =
				message->application.alarm_t1_temperature_hi;
		}

		if (message->application.has_alarm_t1_temperature_hst) {
			LOG_INF_PARAM_FLOAT("application.alarm_t1_temperature_hst",
					    message->application.alarm_t1_temperature_hst);
			config->alarm_t1_temperature_hst =
				message->application.alarm_t1_temperature_hst;
		}

		if (message->application.has_alarm_t2_temperature_enabled) {
			LOG_INF_PARAM_BOOL("application.alarm_t2_temperature_enabled",
					   message->application.alarm_t2_temperature_enabled);
			config->alarm_t2_temperature_enabled =
				message->application.alarm_t2_temperature_enabled;
		}

		if (message->application.has_alarm_t2_temperature_lo) {
			LOG_INF_PARAM_FLOAT("application.alarm_t2_temperature_lo",
					    message->application.alarm_t2_temperature_lo);
			config->alarm_t2_temperature_lo =
				message->application.alarm_t2_temperature_lo;
		}

		if (message->application.has_alarm_t2_temperature_hi) {
			LOG_INF_PARAM_FLOAT("application.alarm_t2_temperature_hi",
					    message->application.alarm_t2_temperature_hi);
			config->alarm_t2_temperature_hi =
				message->application.alarm_t2_temperature_hi;
		}

		if (message->application.has_alarm_t2_temperature_hst) {
			LOG_INF_PARAM_FLOAT("application.alarm_t2_temperature_hst",
					    message->application.alarm_t2_temperature_hst);
			config->alarm_t2_temperature_hst =
				message->application.alarm_t2_temperature_hst;
		}

		if (message->application.has_hall_left_counter) {
			LOG_INF_PARAM_BOOL("application.hall_left_counter",
					   message->application.hall_left_counter);
			config->hall_left_counter = message->application.hall_left_counter;
		}

		if (message->application.has_hall_left_notify_act) {
			LOG_INF_PARAM_BOOL("application.hall_left_notify_act",
					   message->application.hall_left_notify_act);
			config->hall_left_notify_act = message->application.hall_left_notify_act;
		}

		if (message->application.has_hall_left_notify_deact) {
			LOG_INF_PARAM_BOOL("application.hall_left_notify_deact",
					   message->application.hall_left_notify_deact);
			config->hall_left_notify_deact =
				message->application.hall_left_notify_deact;
		}

		if (message->application.has_hall_right_counter) {
			LOG_INF_PARAM_BOOL("application.hall_right_counter",
					   message->application.hall_right_counter);
			config->hall_right_counter = message->application.hall_right_counter;
		}

		if (message->application.has_hall_right_notify_act) {
			LOG_INF_PARAM_BOOL("application.hall_right_notify_act",
					   message->application.hall_right_notify_act);
			config->hall_right_notify_act = message->application.hall_right_notify_act;
		}

		if (message->application.has_hall_right_notify_deact) {
			LOG_INF_PARAM_BOOL("application.hall_right_notify_deact",
					   message->application.hall_right_notify_deact);
			config->hall_right_notify_deact =
				message->application.hall_right_notify_deact;
		}

		if (message->application.has_corr_temperature) {
			LOG_INF_PARAM_FLOAT("application.corr_temperature",
					    message->application.corr_temperature);
			config->corr_temperature = message->application.corr_temperature;
		}

		if (message->application.has_corr_t1_temperature) {
			LOG_INF_PARAM_FLOAT("application.corr_t1_temperature",
					    message->application.corr_t1_temperature);
			config->corr_t1_temperature = message->application.corr_t1_temperature;
		}

		if (message->application.has_corr_t2_temperature) {
			LOG_INF_PARAM_FLOAT("application.corr_t2_temperature",
					    message->application.corr_t2_temperature);
			config->corr_t2_temperature = message->application.corr_t2_temperature;
		}

		if (message->application.has_cap_hall_left) {
			LOG_INF_PARAM_BOOL("application.cap_hall_left",
					   message->application.cap_hall_left);
			config->cap_hall_left = message->application.cap_hall_left;
		}

		if (message->application.has_cap_hall_right) {
			LOG_INF_PARAM_BOOL("application.cap_hall_right",
					   message->application.cap_hall_right);
			config->cap_hall_right = message->application.cap_hall_right;
		}

		if (message->application.has_cap_light_sensor) {
			LOG_INF_PARAM_BOOL("application.cap_light_sensor",
					   message->application.cap_light_sensor);
			config->cap_light_sensor = message->application.cap_light_sensor;
		}

		if (message->application.has_cap_barometer) {
			LOG_INF_PARAM_BOOL("application.cap_barometer",
					   message->application.cap_barometer);
			config->cap_barometer = message->application.cap_barometer;
		}

		if (message->application.has_cap_pir_detector) {
			LOG_INF_PARAM_BOOL("application.cap_pir_detector",
					   message->application.cap_pir_detector);
			config->cap_pir_detector = message->application.cap_pir_detector;
		}

		if (message->application.has_cap_1w_thermometer) {
			LOG_INF_PARAM_BOOL("application.cap_1w_thermometer",
					   message->application.cap_1w_thermometer);
			config->cap_1w_thermometer = message->application.cap_1w_thermometer;
		}

		if (message->application.has_cap_1w_machine_probe) {
			LOG_INF_PARAM_BOOL("application.cap_1w_machine_probe",
					   message->application.cap_1w_machine_probe);
			config->cap_1w_machine_probe = message->application.cap_1w_machine_probe;
		}
	}
}
