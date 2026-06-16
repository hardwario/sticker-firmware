/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_power.h"
#include "app_led.h"
#include "app_log.h"
#include "app_sensor.h"
#if defined(CONFIG_LORAWAN)
#include "app_lrw.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>
#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
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
	app_lrw_suspend();
#endif
	app_sensor_suspend();

	app_led_set(APP_LED_CHANNEL_R, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_G, APP_LED_OFF);
	app_led_set(APP_LED_CHANNEL_Y, APP_LED_OFF);

	/* Let the RTT log line flush before the core powers down. */
	k_sleep(K_MSEC(100));

	/* STM32WL Shutdown: lowest practical quiescent current. Identity and
	 * LoRaWAN keys live in NVS (flash) and survive; RAM and the RTC wall-clock
	 * are lost, so wake is a clean boot that re-syncs time from the network. */
	sys_poweroff();
}

#if defined(CONFIG_FW_DEBUG) && (CONFIG_APP_DEBUG_AUTOSUSPEND_S > 0)

void app_power_check_idle(void)
{
	static int64_t last_activity_ms;
	static unsigned int last_wr;
	static bool inited;

	/* The shell RTT down-buffer WrOff advances whenever the host sends shell
	 * input over the probe — use it as the "last interaction" signal without
	 * touching the shell internals. */
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
