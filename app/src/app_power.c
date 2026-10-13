/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_power.h"
#include "app_battery.h"
#include "app_led.h"
#include "app_log.h"
#include "app_sensor.h"
#if defined(CONFIG_WATCHDOG)
#include "app_wdog.h"
#endif
#if defined(CONFIG_LORAWAN)
#include "app_report.h"
#include "app_radio.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>
#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#if defined(CONFIG_SOC_FAMILY_STM32)
#include <stm32_ll_system.h> /* LL_DBGMCU_Disable* */
#endif

#if defined(CONFIG_FW_DEBUG) && (CONFIG_APP_DEBUG_AUTOSUSPEND_S > 0)
#include <SEGGER_RTT.h>
#endif

LOG_MODULE_REGISTER(app_power, LOG_LEVEL_DBG);

void app_power_suspend(void)
{
	/* WRN level so the line survives the debug log filter (LOG_MAX_LEVEL=2). */
	LOG_WRN("Entering deep sleep (Shutdown) — wake via NRST / power-cycle");

#if defined(CONFIG_LORAWAN)
	app_report_suspend(); /* stop the report cadence before the radio teardown */
	app_radio_suspend();
#endif
	app_sensor_suspend();

	app_led_set(APP_LED_CHANNEL_R, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_G, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_Y, APP_LED_OFF);

	app_power_off();
}

void app_power_off(void)
{
	/* Let the RTT log line flush before the core powers down. */
	k_sleep(K_MSEC(100));

#if defined(CONFIG_SOC_FAMILY_STM32)
	/* Drop the DBGMCU debug-in-low-power bits before powering off. With
	 * CONFIG_STM32_ENABLE_DEBUG_SLEEP_STOP=y (and after any J-Link session)
	 * they stay set, keeping the debug domain powered — which wakes the MCU
	 * straight back out of Shutdown and boot-loops, even after the probe is
	 * unplugged, until a power-on-reset. Clearing them here lets Shutdown hold. */
	LL_DBGMCU_DisableDBGSleepMode();
	LL_DBGMCU_DisableDBGStopMode();
	LL_DBGMCU_DisableDBGStandbyMode();
#endif

	/* STM32WL Shutdown: lowest practical quiescent current. Identity and
	 * LoRaWAN keys live in NVS (flash) and survive; RAM and the RTC wall-clock
	 * are lost, so wake is a clean boot that re-syncs time from the network. */
	sys_poweroff();
}

#if defined(CONFIG_APP_BATTERY_BOOT_GATE)
/* Reads per boot check; the highest counts, so one low conversion does not keep
 * a good cell off. */
#define BOOT_CHECK_READS 3

bool app_power_boot_battery_low(void)
{
	float best = 0.f;
	int good = 0;

	if (app_battery_init()) {
		return false; /* no monitor: boot (a broken ADC must not brick the unit) */
	}
	for (int i = 0; i < BOOT_CHECK_READS; i++) {
		float v;

		if (i > 0) {
			k_sleep(K_MSEC(10));
		}
		if (app_battery_measure(&v) == 0) {
			best = MAX(best, v);
			good++;
		}
	}
	if (good == 0 || best * 1000.f >= CONFIG_APP_BATTERY_BOOT_MIN_MV) {
		return false;
	}

	/* WRN: survives the debug log filter (LOG_MAX_LEVEL=2). */
	LOG_WRN("Battery %s%d.%02d V below %d mV at boot: staying off", APP_FP2(best),
		CONFIG_APP_BATTERY_BOOT_MIN_MV);
	return true;
}
#else
bool app_power_boot_battery_low(void)
{
	return false;
}
#endif /* defined(CONFIG_APP_BATTERY_BOOT_GATE) */

#define LOW_BATT_BLINK_MS 100 /* on, then off: one blink every 200 ms */
#define LOW_BATT_BLINKS   25  /* 5 s */
#define LOW_BATT_FINAL_MS 1000

void app_power_low_battery_off(void)
{
	app_led_hold(true);
	for (int i = 0; i < LOW_BATT_BLINKS; i++) {
		app_led_set(APP_LED_CHANNEL_R, APP_LED_ON);
		k_sleep(K_MSEC(LOW_BATT_BLINK_MS));
		app_led_set(APP_LED_CHANNEL_R, APP_LED_OFF);
		k_sleep(K_MSEC(LOW_BATT_BLINK_MS));
#if defined(CONFIG_WATCHDOG)
		app_wdog_feed();
#endif
	}
	app_led_set(APP_LED_CHANNEL_R, APP_LED_ON);
	k_sleep(K_MSEC(LOW_BATT_FINAL_MS));
	app_led_set(APP_LED_CHANNEL_R, APP_LED_OFF);

	LOG_WRN("Low battery: Shutdown (wake via NRST / power-cycle)");
	app_power_off();
}

#if defined(CONFIG_FW_DEBUG) && (CONFIG_APP_DEBUG_AUTOSUSPEND_S > 0)

void app_power_check_idle(void)
{
	static int64_t last_activity_ms;
	static unsigned int last_wr;
	static bool inited;

	/* The shell RTT down-buffer WrOff advances whenever the host sends shell
	 * input over the probe — use it as the "last interaction" signal without
	 * touching the shell internals. (We deliberately do NOT gate on CoreDebug
	 * C_DEBUGEN: that bit is sticky and stays set after the probe is physically
	 * unplugged, so it can never tell "attached" from "was attached" — a unit
	 * with the J-Link yanked would then never suspend. Relying on shell idle
	 * means an active debug session keeps the device awake by sending commands,
	 * and an unplugged/idle unit suspends after the timeout.) */
	unsigned int wr = _SEGGER_RTT.aDown[CONFIG_SHELL_BACKEND_RTT_BUFFER].WrOff;
	int64_t now = k_uptime_get();

	if (!inited) {
		inited = true;
		last_wr = wr;
		last_activity_ms = now;
		return;
	}

	if (wr != last_wr) {
		last_wr = wr;
		last_activity_ms = now;
		return;
	}

	if (now - last_activity_ms >= (int64_t)CONFIG_APP_DEBUG_AUTOSUSPEND_S * MSEC_PER_SEC) {
		LOG_WRN("Idle %d s with no shell activity — auto-suspend",
			CONFIG_APP_DEBUG_AUTOSUSPEND_S);
		app_power_suspend();
	}
}

#else /* no auto-suspend */

void app_power_check_idle(void)
{
}

#endif

#if defined(CONFIG_SHELL)

/* `power suspend` enters deep sleep on demand — primarily a bench/test hook so
 * the auto-suspend path can be exercised without waiting out the idle timeout. */
static int cmd_power_suspend(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Suspending (deep sleep). Wake via NRST / power-cycle.");
	app_power_suspend();

	return 0; /* not reached */
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_power,
			       SHELL_CMD_ARG(suspend, NULL, "Enter deep sleep now (wake via NRST).",
					     cmd_power_suspend, 1, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(power, &sub_power, "Power management (deep sleep).", NULL);

#endif /* defined(CONFIG_SHELL) */
