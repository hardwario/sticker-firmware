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
	APP_LRW_STATE_WARNING,
	APP_LRW_STATE_RECONNECT,
};

struct app_lrw_info {
	enum app_lrw_state state;
	uint32_t dev_addr;             /* Device address (from OTAA or ABP) */
	uint32_t fcnt_up;              /* Uplink frame counter */
	int datarate;
	int16_t rssi;
	int8_t snr;
	uint8_t margin;
	uint8_t gw_count;
	/* State machine counters */
	uint8_t consecutive_lc_fail;   /* LC failures in a row (HEALTHY) */
	uint8_t consecutive_lc_ok;     /* LC successes in a row (WARNING) */
	uint8_t warning_lc_fail_total; /* Total LC failures in WARNING */
	uint8_t message_count;         /* Messages sent since boot/rejoin */
	/* Thresholds for display */
	uint8_t thresh_warning;        /* FAIL_THRESHOLD_WARNING */
	uint8_t thresh_healthy;        /* OK_THRESHOLD_HEALTHY */
	uint8_t thresh_reconnect;      /* FAIL_THRESHOLD_RECONNECT */
	uint8_t link_check_interval;   /* Every N-th message has LC */
};

int app_lrw_init(void);
void app_lrw_join(void);
void app_lrw_send(void);
void app_lrw_send_with_link_check(void);
enum app_lrw_state app_lrw_get_state(void);
int app_lrw_get_info(struct app_lrw_info *info);
bool app_lrw_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LRW_H_ */
