/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_alarm.h"
#include "app_battery.h"
#include "app_calibration.h"
#include "app_clock.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_counters.h"
#include "app_history.h"
#include "app_version.h"
#include "app_led.h"
#include "app_log.h"
#include "app_radio_lrw.h"
#include "app_nfc.h"
#include "app_power.h"
#include "app_report.h"
#include "app_sensor.h"
#include "app_radio.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define BLINK_INTERVAL_SECONDS 3

/* NFC poll runs on its own thread (not tied to the LED blink loop). It sleeps
 * on the ST25DV GPO interrupt (app_nfc_wait_event) and only wakes to read the
 * tag when the phone touches it — low power. When idle it waits FOREVER (-1):
 * event-driven, so a sticker with no phone near it never wakes to poll the tag
 * and stays in Stop2 indefinitely. The GPO EXTI (PB12) wakes it on RF field-on,
 * and the initial info-record write is done once at boot before the wait loop.
 * (Was a 30 s periodic fallback, which on the release build meant a Stop2 wake
 * every 30 s for nothing.) */
#define NFC_EVENT_FALLBACK_MS      (-1)
/* Sized for the deepest NFC command run on this thread: a GetConfig/GetParam
 * over NFC packs DUMP_FIELDS tags into a flat ids[] (#176), builds a Response
 * (union sized to ConfigDump), and runs PSA AES-CCM decrypt/encrypt + nanopb —
 * far more than a short GetInfo. 3072 B overflowed on the longer commands. */
#define NFC_POLL_THREAD_STACK_SIZE 6144
#define NFC_POLL_THREAD_PRIO       K_LOWEST_APPLICATION_THREAD_PRIO

#define APP_ALARM_ORANGE_RATE_LIMIT_MS 500
#define APP_ALARM_ORANGE_AUTO_OFF_MS   (60 * 60 * 1000)
#define APP_ALARM_ORANGE_BLINK_MS      50

enum app_mode {
	APP_MODE_NORMAL = 0,
	APP_MODE_CALIBRATION,
};

/* NFC-ready state lives in app_nfc.c (app_nfc_ready()); the tag is non-essential
 * (#88): on init failure we degrade instead of die()-ing, and the poll thread
 * self-exits when it stays false so a broken ST25DV can't keep the device
 * awake/looping. */

static void die(void)
{
	LOG_ERR("Rebooting in 60 seconds due to fatal error");

	for (int i = 0; i < 60; i++) {
#if defined(CONFIG_WATCHDOG)
		app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */
		k_sleep(K_SECONDS(1));
	}

	sys_reboot(SYS_REBOOT_COLD);
}

/* End of the boot carousel (k_uptime_get() ms); the periodic status blinks in
 * main() wait for it rather than queue up behind it. */
static int64_t m_carousel_end_ms;

/* Boot self-test of all three LEDs, 3 s. It plays in the LED thread; main()
 * goes on with the init chain meanwhile, and an NFC tap cuts it short
 * (app_led_hold), so neither the phone nor the boot waits for it. */
static void play_carousel_boot(void)
{
	struct app_led_play_req req = {
		.commands = {{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
			     {.type = APP_LED_CMD_DELAY, .duration = 500},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
			     {.type = APP_LED_CMD_DELAY, .duration = 250},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
			     {.type = APP_LED_CMD_DELAY, .duration = 500},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
			     {.type = APP_LED_CMD_DELAY, .duration = 250},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_ON}},
			     {.type = APP_LED_CMD_DELAY, .duration = 1500},
			     {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_OFF}},
			     {.type = APP_LED_CMD_END}},
		.repetitions = 1};

	int64_t len_ms = 0;

	for (int i = 0; req.commands[i].type != APP_LED_CMD_END; i++) {
		if (req.commands[i].type == APP_LED_CMD_DELAY) {
			len_ms += req.commands[i].duration;
		}
	}

	m_carousel_end_ms = k_uptime_get() + len_ms;
	app_led_play(&req);
}

/* #414: before an NFC-triggered reboot, let the mailbox session's result finish
 * on the LED (green + yellow = applied, red = the last exchange failed, 2 s) so
 * the operator sees it; the boot carousel follows the reboot. Replaces the
 * pre-reboot green NFC carousel (#278). */
static void nfc_result_before_reboot(void)
{
	app_nfc_led_result_wait();
}

/* Run any deferred action queued by an NFC command (reboot/save/factory-reset/
 * ...). app_nfc_take_cmd_action() returns an action only once its mailbox
 * session has delivered the reply (the phone read it, or had a second to) and
 * closed, so a reboot/save fires *after* the phone has read the response. */
static void nfc_run_deferred_cmd_actions(void)
{
	enum app_cmd_action cmd_action = app_nfc_take_cmd_action();
	while (cmd_action != APP_CMD_ACTION_NONE) {
		if (app_cmd_action_reboots(cmd_action)) {
			nfc_result_before_reboot();
		}
		app_cmd_run_action(cmd_action);
		cmd_action = app_nfc_take_cmd_action();
	}
}

/* Dedicated NFC poll thread: independent of the LED blink loop. Each cycle does
 * the cheap gated poll (1-byte IT_STS_Dyn; full read only on RF activity) and
 * applies a consumed config. main() starts it right after app_nfc_init(). */
static void nfc_poll_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* NFC init failed at boot (#88): the tag is unusable, so exit instead of
	 * polling a dead ST25DV (which would error every wake and keep the CPU busy).
	 * main() starts this thread only after app_nfc_init() has run. */
	if (!app_nfc_ready()) {
		LOG_WRN("NFC unavailable; poll thread not started");
		return;
	}

	for (;;) {
		/* Sleep until the GPO interrupt fires (phone's field appeared / a mailbox
		 * message landed). Event-driven: with no phone around the thread waits
		 * forever and the device stays in Stop2. */
		(void)app_nfc_wait_event(NFC_EVENT_FALLBACK_MS);

		/* Serve the mailbox while the phone holds its field (#313). The tag holds
		 * no NDEF record any more, so there is nothing else to reconcile. */
		int ret = app_nfc_poll();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_nfc_poll", ret);
		}

		/* Deferred action from a mailbox command (reboot/save/factory-reset/...):
		 * the session has already delivered the reply and closed, so it is safe to
		 * run now. Reboot-type actions first let the session result finish on the
		 * LED (nfc_result_before_reboot). */
		nfc_run_deferred_cmd_actions();
	}
}

/* Not started at boot (SYS_FOREVER_MS): main() starts it right after
 * app_nfc_init(), at the end of the init chain, so no phone command runs before
 * the components it reaches are up, and a phone kept on the tag across an
 * NFC-triggered reboot is served within ~0.2 s of the chip powering up (a fixed
 * start delay could do neither). */
K_THREAD_DEFINE(nfc_poll_tid, NFC_POLL_THREAD_STACK_SIZE, nfc_poll_thread_fn, NULL, NULL, NULL,
		NFC_POLL_THREAD_PRIO, 0, SYS_FOREVER_MS);

static enum app_mode detect_mode(void)
{
#if defined(CONFIG_APP_CALIBRATION)
	if (app_calibration_detect_magnets()) {
		LOG_WRN("Both magnets detected at boot — entering calibration mode");
		return APP_MODE_CALIBRATION;
	}

	if (g_app_config.calibration) {
		LOG_WRN("Calibration flag set in config — entering calibration mode");
		return APP_MODE_CALIBRATION;
	}
#endif /* defined(CONFIG_APP_CALIBRATION) */

	return APP_MODE_NORMAL;
}

static void event_led_handler(enum app_alarm_source source, bool active, void *user_data)
{
	ARG_UNUSED(source);
	ARG_UNUSED(user_data);

	static int64_t last_blink_ms;
	int64_t now = k_uptime_get();

	/* Commissioning-only diagnostic: the event LED confirms input activations for
	 * the first hour after power-up, then goes quiet. The cutoff is deliberately
	 * measured from boot (uptime), NOT from the event — once a unit is
	 * commissioned the blink is no longer wanted. */
	if (now > APP_ALARM_ORANGE_AUTO_OFF_MS) {
		return;
	}

	if (last_blink_ms != 0 && (now - last_blink_ms) < APP_ALARM_ORANGE_RATE_LIMIT_MS) {
		return;
	}

	/* #278: encode the input EDGE in the colour order so an installer can tell an
	 * activation from a release at a glance — and so this event blink can never be
	 * mistaken for the single-yellow radio-off heartbeat (it is a two-colour
	 * green/orange sequence, not plain yellow). Orange = R+G lit together.
	 *   activation (0->1, active=true):  green -> orange
	 *   release    (1->0, active=false): orange -> green
	 * Momentary sources (PIR, accelerometer) only ever send active=true, so they
	 * always show the green->orange activation sequence; hall/input send both
	 * edges. */
	const uint16_t d = APP_ALARM_ORANGE_BLINK_MS;
	struct app_led_play_req req;
	if (active) {
		/* green, then orange (R+G) */
		req = (struct app_led_play_req){
			.commands =
				{{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_ON}},
				 {.type = APP_LED_CMD_DELAY, .duration = d},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_OFF}},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_ON}},
				 {.type = APP_LED_CMD_DELAY, .duration = d},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_OFF}},
				 {.type = APP_LED_CMD_END}},
			.repetitions = 1};
	} else {
		/* orange (R+G), then green */
		req = (struct app_led_play_req){
			.commands =
				{{.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_ON}},
				 {.type = APP_LED_CMD_DELAY, .duration = d},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_OFF}},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_ON}},
				 {.type = APP_LED_CMD_DELAY, .duration = d},
				 {.type = APP_LED_CMD_SET, .set = {APP_LED_CHANNEL_G, APP_LED_OFF}},
				 {.type = APP_LED_CMD_END}},
			.repetitions = 1};
	}
	app_led_play(&req);

	last_blink_ms = now;
}

static int init(void)
{
	k_sleep(K_MSEC(500));

	return 0;
}

SYS_INIT(init, POST_KERNEL, 0);

int main(void)
{
	int ret;

	LOG_INF("Firmware version: %d.%d.%d (%s, %s)", APP_VERSION_MAJOR, APP_VERSION_MINOR,
		APP_VERSION_PATCH, app_build_type_str(APP_BUILD_TYPE),
		app_version_is_debug() ? "debug" : "release");
	LOG_INF("Build time: " __DATE__ " " __TIME__);

	/* Capture why we last reset (watchdog / brownout / pin / software) and clear
	 * the latch so the next boot reports a fresh cause. Reported in GetInfo so a
	 * watchdog reset is visible in the field (#88). */
#if defined(CONFIG_HWINFO)
	uint32_t reset_cause = 0;
	if (hwinfo_get_reset_cause(&reset_cause) == 0) {
		LOG_INF("Reset cause: 0x%08x", reset_cause);
		(void)hwinfo_clear_reset_cause();
	} else {
		LOG_WRN("hwinfo_get_reset_cause unavailable");
	}
	app_cmd_set_reset_cause(reset_cause);
#endif /* defined(CONFIG_HWINFO) */

	/* Shared HW init */
#if defined(CONFIG_WATCHDOG)
	ret = app_wdog_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_wdog_init", ret);
		die();
	}
#endif /* defined(CONFIG_WATCHDOG) */

	ret = app_led_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_led_init", ret);
		die();
	}

	/* Mode detection */
	enum app_mode mode = detect_mode();

	switch (mode) {
	case APP_MODE_CALIBRATION:
#if defined(CONFIG_APP_CALIBRATION)
		ret = app_calibration_init();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_calibration_init", ret);
			die();
		}
		app_calibration_run();
		/* Never reached */
#endif /* defined(CONFIG_APP_CALIBRATION) */
		break;

	case APP_MODE_NORMAL:
		break;
	}

	/* --- Normal mode --- */

#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

	play_carousel_boot();

	ret = app_clock_init();
	if (ret) {
		LOG_WRN("app_clock_init failed: %d (wall-clock unavailable)", ret);
	}

	ret = app_history_init();
	if (ret) {
		LOG_WRN("app_history_init failed: %d (history unavailable)", ret);
	}

	ret = app_alarm_rules_init();
	if (ret) {
		LOG_WRN("app_alarm_rules_init failed: %d (alarms unavailable)", ret);
	}

	/* Radio (#118): bring up the stack selected by `radio_mode` (LoRaWAN or
	 * raw-LoRa P2P). Both are linked; only the chosen one is started. */
	ret = app_radio_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_radio_init", ret);
		die();
	}

	/* Report orchestration (#126): owns the interval_report cadence and hands
	 * telemetry frames to the radio. Register before the start so the
	 * link-ready kick is wired when the radio comes up. */
	ret = app_report_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_report_init", ret);
		die();
	}

	/* A failed battery monitor must not brick an otherwise-healthy device into a
	 * die() reboot loop (#88): the radio and sensors work without it. Degrade
	 * gracefully — app_battery_measure() then returns an error and callers already
	 * treat that as "unavailable" (Info battery = 0, DevStatusAns level = 255). */
	ret = app_battery_init();
	if (ret) {
		LOG_WRN("app_battery_init failed: %d (battery monitoring unavailable)", ret);
	}

#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

	ret = app_sensor_init();
	if (ret) {
		LOG_WRN("Sensor init partially failed: %d (continuing)", ret);
	}

	/* Restore persisted pulse totalizers. Must run after app_sensor_init so the
	 * seed is not clobbered by app_hall_init / app_input_init. */
	ret = app_counters_init();
	if (ret) {
		LOG_WRN("app_counters_init failed: %d (counter persistence unavailable)", ret);
	}

	/* NFC last, once every component a command can reach is up (#414): the poll
	 * thread serves phone commands as soon as it starts, and one served before
	 * app_counters_init() / app_history_init() / app_sensor_init() / ... would act
	 * on uninitialised state (#340 M8: a reset_counters saved before the counters
	 * were restored wiped every totalizer). Until then the chip stays unpowered,
	 * so a phone on the tag just waits for VCC_ON. Before app_radio_start()
	 * (the LoRaWAN join): the claim state loaded here feeds the join Info.
	 *
	 * The NFC tag (ST25DV) is non-essential: a broken tag must not brick an
	 * otherwise-healthy device (radio + sensors fine) into a die() reboot loop
	 * (#88). On init failure, log and continue with NFC disabled — the poll
	 * thread self-exits (app_nfc_ready() stays false). The tag holds no NDEF
	 * record (#313, mailbox-only), so there is nothing to lay down at boot. */
	ret = app_nfc_init();
	if (ret) {
		LOG_WRN("app_nfc_init failed: %d (NFC unavailable, continuing)", ret);
	}
	/* Also on failure: the thread then sees !app_nfc_ready() and exits. */
	k_thread_start(nfc_poll_tid);

#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

	app_radio_start();

	app_alarm_set_event_callback(event_led_handler, NULL);

	/* Normal mode main loop */
	for (;;) {
		LOG_INF("Alive");

#if defined(CONFIG_POWEROFF)
		/* Debug auto-suspend: deep-sleep after a configurable idle timeout
		 * (no-op outside the Debug build / when the timeout is 0). */
		app_power_check_idle();
#endif

#if defined(CONFIG_WATCHDOG)
		/* -EBUSY = feed deliberately withheld because a worker is wedged
		 * (app_wdog logs which channel); let the IWDG reset us. */
		ret = app_wdog_feed();
		if (ret && ret != -EBUSY) {
			LOG_ERR_CALL_FAILED_INT("app_wdog_feed", ret);
		}
#endif /* defined(CONFIG_WATCHDOG) */

		/* NFC is polled on its own thread (nfc_poll_thread_fn), not here. */

#if defined(CONFIG_APP_CALIBRATION)
		/* Detect magnet on BOTH Hall sensors → reboot into calibration mode */
		if (k_uptime_get() < (int64_t)APP_CALIBRATION_ACTIVATION_WINDOW_MIN * 60 * 1000) {
			app_calibration_check_trigger();
		}
#endif /* defined(CONFIG_APP_CALIBRATION) */

		/* While a phone is interacting over NFC, the NFC interaction LED (app_nfc.c)
		 * owns the indicator — suppress the periodic status/heartbeat blinks below so
		 * they do not fight it. Alarm polling still runs (its latch/queue side
		 * effects), only its LED is gated like the rest. The same holds while the
		 * boot carousel is still playing. */
		bool led_handled = app_nfc_session_active() || k_uptime_get() < m_carousel_end_ms;

		/* Config NVS failed to load at boot (H-4): the device is running on
		 * compile-time defaults with its identity + provisioning gone. Signal it
		 * with a distinct red+yellow alternating pattern (highest priority) so a
		 * technician sees a corrupt-config fault rather than a silently blank or
		 * merely "not provisioned" device. */
		if (!led_handled && app_config_load_failed()) {
			struct app_led_play_req req = {
				.commands = {{.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 60},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 60},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
					     {.type = APP_LED_CMD_END}},
				.repetitions = 2};
			app_led_play(&req);
			led_handled = true;
		}

		/* Status LED reflects the active radio, LoRaWAN or P2P, through the
		 * common app_radio state (a P2P join, self-heal or link-check-like
		 * WARNING animates exactly like its LoRaWAN counterpart). */
		enum app_radio_state radio_state = app_radio_get_state();

		if (led_handled) {
			/* NFC interaction (or a higher-priority indicator) owns the LED. */
		} else if (radio_state == APP_RADIO_STATE_JOINING ||
			   radio_state == APP_RADIO_STATE_RECONNECT) {
			/* Not on the network — initial join or a rejoin after the link was
			 * lost (#278). This is the SEVERE LoRaWAN state (worse than WARNING,
			 * which keeps its session), so it carries a red accent: one yellow
			 * blink followed by one red. The severity scale across the three
			 * yellow states is radio-off (1× yellow) < warning (2× yellow) <
			 * joining/reconnect (yellow + red). */
			struct app_led_play_req req = {
				.commands = {{.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 10},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
					     {.type = APP_LED_CMD_DELAY, .duration = 200},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_R, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 80},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_R, APP_LED_OFF}},
					     {.type = APP_LED_CMD_END}},
				.repetitions = 1};
			app_led_play(&req);
			led_handled = true;
		} else if (radio_state == APP_RADIO_STATE_WARNING) {
			/* Link-check streak failing but the session is still up (#278) — the
			 * MILD network state. Two yellow blinks, no red (one step above
			 * radio-off's single yellow, one below joining's yellow+red). */
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_Y,
							.duration = 10,
							.space = 200,
							.repetitions = 2};
			app_led_blink(&req);
			led_handled = true;
		} else if (radio_state == APP_RADIO_STATE_DISABLED) {
			/* Radio disabled (#271/#278): a single yellow blink — the lowest rung
			 * of the yellow severity scale. LoRaWAN: DevEUI all-zero; P2P:
			 * radio_appkey or radio_deveui all-zero (device not provisioned). */
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_Y,
							.duration = 5,
							.space = 0,
							.repetitions = 1};
			app_led_blink(&req);
			led_handled = true;
		}

		/* Always evaluate alarms — do NOT short-circuit on led_handled. The poll
		 * is the only place thresholds/state/count rules, the no-data watchdog and
		 * the low-battery watchdog run and latch/queue their fPort-3 events. Gating
		 * it behind the LRW LED (JOINING/RECONNECT/WARNING) left the device blind to
		 * real alarms exactly while the network was down, and edge-triggered
		 * threshold crossings during that window were lost forever. The red alarm
		 * LED is still suppressed while the LRW LED owns the indicator. */
		bool alarm_active = app_alarm_poll();

		if (!led_handled && alarm_active) {
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_R,
							.duration = 5,
							.space = 0,
							.repetitions = 1};
			app_led_blink(&req);
			led_handled = true;
		}

		if (!led_handled) {
#if defined(CONFIG_FW_DEBUG)
			/* Debug: green + yellow LED blink */
			struct app_led_play_req req = {
				.commands = {{.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_G, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 5},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_G, APP_LED_OFF}},
					     {.type = APP_LED_CMD_DELAY, .duration = 50},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_ON}},
					     {.type = APP_LED_CMD_DELAY, .duration = 5},
					     {.type = APP_LED_CMD_SET,
					      .set = {APP_LED_CHANNEL_Y, APP_LED_OFF}},
					     {.type = APP_LED_CMD_END}},
				.repetitions = 1};
			app_led_play(&req);
#else
			/* Release: green LED blink */
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_G,
							.duration = 5,
							.space = 0,
							.repetitions = 1};
			app_led_blink(&req);
#endif /* defined(CONFIG_FW_DEBUG) */
		}

		k_sleep(K_SECONDS(BLINK_INTERVAL_SECONDS));
	}

	return 0;
}

#if defined(CONFIG_SHELL) && (defined(CONFIG_LORAWAN) || defined(CONFIG_RADIO_P2P))

/* app_radio_rejoin() is transport-agnostic (routes through app_radio, #118)
 * and, unlike app_radio_start() (boot-time bring-up), always forces a fresh
 * join attempt even if the radio already has a live session/pairing --
 * LoRaWAN already worked that way unconditionally; P2P needed the explicit
 * rejoin entry point since app_radio_p2p_start() intentionally treats an existing
 * pairing as sufficient (a session persists across a normal power cycle,
 * doc/p2p.md §7). Mirrors the `send` fix below -- this used to call
 * app_radio_lrw_join() directly and was compiled out on P2P-only builds. */
static int cmd_join(const struct shell *shell, size_t argc, char **argv)
{
	app_radio_rejoin();

	shell_print(shell, "command succeeded");

	return 0;
}

SHELL_CMD_REGISTER(join, NULL, "Force a fresh (re)join, even if already joined/paired.", cmd_join);

/* app_report_trigger() is transport-agnostic too (routes through app_radio,
 * #118 phase 2), so it stays available on a P2P-only (CONFIG_LORAWAN=n) build
 * just like `join` above. Regression found via #118 phase 2 HIL after
 * CONFIG_LORAWAN became toggleable: this command was silently compiled out on
 * the P2P bench overlay. */
static int cmd_send(const struct shell *shell, size_t argc, char **argv)
{
	app_report_trigger();

	shell_print(shell, "command succeeded");

	return 0;
}

SHELL_CMD_REGISTER(send, NULL, "Trigger an ad-hoc report send.", cmd_send);

#endif /* defined(CONFIG_SHELL) && (defined(CONFIG_LORAWAN) || defined(CONFIG_RADIO_P2P)) */
