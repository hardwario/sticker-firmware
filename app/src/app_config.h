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

enum app_config_lrw_region {
	APP_CONFIG_LRW_REGION_EU868 = 0,
	APP_CONFIG_LRW_REGION_US915 = 1,
	APP_CONFIG_LRW_REGION_AU915 = 2,
};

enum app_config_lrw_network {
	APP_CONFIG_LRW_NETWORK_PUBLIC = 0,
	APP_CONFIG_LRW_NETWORK_PRIVATE = 1,
};

enum app_config_lrw_activation {
	APP_CONFIG_LRW_ACTIVATION_OTAA = 0,
	APP_CONFIG_LRW_ACTIVATION_ABP = 1,
};

struct app_config {
	uint8_t secret_key[16];
	uint32_t serial_number;
	uint32_t nonce_counter;
	bool calibration;
	int interval_sample;
	int interval_report;
	enum app_config_lrw_region lrw_region;
	enum app_config_lrw_network lrw_network;
	bool lrw_adr;
	enum app_config_lrw_activation lrw_activation;
	uint8_t lrw_deveui[8];
	uint8_t lrw_joineui[8];
	uint8_t lrw_nwkkey[16];
	uint8_t lrw_appkey[16];
	uint8_t lrw_devaddr[4];
	uint8_t lrw_nwkskey[16];
	uint8_t lrw_appskey[16];
	bool alarm_temperature_enabled;
	float alarm_temperature_lo;
	float alarm_temperature_hi;
	float alarm_temperature_hst;
	bool alarm_humidity_enabled;
	float alarm_humidity_lo;
	float alarm_humidity_hi;
	float alarm_humidity_hst;
	bool alarm_pressure_enabled;
	float alarm_pressure_lo;
	float alarm_pressure_hi;
	float alarm_pressure_hst;
	bool alarm_t1_temperature_enabled;
	float alarm_t1_temperature_lo;
	float alarm_t1_temperature_hi;
	float alarm_t1_temperature_hst;
	bool alarm_t2_temperature_enabled;
	float alarm_t2_temperature_lo;
	float alarm_t2_temperature_hi;
	float alarm_t2_temperature_hst;
	bool hall_left_counter;
	bool hall_left_notify_act;
	bool hall_left_notify_deact;
	bool hall_right_counter;
	bool hall_right_notify_act;
	bool hall_right_notify_deact;
	bool input_a_counter;
	bool input_a_notify_act;
	bool input_a_notify_deact;
	bool input_b_counter;
	bool input_b_notify_act;
	bool input_b_notify_deact;
	float corr_temperature;
	float corr_t1_temperature;
	float corr_t2_temperature;
	bool cap_hall_left;
	bool cap_hall_right;
	bool cap_input_a;
	bool cap_input_b;
	bool cap_light_sensor;
	bool cap_barometer;
	bool cap_pir_detector;
	bool cap_1w_thermometer;
	bool cap_1w_machine_probe;
};

extern struct app_config g_app_config;

struct app_config *app_config(void);
int app_config_init(void);
int app_config_save(void);
int app_config_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H_ */
