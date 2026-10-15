/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_clock.h"
#include "app_log.h"
#include "app_radio.h"

#if defined(__has_include) && __has_include("app_history.h")
#include "app_history.h"
#define APP_CLOCK_HAVE_HISTORY 1
#endif

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/timeutil.h>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

/* Standard includes */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

LOG_MODULE_REGISTER(app_clock, LOG_LEVEL_INF);

static const struct device *const m_rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));

/* Uptime (ms) the last network time landed (app_clock_set_network_time()), 0 = never. */
static int64_t m_network_time_at_ms;

/* Periodic re-sync (#96): the LSE drifts ~±20 ppm (~1.7 s/day, ~10 min/year),
 * which shows up in history timestamps and GetInfo unix_time. Re-request the
 * network time roughly weekly through app_radio, whatever the radio: it rides
 * the next regular uplink (LoRaWAN DeviceTimeReq, P2P TIME_REQ), so it costs
 * no extra message. The timer is armed once after the first network time (no
 * point before the network has time). */
#define RESYNC_PERIOD_SEC (7U * 24U * 3600U)

static void resync_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	LOG_INF("Periodic time re-sync");
	app_radio_time_request(); /* any context: work on the radio work queue */
}
static K_TIMER_DEFINE(m_resync_timer, resync_timer_handler, NULL);

int app_clock_init(void)
{
	if (!device_is_ready(m_rtc)) {
		LOG_ERR("RTC device not ready");
		return -ENODEV;
	}

	return 0;
}

int app_clock_set_network_time(uint32_t unix_s)
{
	/* L-5: sanity-bound the network time the same way the NFC clock_sync path
	 * does. A bogus network time would otherwise be written to the RTC and
	 * skew every history/alarm timestamp. */
	if (unix_s < APP_CLOCK_UNIX_MIN || unix_s >= APP_CLOCK_UNIX_END) {
		LOG_WRN("Network time %u out of range - ignored (L-5)", unix_s);
		return -ERANGE;
	}

	int ret = app_clock_set_unix(unix_s);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_clock_set_unix", ret);
		return ret;
	}

	/* Arm the periodic re-sync once, after the first network time. */
	if (m_network_time_at_ms == 0) {
		k_timer_start(&m_resync_timer, K_SECONDS(RESYNC_PERIOD_SEC),
			      K_SECONDS(RESYNC_PERIOD_SEC));
	}
	m_network_time_at_ms = MAX(k_uptime_get(), 1);
	return 0;
}

int64_t app_clock_network_time_at_ms(void)
{
	return m_network_time_at_ms;
}

int app_clock_get_unix(uint32_t *unix_s)
{
	if (!unix_s) {
		return -EINVAL;
	}

	struct rtc_time rtc_tm;
	int ret = rtc_get_time(m_rtc, &rtc_tm);
	if (ret) {
		return ret; /* -ENODATA when the RTC has never been set */
	}

	*unix_s = (uint32_t)timeutil_timegm(rtc_time_to_tm(&rtc_tm));

	return 0;
}

int app_clock_set_unix(uint32_t unix_s)
{
	/* #386/#494: the backstop for every caller, including the debug `clock set`
	 * shell command. The STM32 RTC year is 2 BCD digits (2000-2099); a year
	 * >= 2100 gives an out-of-range BCD nibble that hung rtc_stm32_get_time()'s
	 * read-twice loop until the watchdog (#386), and 2099-12-31 23:59:59 was
	 * accepted but the calendar then froze (#494 HIL). The low bound keeps the
	 * history re-anchor base from underflowing (L-5). */
	if (unix_s < APP_CLOCK_UNIX_MIN || unix_s >= APP_CLOCK_UNIX_END) {
		LOG_ERR("unix_time %u outside the accepted window [%lu, %lu)", unix_s,
			APP_CLOCK_UNIX_MIN, APP_CLOCK_UNIX_END);
		return -EINVAL;
	}

	time_t t = (time_t)unix_s;
	struct tm tm_utc;

	gmtime_r(&t, &tm_utc);

	struct rtc_time rtc_tm = {
		.tm_sec = tm_utc.tm_sec,
		.tm_min = tm_utc.tm_min,
		.tm_hour = tm_utc.tm_hour,
		.tm_mday = tm_utc.tm_mday,
		.tm_mon = tm_utc.tm_mon,
		.tm_year = tm_utc.tm_year,
		.tm_wday = tm_utc.tm_wday,
		.tm_yday = tm_utc.tm_yday,
		.tm_isdst = -1,
		.tm_nsec = 0,
	};

	int ret = rtc_set_time(m_rtc, &rtc_tm);
#ifdef APP_CLOCK_HAVE_HISTORY
	if (ret == 0) {
		app_history_on_clock_sync(unix_s);
	}
#endif
	return ret;
}

#if defined(CONFIG_SHELL)
static int cmd_clock_get(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint32_t unix_s;
	int ret = app_clock_get_unix(&unix_s);
	if (ret == -ENODATA) {
		shell_warn(sh, "RTC not set yet (no time sync)");
		return 0;
	}
	if (ret) {
		shell_error(sh, "app_clock_get_unix failed: %d", ret);
		return ret;
	}

	time_t t = (time_t)unix_s;
	struct tm tm;
	gmtime_r(&t, &tm);
	shell_print(sh, "unix=%u  UTC %04d-%02d-%02d %02d:%02d:%02d", unix_s, tm.tm_year + 1900,
		    tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	return 0;
}

static int cmd_clock_set(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	uint32_t unix_s = (uint32_t)strtoul(argv[1], NULL, 10);
	int ret = app_clock_set_unix(unix_s);
	if (ret) {
		shell_error(sh, "app_clock_set_unix failed: %d", ret);
		return ret;
	}
	shell_print(sh, "RTC set to unix=%u", unix_s);
	return 0;
}

static int cmd_clock_sync(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	app_radio_time_request();
	shell_print(sh, "Network time requested; the RTC updates when it lands");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_clock, SHELL_CMD_ARG(get, NULL, "Read RTC time.", cmd_clock_get, 1, 0),
	SHELL_CMD_ARG(set, NULL, "Set RTC time. Usage: set <unix>", cmd_clock_set, 2, 0),
	SHELL_CMD_ARG(sync, NULL, "Request network time (LoRaWAN DeviceTimeReq, P2P TIME_REQ).",
		      cmd_clock_sync, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(clock, &sub_clock, "RTC wall-clock commands.", NULL);
#endif /* defined(CONFIG_SHELL) */
