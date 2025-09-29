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
	if (message->has_lorawan) {
		if (message->lorawan.has_region) {
			LOG_INF("Parameter `lorawan.region`: %d", message->lorawan.region);
		}

		if (message->lorawan.has_deveui) {
			LOG_INF("Parameter `lorawan.deveui`: %s", message->lorawan.deveui);
		}

		if (message->lorawan.has_devaddr) {
			LOG_INF("Parameter `lorawan.devaddr`: %s", message->lorawan.devaddr);
		}

		if (message->lorawan.has_nwkskey) {
			LOG_INF("Parameter `lorawan.nwkskey`: %s", message->lorawan.nwkskey);
		}

		if (message->lorawan.has_appskey) {
			LOG_INF("Parameter `lorawan.appskey`: %s", message->lorawan.appskey);
		}
	}

	if (message->has_application) {
		if (message->application.has_calibration) {
			LOG_INF("Parameter `application.calibration`: %s",
				message->application.calibration ? "true" : "false");
			g_app_config.calibration = message->application.calibration;
		}

		if (message->application.has_interval_sample) {
			LOG_INF("Parameter `application.interval_sample`: %u",
				message->application.interval_sample);
			g_app_config.interval_sample = message->application.interval_sample;
		}

		if (message->application.has_interval_report) {
			LOG_INF("Parameter `application.interval_report`: %u",
				message->application.interval_report);
			g_app_config.interval_report = message->application.interval_report;
		}

		/* Hall switch configuration */
		if (message->application.has_hall_left_enabled) {
			LOG_INF("Parameter `application.hall_left_enabled`: %s",
				message->application.hall_left_enabled ? "true" : "false");
			g_app_config.hall_left_enabled = message->application.hall_left_enabled;
		}

		if (message->application.has_hall_left_counter) {
			LOG_INF("Parameter `application.hall_left_counter`: %s",
				message->application.hall_left_counter ? "true" : "false");
			g_app_config.hall_left_counter = message->application.hall_left_counter;
		}

		if (message->application.has_hall_left_notify_act) {
			LOG_INF("Parameter `application.hall_left_notify_act`: %s",
				message->application.hall_left_notify_act ? "true" : "false");
			g_app_config.hall_left_notify_act = message->application.hall_left_notify_act;
		}

		if (message->application.has_hall_left_notify_deact) {
			LOG_INF("Parameter `application.hall_left_notify_deact`: %s",
				message->application.hall_left_notify_deact ? "true" : "false");
			g_app_config.hall_left_notify_deact = message->application.hall_left_notify_deact;
		}

		if (message->application.has_hall_right_enabled) {
			LOG_INF("Parameter `application.hall_right_enabled`: %s",
				message->application.hall_right_enabled ? "true" : "false");
			g_app_config.hall_right_enabled = message->application.hall_right_enabled;
		}

		if (message->application.has_hall_right_counter) {
			LOG_INF("Parameter `application.hall_right_counter`: %s",
				message->application.hall_right_counter ? "true" : "false");
			g_app_config.hall_right_counter = message->application.hall_right_counter;
		}

		if (message->application.has_hall_right_notify_act) {
			LOG_INF("Parameter `application.hall_right_notify_act`: %s",
				message->application.hall_right_notify_act ? "true" : "false");
			g_app_config.hall_right_notify_act = message->application.hall_right_notify_act;
		}

		if (message->application.has_hall_right_notify_deact) {
			LOG_INF("Parameter `application.hall_right_notify_deact`: %s",
				message->application.hall_right_notify_deact ? "true" : "false");
			g_app_config.hall_right_notify_deact = message->application.hall_right_notify_deact;
		}
	}
}