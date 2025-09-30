/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_nfc_ingest.h"
#include "app_config.h"

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
		LOG_ERR("Call `hex2bin` failed");
		return false;
	}

	return true;
}

void app_nfc_ingest(const NfcConfigMessage *message)
{
	struct app_config *config = app_config();

	if (message->has_factory) {
		LOG_INF("Parameter `factory`: %s", message->factory ? "true" : "false");
		if (message->factory) {
			LOG_INF("Factory reset requested via NFC");
			app_config_reset();
		}
	}

	if (message->has_lorawan) {
		if (message->lorawan.has_region) {
			LOG_INF("Parameter `lorawan.region`: %d", message->lorawan.region);
			config->lrw_region = (enum app_config_lrw_region)message->lorawan.region;
		}

		if (message->lorawan.has_network) {
			LOG_INF("Parameter `lorawan.network`: %d", message->lorawan.network);
			config->lrw_network = (enum app_config_lrw_network)message->lorawan.network;
		}

		if (message->lorawan.has_adr) {
			LOG_INF("Parameter `lorawan.adr`: %s",
				message->lorawan.adr ? "true" : "false");
			config->lrw_adr = message->lorawan.adr;
		}

		if (message->lorawan.has_activation) {
			LOG_INF("Parameter `lorawan.activation`: %d", message->lorawan.activation);
			config->lrw_activation =
				(enum app_config_lrw_activation)message->lorawan.activation;
		}

		if (message->lorawan.has_deveui) {
			LOG_INF("Parameter `lorawan.deveui`: %s", message->lorawan.deveui);
			if (!parse_hex_string(message->lorawan.deveui, config->lrw_deveui,
					      sizeof(config->lrw_deveui))) {
				LOG_ERR("Failed to parse lorawan.deveui");
			}
		}

		if (message->lorawan.has_joineui) {
			LOG_INF("Parameter `lorawan.joineui`: %s", message->lorawan.joineui);
			if (!parse_hex_string(message->lorawan.joineui, config->lrw_joineui,
					      sizeof(config->lrw_joineui))) {
				LOG_ERR("Failed to parse lorawan.joineui");
			}
		}

		if (message->lorawan.has_nwkkey) {
			LOG_INF("Parameter `lorawan.nwkkey`: %s", message->lorawan.nwkkey);
			if (!parse_hex_string(message->lorawan.nwkkey, config->lrw_nwkkey,
					      sizeof(config->lrw_nwkkey))) {
				LOG_ERR("Failed to parse lorawan.nwkkey");
			}
		}

		if (message->lorawan.has_appkey) {
			LOG_INF("Parameter `lorawan.appkey`: %s", message->lorawan.appkey);
			if (!parse_hex_string(message->lorawan.appkey, config->lrw_appkey,
					      sizeof(config->lrw_appkey))) {
				LOG_ERR("Failed to parse lorawan.appkey");
			}
		}

		if (message->lorawan.has_devaddr) {
			LOG_INF("Parameter `lorawan.devaddr`: %s", message->lorawan.devaddr);
			if (!parse_hex_string(message->lorawan.devaddr, config->lrw_devaddr,
					      sizeof(config->lrw_devaddr))) {
				LOG_ERR("Failed to parse lorawan.devaddr");
			}
		}

		if (message->lorawan.has_nwkskey) {
			LOG_INF("Parameter `lorawan.nwkskey`: %s", message->lorawan.nwkskey);
			if (!parse_hex_string(message->lorawan.nwkskey, config->lrw_nwkskey,
					      sizeof(config->lrw_nwkskey))) {
				LOG_ERR("Failed to parse lorawan.nwkskey");
			}
		}

		if (message->lorawan.has_appskey) {
			LOG_INF("Parameter `lorawan.appskey`: %s", message->lorawan.appskey);
			if (!parse_hex_string(message->lorawan.appskey, config->lrw_appskey,
					      sizeof(config->lrw_appskey))) {
				LOG_ERR("Failed to parse lorawan.appskey");
			}
		}
	}

	if (message->has_application) {
		if (message->application.has_calibration) {
			LOG_INF("Parameter `application.calibration`: %s",
				message->application.calibration ? "true" : "false");
			config->calibration = message->application.calibration;
		}

		if (message->application.has_interval_sample) {
			LOG_INF("Parameter `application.interval_sample`: %u",
				message->application.interval_sample);
			config->interval_sample = message->application.interval_sample;
		}

		if (message->application.has_interval_report) {
			LOG_INF("Parameter `application.interval_report`: %u",
				message->application.interval_report);
			config->interval_report = message->application.interval_report;
		}

		if (message->application.has_alarm_temperature_enabled) {
			LOG_INF("Parameter `application.alarm_temperature_enabled`: %s",
				message->application.alarm_temperature_enabled ? "true" : "false");
			config->alarm_temperature_enabled = message->application.alarm_temperature_enabled;
		}

		if (message->application.has_alarm_temperature_lo) {
			LOG_INF("Parameter `application.alarm_temperature_lo`: %.2f",
				(double)message->application.alarm_temperature_lo);
			config->alarm_temperature_lo = message->application.alarm_temperature_lo;
		}

		if (message->application.has_alarm_temperature_hi) {
			LOG_INF("Parameter `application.alarm_temperature_hi`: %.2f",
				(double)message->application.alarm_temperature_hi);
			config->alarm_temperature_hi = message->application.alarm_temperature_hi;
		}

		if (message->application.has_alarm_temperature_hst) {
			LOG_INF("Parameter `application.alarm_temperature_hst`: %.2f",
				(double)message->application.alarm_temperature_hst);
			config->alarm_temperature_hst = message->application.alarm_temperature_hst;
		}

		if (message->application.has_alarm_humidity_enabled) {
			LOG_INF("Parameter `application.alarm_humidity_enabled`: %s",
				message->application.alarm_humidity_enabled ? "true" : "false");
			config->alarm_humidity_enabled = message->application.alarm_humidity_enabled;
		}

		if (message->application.has_alarm_humidity_lo) {
			LOG_INF("Parameter `application.alarm_humidity_lo`: %.2f",
				(double)message->application.alarm_humidity_lo);
			config->alarm_humidity_lo = message->application.alarm_humidity_lo;
		}

		if (message->application.has_alarm_humidity_hi) {
			LOG_INF("Parameter `application.alarm_humidity_hi`: %.2f",
				(double)message->application.alarm_humidity_hi);
			config->alarm_humidity_hi = message->application.alarm_humidity_hi;
		}

		if (message->application.has_alarm_humidity_hst) {
			LOG_INF("Parameter `application.alarm_humidity_hst`: %.2f",
				(double)message->application.alarm_humidity_hst);
			config->alarm_humidity_hst = message->application.alarm_humidity_hst;
		}

		if (message->application.has_alarm_pressure_enabled) {
			LOG_INF("Parameter `application.alarm_pressure_enabled`: %s",
				message->application.alarm_pressure_enabled ? "true" : "false");
			config->alarm_pressure_enabled = message->application.alarm_pressure_enabled;
		}

		if (message->application.has_alarm_pressure_lo) {
			LOG_INF("Parameter `application.alarm_pressure_lo`: %.2f",
				(double)message->application.alarm_pressure_lo);
			config->alarm_pressure_lo = message->application.alarm_pressure_lo;
		}

		if (message->application.has_alarm_pressure_hi) {
			LOG_INF("Parameter `application.alarm_pressure_hi`: %.2f",
				(double)message->application.alarm_pressure_hi);
			config->alarm_pressure_hi = message->application.alarm_pressure_hi;
		}

		if (message->application.has_alarm_pressure_hst) {
			LOG_INF("Parameter `application.alarm_pressure_hst`: %.2f",
				(double)message->application.alarm_pressure_hst);
			config->alarm_pressure_hst = message->application.alarm_pressure_hst;
		}

		if (message->application.has_alarm_t1_temperature_enabled) {
			LOG_INF("Parameter `application.alarm_t1_temperature_enabled`: %s",
				message->application.alarm_t1_temperature_enabled ? "true" : "false");
			config->alarm_t1_temperature_enabled = message->application.alarm_t1_temperature_enabled;
		}

		if (message->application.has_alarm_t1_temperature_lo) {
			LOG_INF("Parameter `application.alarm_t1_temperature_lo`: %.2f",
				(double)message->application.alarm_t1_temperature_lo);
			config->alarm_t1_temperature_lo =
				message->application.alarm_t1_temperature_lo;
		}

		if (message->application.has_alarm_t1_temperature_hi) {
			LOG_INF("Parameter `application.alarm_t1_temperature_hi`: %.2f",
				(double)message->application.alarm_t1_temperature_hi);
			config->alarm_t1_temperature_hi =
				message->application.alarm_t1_temperature_hi;
		}

		if (message->application.has_alarm_t1_temperature_hst) {
			LOG_INF("Parameter `application.alarm_t1_temperature_hst`: %.2f",
				(double)message->application.alarm_t1_temperature_hst);
			config->alarm_t1_temperature_hst =
				message->application.alarm_t1_temperature_hst;
		}

		if (message->application.has_alarm_t2_temperature_enabled) {
			LOG_INF("Parameter `application.alarm_t2_temperature_enabled`: %s",
				message->application.alarm_t2_temperature_enabled ? "true" : "false");
			config->alarm_t2_temperature_enabled = message->application.alarm_t2_temperature_enabled;
		}

		if (message->application.has_alarm_t2_temperature_lo) {
			LOG_INF("Parameter `application.alarm_t2_temperature_lo`: %.2f",
				(double)message->application.alarm_t2_temperature_lo);
			config->alarm_t2_temperature_lo =
				message->application.alarm_t2_temperature_lo;
		}

		if (message->application.has_alarm_t2_temperature_hi) {
			LOG_INF("Parameter `application.alarm_t2_temperature_hi`: %.2f",
				(double)message->application.alarm_t2_temperature_hi);
			config->alarm_t2_temperature_hi =
				message->application.alarm_t2_temperature_hi;
		}

		if (message->application.has_alarm_t2_temperature_hst) {
			LOG_INF("Parameter `application.alarm_t2_temperature_hst`: %.2f",
				(double)message->application.alarm_t2_temperature_hst);
			config->alarm_t2_temperature_hst =
				message->application.alarm_t2_temperature_hst;
		}

		if (message->application.has_hall_left_counter) {
			LOG_INF("Parameter `application.hall_left_counter`: %s",
				message->application.hall_left_counter ? "true" : "false");
			config->hall_left_counter = message->application.hall_left_counter;
		}

		if (message->application.has_hall_left_notify_act) {
			LOG_INF("Parameter `application.hall_left_notify_act`: %s",
				message->application.hall_left_notify_act ? "true" : "false");
			config->hall_left_notify_act = message->application.hall_left_notify_act;
		}

		if (message->application.has_hall_left_notify_deact) {
			LOG_INF("Parameter `application.hall_left_notify_deact`: %s",
				message->application.hall_left_notify_deact ? "true" : "false");
			config->hall_left_notify_deact =
				message->application.hall_left_notify_deact;
		}

		if (message->application.has_hall_right_counter) {
			LOG_INF("Parameter `application.hall_right_counter`: %s",
				message->application.hall_right_counter ? "true" : "false");
			config->hall_right_counter = message->application.hall_right_counter;
		}

		if (message->application.has_hall_right_notify_act) {
			LOG_INF("Parameter `application.hall_right_notify_act`: %s",
				message->application.hall_right_notify_act ? "true" : "false");
			config->hall_right_notify_act = message->application.hall_right_notify_act;
		}

		if (message->application.has_hall_right_notify_deact) {
			LOG_INF("Parameter `application.hall_right_notify_deact`: %s",
				message->application.hall_right_notify_deact ? "true" : "false");
			config->hall_right_notify_deact =
				message->application.hall_right_notify_deact;
		}

		if (message->application.has_corr_temperature) {
			LOG_INF("Parameter `application.corr_temperature`: %.2f",
				(double)message->application.corr_temperature);
			config->corr_temperature = message->application.corr_temperature;
		}

		if (message->application.has_corr_t1_temperature) {
			LOG_INF("Parameter `application.corr_t1_temperature`: %.2f",
				(double)message->application.corr_t1_temperature);
			config->corr_t1_temperature = message->application.corr_t1_temperature;
		}

		if (message->application.has_corr_t2_temperature) {
			LOG_INF("Parameter `application.corr_t2_temperature`: %.2f",
				(double)message->application.corr_t2_temperature);
			config->corr_t2_temperature = message->application.corr_t2_temperature;
		}

		if (message->application.has_cap_hall_left) {
			LOG_INF("Parameter `application.cap_hall_left`: %s",
				message->application.cap_hall_left ? "true" : "false");
			config->cap_hall_left = message->application.cap_hall_left;
		}

		if (message->application.has_cap_hall_right) {
			LOG_INF("Parameter `application.cap_hall_right`: %s",
				message->application.cap_hall_right ? "true" : "false");
			config->cap_hall_right = message->application.cap_hall_right;
		}

		if (message->application.has_cap_light_sensor) {
			LOG_INF("Parameter `application.cap_light_sensor`: %s",
				message->application.cap_light_sensor ? "true" : "false");
			config->cap_light_sensor = message->application.cap_light_sensor;
		}

		if (message->application.has_cap_barometer) {
			LOG_INF("Parameter `application.cap_barometer`: %s",
				message->application.cap_barometer ? "true" : "false");
			config->cap_barometer = message->application.cap_barometer;
		}

		if (message->application.has_cap_pir_detector) {
			LOG_INF("Parameter `application.cap_pir_detector`: %s",
				message->application.cap_pir_detector ? "true" : "false");
			config->cap_pir_detector = message->application.cap_pir_detector;
		}

		if (message->application.has_cap_1w_thermometer) {
			LOG_INF("Parameter `application.cap_1w_thermometer`: %s",
				message->application.cap_1w_thermometer ? "true" : "false");
			config->cap_1w_thermometer = message->application.cap_1w_thermometer;
		}

		if (message->application.has_cap_1w_machine_probe) {
			LOG_INF("Parameter `application.cap_1w_machine_probe`: %s",
				message->application.cap_1w_machine_probe ? "true" : "false");
			config->cap_1w_machine_probe = message->application.cap_1w_machine_probe;
		}
	}
}
