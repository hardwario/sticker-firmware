/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_nfc_ingest.h"
#include "app_config.h"

/* Zephyr includes */
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_nfc_ingest, LOG_LEVEL_DBG);

void app_nfc_ingest(const NfcConfigMessage *message)
{
	struct app_config *config = app_config();

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
			LOG_INF("Parameter `lorawan.adr`: %s", message->lorawan.adr ? "true" : "false");
			config->lrw_adr = message->lorawan.adr;
		}

		if (message->lorawan.has_activation) {
			LOG_INF("Parameter `lorawan.activation`: %d", message->lorawan.activation);
			config->lrw_activation = (enum app_config_lrw_activation)message->lorawan.activation;
		}

		if (message->lorawan.has_deveui) {
			LOG_INF("Parameter `lorawan.deveui`: %s", message->lorawan.deveui);
			/* TODO: Parse hex string and store in config->lrw_deveui */
		}

		if (message->lorawan.has_joineui) {
			LOG_INF("Parameter `lorawan.joineui`: %s", message->lorawan.joineui);
			/* TODO: Parse hex string and store in config->lrw_joineui */
		}

		if (message->lorawan.has_nwkkey) {
			LOG_INF("Parameter `lorawan.nwkkey`: %s", message->lorawan.nwkkey);
			/* TODO: Parse hex string and store in config->lrw_nwkkey */
		}

		if (message->lorawan.has_appkey) {
			LOG_INF("Parameter `lorawan.appkey`: %s", message->lorawan.appkey);
			/* TODO: Parse hex string and store in config->lrw_appkey */
		}

		if (message->lorawan.has_devaddr) {
			LOG_INF("Parameter `lorawan.devaddr`: %s", message->lorawan.devaddr);
			/* TODO: Parse hex string and store in config->lrw_devaddr */
		}

		if (message->lorawan.has_nwkskey) {
			LOG_INF("Parameter `lorawan.nwkskey`: %s", message->lorawan.nwkskey);
			/* TODO: Parse hex string and store in config->lrw_nwkskey */
		}

		if (message->lorawan.has_appskey) {
			LOG_INF("Parameter `lorawan.appskey`: %s", message->lorawan.appskey);
			/* TODO: Parse hex string and store in config->lrw_appskey */
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

		if (message->application.has_alarm_t1_temperature_lo) {
			LOG_INF("Parameter `application.alarm_t1_temperature_lo`: %.2f",
				(double)message->application.alarm_t1_temperature_lo);
			config->alarm_t1_temperature_lo = message->application.alarm_t1_temperature_lo;
		}

		if (message->application.has_alarm_t1_temperature_hi) {
			LOG_INF("Parameter `application.alarm_t1_temperature_hi`: %.2f",
				(double)message->application.alarm_t1_temperature_hi);
			config->alarm_t1_temperature_hi = message->application.alarm_t1_temperature_hi;
		}

		if (message->application.has_alarm_t1_temperature_hst) {
			LOG_INF("Parameter `application.alarm_t1_temperature_hst`: %.2f",
				(double)message->application.alarm_t1_temperature_hst);
			config->alarm_t1_temperature_hst = message->application.alarm_t1_temperature_hst;
		}

		if (message->application.has_alarm_t2_temperature_lo) {
			LOG_INF("Parameter `application.alarm_t2_temperature_lo`: %.2f",
				(double)message->application.alarm_t2_temperature_lo);
			config->alarm_t2_temperature_lo = message->application.alarm_t2_temperature_lo;
		}

		if (message->application.has_alarm_t2_temperature_hi) {
			LOG_INF("Parameter `application.alarm_t2_temperature_hi`: %.2f",
				(double)message->application.alarm_t2_temperature_hi);
			config->alarm_t2_temperature_hi = message->application.alarm_t2_temperature_hi;
		}

		if (message->application.has_alarm_t2_temperature_hst) {
			LOG_INF("Parameter `application.alarm_t2_temperature_hst`: %.2f",
				(double)message->application.alarm_t2_temperature_hst);
			config->alarm_t2_temperature_hst = message->application.alarm_t2_temperature_hst;
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

		if (message->application.has_hall_left_enabled) {
			LOG_INF("Parameter `application.hall_left_enabled`: %s",
				message->application.hall_left_enabled ? "true" : "false");
			config->hall_left_enabled = message->application.hall_left_enabled;
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
			config->hall_left_notify_deact = message->application.hall_left_notify_deact;
		}

		if (message->application.has_hall_right_enabled) {
			LOG_INF("Parameter `application.hall_right_enabled`: %s",
				message->application.hall_right_enabled ? "true" : "false");
			config->hall_right_enabled = message->application.hall_right_enabled;
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
			config->hall_right_notify_deact = message->application.hall_right_notify_deact;
		}

		if (message->application.has_barometer_enabled) {
			LOG_INF("Parameter `application.barometer_enabled`: %s",
				message->application.barometer_enabled ? "true" : "false");
			config->barometer_enabled = message->application.barometer_enabled;
		}
	}
}
