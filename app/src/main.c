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
#include "app_lrw.h"
#include "app_nfc.h"
#include "app_power.h"
#include "app_report.h"
#include "app_sensor.h"
#include "app_settings.h"
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
#define NFC_EVENT_FALLBACK_MS        (-1)
/* #164: once a response record is left on the tag, poll this often so the info
 * record is restored ~10 s after the phone leaves (no GPO events in this window
 * = field lost). The debounce avoids the #144 race with the phone still reading
 * the reply. */
#define NFC_INFO_RESTORE_DEBOUNCE_MS 10000
#define NFC_POLL_START_DELAY_MS      3000
/* Sized for the deepest NFC command run on this thread: a GetConfig/GetParam
 * over NFC packs DUMP_FIELDS tags into a flat ids[] (#176), builds a Response
 * (union sized to ConfigDump), and runs PSA AES-CCM decrypt/encrypt + nanopb —
 * far more than a short GetInfo. 3072 B overflowed on the longer commands. */
#define NFC_POLL_THREAD_STACK_SIZE   6144
#define NFC_POLL_THREAD_PRIO         K_LOWEST_APPLICATION_THREAD_PRIO

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

	app_led_play(&req);
	k_sleep(K_MSEC(5000));
}

static void play_carousel_nfc(void)
{
	/* #278: green (was yellow) — an applied NFC config is a SUCCESS, so it reads
	 * as green rather than joining the overloaded set of yellow signals. */
	struct app_led_blink_req req = {
		.color = APP_LED_CHANNEL_G, .duration = 100, .space = 100, .repetitions = 10};
	app_led_blink(&req);
	k_sleep(K_MSEC(10 * 200 - 100));
}

/* Run any deferred action queued by an NFC command (reboot/save/factory-reset/
 * ...). app_nfc_take_cmd_action() returns an action only once its response has
 * been delivered to the phone — the phone acked the reply and the info record was
 * restored, or the quiet backstop restored it — so a reboot/save fires *after*
 * the phone has read the response instead of racing a blind fixed delay. */
static void nfc_run_deferred_cmd_actions(void)
{
	enum app_cmd_action cmd_action = app_nfc_take_cmd_action();
	while (cmd_action != APP_CMD_ACTION_NONE) {
		switch (cmd_action) {
		case APP_CMD_ACTION_SETTINGS_SAVE:
			/* #250: green NFC carousel (#278) as operator feedback that an
			 * (offline-)staged config was applied — was previously only
			 * shown for the retired hio.stck:cfg record. */
			play_carousel_nfc();
			app_settings_save(true);
			break;
		case APP_CMD_ACTION_REBOOT:
			sys_reboot(SYS_REBOOT_COLD);
			break;
		case APP_CMD_ACTION_DEVICE_RESET:
			play_carousel_nfc();
			app_settings_device_reset();
			break;
		case APP_CMD_ACTION_FACTORY_RESET:
			/* #299, narrower than device_reset above: drops LoRaWAN too. */
			play_carousel_nfc();
			app_settings_factory_reset();
			break;
		case APP_CMD_ACTION_VENDOR_RESET:
			/* #299/#316, narrowest tier: set by the vendor_reset Command over the
			 * NFC hio.stck:vnd (vendor-token) channel — never reachable over
			 * LoRaWAN. The replacement secret_key travelled in the same request. */
			play_carousel_nfc();
			app_settings_vendor_reset(app_cmd_take_pending_vendor_secret_key());
			break;
		case APP_CMD_ACTION_SECRET_KEY_SAVE:
			/* #322: persist the staged new secret_key (#299 set_secret_key) and
			 * cold-reboot, so the rotated key is live right away via h_commit's
			 * normal g_app_config sync. A bare persist left the device
			 * authenticating with the OLD key until some later, unrelated
			 * reboot. The Ack the phone already read was encrypted with that old
			 * key — deliberately: this action only runs once the response has
			 * been delivered (#242), so the reply is never cut off. */
			play_carousel_nfc();
			app_settings_save(true);
			break;
		case APP_CMD_ACTION_CLM_REARM_SAVE:
			/* #351: flip the clm latch to UNSET and persist+reboot together,
			 * always (both the same-token and new-token clm_rearm cases run
			 * this action, see app_cmd_handle_clm_rearm) so the phone can
			 * always assume "ack read -> reboot" regardless of which case it
			 * took. When a new token was staged, this also ensures
			 * g_app_config.claim_token becomes live (h_commit) in the same
			 * breath the latch clears — no window where a poll could
			 * re-expose the OLD token; when no new token was given this is a
			 * same-value no-op re-persist.
			 *
			 * #340 M15: app_nfc_clm_reset() already persisted clm/state=UNSET
			 * to flash by the time app_settings_save() runs. If that save
			 * then fails, don't keep running live with the clm latch reset
			 * but the (possibly new) claim_token never persisted - mirrors
			 * app_settings.c's post-destructive-step convention (34a1ed8):
			 * force a reboot so the device re-reads whatever DID actually
			 * get persisted, instead of a silent, un-rebooted return leaving
			 * flash and live state out of sync until some later, unrelated
			 * reboot. */
			play_carousel_nfc();
			app_nfc_clm_reset();
			if (app_settings_save(true)) {
				sys_reboot(SYS_REBOOT_COLD);
			}
			break;
		case APP_CMD_ACTION_ENTER_CALIBRATION:
			/* Persist calibration=true + reboot; next boot enters
			 * calibration mode (app_calibration_init() clears it).
			 * Write the staging config (settings_save persists that,
			 * not the boot-time g_app_config copy). */
			play_carousel_nfc();
			app_config()->calibration = true;
			app_settings_save(true);
			break;
#if defined(CONFIG_LORAWAN)
		case APP_CMD_ACTION_LRW_RESET:
			/* Wipe LoRaWAN NVM (counters + DevNonce) + reboot (#109). */
			app_lrw_reset_nvm();
			sys_reboot(SYS_REBOOT_COLD);
			break;
		case APP_CMD_ACTION_LRW_JOIN:
			/* Force a (re)join now, no reboot (#109). */
			app_lrw_join();
			break;
#endif /* defined(CONFIG_LORAWAN) */
		case APP_CMD_ACTION_COUNTERS_SAVE:
			/* Persist the (reset) pulse totalizers, no reboot. */
			app_counters_save(true);
			break;
		default:
			break;
		}
		cmd_action = app_nfc_take_cmd_action();
	}
}

/* Dedicated NFC poll thread: independent of the LED blink loop. Each cycle does
 * the cheap gated poll (1-byte IT_STS_Dyn; full read only on RF activity) and
 * applies a consumed config. Started after a delay so app_nfc_init() has run. */
static void nfc_poll_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* NFC init failed at boot (#88): the tag is unusable, so exit instead of
	 * polling a dead ST25DV (which would error every wake and keep the CPU busy).
	 * The thread starts after NFC_POLL_START_DELAY_MS, so main() has already run
	 * app_nfc_init() by now. */
	if (!app_nfc_ready()) {
		LOG_WRN("NFC unavailable; poll thread not started");
		return;
	}

	/* Lay down the plaintext info record once at boot. The wait loop below is
	 * event-driven (waits forever on the GPO when idle), so without this initial
	 * check a freshly-booted tag would keep whatever was on it — or stay blank —
	 * until the first phone tap. */
	if (app_nfc_periodic_enabled()) {
		int ret = app_nfc_poll();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_nfc_poll", ret);
		}
	}

	for (;;) {
		/* Sleep until the GPO interrupt fires (phone touched the tag) or the
		 * fallback elapses. While a response is mid-write or a stale response
		 * record is on the tag (#164), use a short ~10 s fallback so we retry the
		 * deferred write / restore the info record soon; otherwise wait forever
		 * (event-driven — only the GPO wakes us). */
		bool nfc_busy = app_nfc_info_restore_pending() || app_nfc_resp_write_pending();
		int fallback = nfc_busy ? NFC_INFO_RESTORE_DEBOUNCE_MS : NFC_EVENT_FALLBACK_MS;
		int wret = app_nfc_wait_event(fallback);

		if (!app_nfc_periodic_enabled()) {
			continue;
		}

		/* A response write was deferred (RF field was up): fall through to
		 * app_nfc_poll(), which rewrites the cached reply in this field-off window
		 * — do NOT restore the info record (it would clobber the pending reply). */
		if (wret == -EAGAIN && !app_nfc_resp_write_pending() &&
		    app_nfc_info_restore_pending()) {
			/* #164: the fallback elapsed with a response record still on the tag
			 * and no GPO event in the debounce window → the RF field has been
			 * quiet (phone gone), so it is safe to restore the info record without
			 * racing the phone reading the reply (the #144 hazard). */
			int ret = app_nfc_restore_info();
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_nfc_restore_info", ret);
			}
			/* The backstop restore releases any deferred action (the phone left
			 * without acking) — run it now instead of waiting for the next wake. */
			nfc_run_deferred_cmd_actions();
			continue;
		}

		int ret = app_nfc_poll();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_nfc_poll", ret);
		}

		/* Deferred action from an NFC command (reboot/save/factory-reset/...) —
		 * the unified provisioning path (#250). Gated: only runs once the phone has
		 * read the response (see nfc_run_deferred_cmd_actions / app_nfc_take_cmd_action).
		 * play_carousel_nfc() is driven from inside the SETTINGS_SAVE/RESET cases if
		 * needed; here we just release the staged action. */
		nfc_run_deferred_cmd_actions();
	}
}

K_THREAD_DEFINE(nfc_poll_tid, NFC_POLL_THREAD_STACK_SIZE, nfc_poll_thread_fn, NULL, NULL, NULL,
		NFC_POLL_THREAD_PRIO, 0, NFC_POLL_START_DELAY_MS);

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

	/* The NFC tag (ST25DV) is non-essential: a broken tag must not brick an
	 * otherwise-healthy device (radio + sensors fine) into a die() reboot loop
	 * (#88). On init failure, log and continue with NFC disabled — the poll
	 * thread self-exits (app_nfc_ready() stays false) and the boot config check is
	 * skipped. die() stays reserved for wdog / LED / LRW init. */
	ret = app_nfc_init();
	if (ret) {
		LOG_WRN("app_nfc_init failed: %d (NFC unavailable, continuing)", ret);
	} else {
		/* A stale/replay command left on the tag makes app_nfc_check() fail
		 * (anti-replay) on every boot. Do NOT die() here — that would brick the
		 * device into a reboot loop. Log and continue, like the periodic check
		 * in the main loop does. */
		ret = app_nfc_check();
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_nfc_check", ret);
		}

		/* Open deferred-action gate: restore the info record after apply check.
		 * The phone that staged the command is gone (device was powered off). */
		if (app_nfc_info_restore_pending()) {
			ret = app_nfc_restore_info();
			if (ret) {
				LOG_ERR_CALL_FAILED_INT("app_nfc_restore_info", ret);
			}
		}
	}

#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

	play_carousel_boot();

#if defined(CONFIG_WATCHDOG)
	/* The carousel just blocked for 5 s of the 10 s IWDG window; feed again so
	 * the init chain below gets the full budget rather than the remainder. */
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

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

#if defined(CONFIG_LORAWAN)
	ret = app_lrw_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_lrw_init", ret);
		die();
	}

	/* Report orchestration (#126): owns the interval_report cadence and hands
	 * telemetry frames to app_lrw. Register before the join so the link-ready
	 * kick is wired when on_join_success fires. */
	ret = app_report_init();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_report_init", ret);
		die();
	}
#endif /* defined(CONFIG_LORAWAN) */

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

	/* Run deferred NFC command actions. Must occur after counters_init and
	 * sensor_init so selective resets (COUNTERS_SAVE) don't persist zero
	 * counters. Rebooting actions still fire before app_lrw_join(), keeping
	 * staged LoRaWAN keys in place (#147, #250). */
	nfc_run_deferred_cmd_actions();

#if defined(CONFIG_WATCHDOG)
	app_wdog_feed();
#endif /* defined(CONFIG_WATCHDOG) */

#if defined(CONFIG_LORAWAN)
	app_lrw_join();
#endif /* defined(CONFIG_LORAWAN) */

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
		 * effects), only its LED is gated like the rest. */
		bool led_handled = app_nfc_session_active();

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

#if defined(CONFIG_LORAWAN)
		enum app_lrw_state lrw_state = app_lrw_get_state();

		if (led_handled) {
			/* NFC interaction (or a higher-priority indicator) owns the LED. */
		} else if (lrw_state == APP_LRW_STATE_JOINING ||
			   lrw_state == APP_LRW_STATE_RECONNECT) {
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
		} else if (lrw_state == APP_LRW_STATE_WARNING) {
			/* Link-check streak failing but the session is still up (#278) — the
			 * MILD network state. Two yellow blinks, no red (one step above
			 * radio-off's single yellow, one below joining's yellow+red). */
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_Y,
							.duration = 10,
							.space = 200,
							.repetitions = 2};
			app_led_blink(&req);
			led_handled = true;
		} else if (lrw_state == APP_LRW_STATE_DISABLED) {
			/* Radio disabled by radio-mode (#271/#278): a single yellow blink — the
			 * lowest rung of the yellow severity scale, since this is a deliberate
			 * operator choice (radio-mode off/p2p), not a network fault. */
			struct app_led_blink_req req = {.color = APP_LED_CHANNEL_Y,
							.duration = 5,
							.space = 0,
							.repetitions = 1};
			app_led_blink(&req);
			led_handled = true;
		}
#endif /* defined(CONFIG_LORAWAN) */

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

#if defined(CONFIG_SHELL) && defined(CONFIG_LORAWAN)

static int cmd_join(const struct shell *shell, size_t argc, char **argv)
{
	app_lrw_join();

	shell_print(shell, "command succeeded");

	return 0;
}

static int cmd_send(const struct shell *shell, size_t argc, char **argv)
{
	app_report_trigger();

	shell_print(shell, "command succeeded");

	return 0;
}

SHELL_CMD_REGISTER(join, NULL, "Join LoRaWAN network.", cmd_join);
SHELL_CMD_REGISTER(send, NULL, "Send LoRaWAN data.", cmd_send);

#endif /* defined(CONFIG_SHELL) && defined(CONFIG_LORAWAN) */
