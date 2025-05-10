/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <errno.h>

LOG_MODULE_REGISTER(app_wdog, LOG_LEVEL_DBG);

#define MAX_TIMEOUT_MSEC 10000

static int m_wdog_channel;

int app_wdog_feed(void)
{
	int ret;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(iwdg));
	if (!device_is_ready(dev)) {
		LOG_ERR("Device not ready");
		return -ENODEV;
	}

	ret = wdt_feed(dev, m_wdog_channel);
	if (ret) {
		LOG_ERR("Call `wdt_feed` failed: %d", ret);
		return ret;
	}

	return 0;
}

static int init(void)
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
		LOG_ERR("Call `wdt_install_timeout` failed: %d", ret);
		return ret;
	}

	m_wdog_channel = ret;

	ret = wdt_setup(dev, 0);
	if (ret) {
		LOG_ERR("Call `wdt_setup` failed: %d", ret);
		return ret;
	}

	return 0;
}

SYS_INIT(init, APPLICATION, 0);
