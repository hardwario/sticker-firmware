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

#define APP_LED_PLAY_MAX_COMMANDS 32

enum app_led_channel {
	APP_LED_CHANNEL_R = 0,
	APP_LED_CHANNEL_G = 1,
	APP_LED_CHANNEL_Y = 2,
};

enum app_led_state {
	APP_LED_OFF = 0,
	APP_LED_ON = 1,
};

enum app_led_cmd_type {
	APP_LED_CMD_END = -1,
	APP_LED_CMD_SET = 0,
	APP_LED_CMD_DELAY = 1,
};

struct app_led_cmd {
	enum app_led_cmd_type type;
	union {
		struct {
			enum app_led_channel channel;
			enum app_led_state state;
		} set;
		int duration;
	};
};

struct app_led_blink_req {
	enum app_led_channel color;
	int duration;
	int space;
	int repetitions;
};

struct app_led_play_req {
	struct app_led_cmd commands[APP_LED_PLAY_MAX_COMMANDS];
	int repetitions;
};

int app_led_init(void);
void app_led_set(enum app_led_channel channel, int state);
int app_led_blink(const struct app_led_blink_req *req);
int app_led_play(const struct app_led_play_req *req);

#ifdef __cplusplus
}
#endif

#endif /* APP_LED_H_ */
