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
	uint32_t serial_number;
	uint8_t secret[32];
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
};

extern struct app_config g_app_config;

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H_ */
