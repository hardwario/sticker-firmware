/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LRW_H_
#define APP_LRW_H_

#include <stdbool.h>
#include <stddef.h>

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

int app_lrw_init(void);
void app_lrw_join(void);
void app_lrw_send(void);
enum app_lrw_state app_lrw_get_state(void);
bool app_lrw_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LRW_H_ */
