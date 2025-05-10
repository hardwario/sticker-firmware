/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LED_H_
#define APP_LED_H_

#ifdef __cplusplus
extern "C" {
#endif

enum app_led_channel {
	APP_LED_CHANNEL_R = 0,
	APP_LED_CHANNEL_G = 1,
	APP_LED_CHANNEL_Y = 2,
};

void app_led_set(enum app_led_channel channel, int state);

#ifdef __cplusplus
}
#endif

#endif /* APP_LED_H_ */
