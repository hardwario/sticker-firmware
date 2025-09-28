/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct app_config {
	uint8_t secret_key[16];
	uint32_t serial_number;
	uint32_t nonce_counter;
	bool calibration;
	int interval_sample;
	int interval_report;
	uint8_t lrw_deveui[8];
	uint8_t lrw_devaddr[4];
	uint8_t lrw_nwkskey[16];
	uint8_t lrw_appskey[16];
	float corr_temperature;
	float corr_ext_temperature_1;
	float corr_ext_temperature_2;
	bool has_mpl3115a2;
	bool hall_left_enabled;
	bool hall_left_counter;
	bool hall_left_notify_act;
	bool hall_left_notify_deact;
	bool hall_right_enabled;
	bool hall_right_counter;
	bool hall_right_notify_act;
	bool hall_right_notify_deact;
};

extern struct app_config g_app_config;

struct app_config *app_config(void);
int app_config_save(void);
int app_config_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H_ */
