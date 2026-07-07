/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_wdog.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

/* Standard includes */
#include <errno.h>

LOG_MODULE_REGISTER(app_wdog, LOG_LEVEL_DBG);

#define MAX_TIMEOUT_MSEC 10000

/* Software liveness layer on top of the hardware IWDG (#182). The IWDG is fed
 * from the main loop every few seconds and so cannot, on its own, catch a worker
 * thread that has wedged (e.g. m_work_q blocked forever in lorawan_send(), #181).
 * Each monitored worker registers a channel and pings it as it makes progress;
 * app_wdog_feed() withholds the IWDG feed once any channel goes stale, so a
 * permanent wedge resets the SoC (which then rejoins on a clean boot). Timeouts
 * are kept well above the worst-case *legitimate* blocking time (a single
 * lorawan_send plus RX windows, ~7 s on TTN) so normal operation never trips it. */
#define APP_WDOG_MAX_CHANNELS 4

static int m_wdog_channel;

/* Per-channel last-ping uptime in seconds (atomic 32-bit; fits for >100 years).
 * A timeout of 0 marks an unused slot. */
static atomic_t m_chan_last_sec[APP_WDOG_MAX_CHANNELS];
static uint32_t m_chan_timeout_sec[APP_WDOG_MAX_CHANNELS];

int app_wdog_init(void)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(iwdg));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	static struct wdt_timeout_cfg cfg = {
		.flags = WDT_FLAG_RESET_SOC,
		.window.max = MAX_TIMEOUT_MSEC,
	};

	ret = wdt_install_timeout(dev, &cfg);
	if (ret < 0) {
		LOG_ERR_CALL_FAILED_INT("wdt_install_timeout", ret);
		return ret;
	}

	m_wdog_channel = ret;

	/* On a debug build, freeze the IWDG while a debugger has the core halted so a
	 * breakpoint doesn't reset the SoC out from under the debugger (#267, dev QoL).
	 * The option only takes effect with a debugger attached, so a Release build is
	 * unaffected either way; gate it to the debug build to keep production's DBGMCU
	 * state untouched. STM32WL supports it (wdt_iwdg_stm32 else-branch). */
	ret = wdt_setup(dev, IS_ENABLED(CONFIG_FW_DEBUG) ? WDT_OPT_PAUSE_HALTED_BY_DBG : 0);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("wdt_setup", ret);
		return ret;
	}

	return 0;
}

int app_wdog_register(uint32_t timeout_ms)
{
	for (int i = 0; i < APP_WDOG_MAX_CHANNELS; i++) {
		if (m_chan_timeout_sec[i] == 0) {
			/* Round up to whole seconds, minimum 1 s. */
			m_chan_timeout_sec[i] = (timeout_ms + 999) / 1000;
			if (m_chan_timeout_sec[i] == 0) {
				m_chan_timeout_sec[i] = 1;
			}
			/* Seed fresh so the worker gets one full timeout of grace. */
			atomic_set(&m_chan_last_sec[i], (atomic_val_t)k_uptime_seconds());
			return i;
		}
	}

	LOG_ERR("No free liveness channel");
	return -ENOMEM;
}

void app_wdog_ping(int channel)
{
	if (channel < 0 || channel >= APP_WDOG_MAX_CHANNELS) {
		return;
	}

	atomic_set(&m_chan_last_sec[channel], (atomic_val_t)k_uptime_seconds());
}

/* True when every registered liveness channel has been pinged within its
 * timeout. Vacuously true when no channels are registered (boot path). */
static bool liveness_ok(void)
{
	uint32_t now = (uint32_t)k_uptime_seconds();

	for (int i = 0; i < APP_WDOG_MAX_CHANNELS; i++) {
		if (m_chan_timeout_sec[i] == 0) {
			continue;
		}

		uint32_t last = (uint32_t)atomic_get(&m_chan_last_sec[i]);

		if (now - last > m_chan_timeout_sec[i]) {
			LOG_ERR("Liveness channel %d stale (%u s > %u s); withholding feed", i,
				now - last, m_chan_timeout_sec[i]);
			return false;
		}
	}

	return true;
}

int app_wdog_feed(void)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(iwdg));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	/* Gate the hardware feed on worker liveness: a wedged work queue stops
	 * pinging, so we stop feeding and let the IWDG reset the SoC (#181/#182). */
	if (!liveness_ok()) {
		return -EBUSY;
	}

	ret = wdt_feed(dev, m_wdog_channel);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("wdt_feed", ret);
		return ret;
	}

	return 0;
}
