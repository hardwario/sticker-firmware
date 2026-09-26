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

/* Smoothly ramp a PWM status LED (red or green only; yellow is GPIO and cannot
 * dim) from `from_pct` to `to_pct` duty over `duration_ms`. Blocking — runs in
 * the caller's thread. For visual effects / bench testing. Returns 0, or
 * -EINVAL for a non-PWM channel. */
int app_led_fade(enum app_led_channel channel, int from_pct, int to_pct, int duration_ms);

/* One heartbeat pulse (0 -> 100% -> 0 over ~200 ms) on a PWM LED. Blocking. */
int app_led_heartbeat(enum app_led_channel channel);

/* Transport/style of the periodic idle-state indicator emitted once per blink
 * interval while lrw-mode is off. A bench knob for comparing LED power. */
enum app_led_idle_mode {
	APP_LED_IDLE_OFF = 0,  /* dark — no periodic pulse (measure pure idle) */
	APP_LED_IDLE_GPIO = 1, /* short on/off blink (app_led_blink) */
	APP_LED_IDLE_PWM = 2,  /* heartbeat fade (app_led_heartbeat; red/green only) */
};

/* Configure the periodic lrw-mode-off indicator (runtime-only, not persisted —
 * a test knob). `on_ms` is the GPIO blink on-time (ignored for the PWM
 * heartbeat); <=0 keeps the current value. PWM mode with yellow (no timer)
 * falls back to green. */
void app_led_idle_config(enum app_led_idle_mode mode, enum app_led_channel channel, int on_ms);

/* Read back the current idle config; any out-pointer may be NULL. */
void app_led_idle_get(enum app_led_idle_mode *mode, enum app_led_channel *channel, int *on_ms);

/* Emit one idle-indicator pulse per the current config. Called ~once per blink
 * interval from the main loop while lrw-mode is off. */
void app_led_idle_pulse(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LED_H_ */
