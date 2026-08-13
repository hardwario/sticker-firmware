/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_BUZZER_H_
#define APP_BUZZER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_buzzer_init(void);
int app_buzzer_set(bool on);
int app_buzzer_on(uint32_t duration_ms);
int app_buzzer_play_info(void);
int app_buzzer_play_warning(void);
int app_buzzer_play_alarm(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BUZZER_H_ */
