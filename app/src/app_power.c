/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_power.h"
#include "app_led.h"
#include "app_log.h"
#include "app_nfc.h"
#include "app_sensor.h"
#if defined(CONFIG_LORAWAN)
#include "app_lrw.h"
#include "app_report.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

#if defined(CONFIG_SOC_FAMILY_STM32)
#include <stm32_ll_system.h> /* LL_DBGMCU_Disable* */
#include <stm32_ll_pwr.h>    /* LL_PWR_SetPowerMode, LL_PWR_MODE_STOP2 */
#include <stm32_ll_cortex.h> /* LL_LPM_Enable{DeepSleep,Sleep} */
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
	app_lrw_suspend();
#endif
	app_sensor_suspend();

	app_led_set(APP_LED_CHANNEL_R, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_G, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_Y, APP_LED_OFF);

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

void app_power_standby(void)
{
	/* User-initiated, NFC-triggered standby (#156): behaves "off" but is woken
	 * by an NFC tap. The ST25DV GPO is on PB12/EXTI (not a WKUP pin), which can
	 * wake STM32WL Stop2 but NOT Shutdown — so standby uses Stop2 (RAM retained,
	 * low µA) rather than the deeper Shutdown used by app_power_suspend(). On
	 * wake we cold-reboot into normal operation (RAM-only: a power-cycle / NRST
	 * also exits standby). */
	LOG_WRN("Entering NFC standby (Stop2) — tap a phone (NFC) to wake");

#if defined(CONFIG_LORAWAN)
	app_report_suspend();
	app_lrw_suspend();
#endif
	app_sensor_suspend();

	app_led_set(APP_LED_CHANNEL_R, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_G, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_Y, APP_LED_OFF);

	/* Arm the ST25DV GPO to pulse PB12/EXTI on an RF field change (tap). Leaves
	 * the tag powered (LPD low) so it keeps sensing the field while we sleep. */
	int ret = app_nfc_arm_field_wake();
	if (ret) {
		/* Could not arm the wake source — entering Stop2 now would be a one-way
		 * trip (only NRST/power-cycle could recover). Abort to normal operation. */
		LOG_ERR_CALL_FAILED_INT("app_nfc_arm_field_wake", ret);
		LOG_ERR("NFC standby aborted — wake source not armed");
		return;
	}

	/* Let the RTT log line flush before the core sleeps. */
	k_sleep(K_MSEC(100));

#if defined(CONFIG_SOC_FAMILY_STM32)
	/* Without this the debug domain keeps the core out of Stop2 (and would wake
	 * it immediately), same as the Shutdown path. */
	LL_DBGMCU_DisableDBGStopMode();

	/* Enter Stop2 via a direct WFI rather than the PM idle path, so the dwell is
	 * deterministic (it does not depend on quiescing every kernel timeout): mask
	 * interrupts (the pending GPO EXTI still wakes WFI, its handler just does not
	 * run), select Stop2, set SLEEPDEEP, and sleep. A phone tap (or NRST) wakes
	 * us; we then cold-reboot into a clean normal boot. */
	(void)irq_lock();
	LL_PWR_SetPowerMode(LL_PWR_MODE_STOP2);
	LL_LPM_EnableDeepSleep();
	__WFI();
	/* Woken by the GPO EXTI (NFC tap). Clear deep-sleep and restart clean. */
	LL_LPM_EnableSleep();
#endif

	sys_reboot(SYS_REBOOT_COLD);
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
