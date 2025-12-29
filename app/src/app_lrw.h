/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LRW_H_
#define APP_LRW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum app_lrw_state {
	APP_LRW_STATE_IDLE,
	APP_LRW_STATE_JOINING,
	APP_LRW_STATE_HEALTHY,
	APP_LRW_STATE_LINK_CHECK_PENDING,
	APP_LRW_STATE_LINK_CHECK_RETRY,
};

struct app_lrw_info {
	enum app_lrw_state state;
	bool joined;
	int datarate;
	int min_datarate;
	uint8_t max_payload;
	uint8_t max_next_payload;
	uint32_t dev_addr;
	uint8_t nwk_s_key[16];
	uint8_t app_s_key[16];
	uint32_t session_uptime_s;
	int16_t last_rssi;
	int8_t last_snr;
	uint8_t link_check_margin;
	uint8_t link_check_gateways;
};

int app_lrw_init(void);
void app_lrw_join(void);
void app_lrw_send(void);
enum app_lrw_state app_lrw_get_state(void);
bool app_lrw_is_ready(void);
int app_lrw_get_info(struct app_lrw_info *info);

/* Test bypass - simulates network unavailability (runtime only, not persisted) */
void app_lrw_set_bypass(bool enable);
bool app_lrw_get_bypass(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LRW_H_ */
