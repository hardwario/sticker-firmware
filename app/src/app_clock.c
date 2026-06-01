/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_clock.h"
#include "app_log.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/timeutil.h>

#if defined(CONFIG_LORAWAN)
#include <zephyr/lorawan/lorawan.h>
#endif

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

/* Standard includes */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

LOG_MODULE_REGISTER(app_clock, LOG_LEVEL_INF);

/* GPS epoch (1980-01-06) to Unix epoch (1970-01-01) offset in seconds. */
#define GPS_UNIX_EPOCH_OFFSET 315964800UL
/* GPS-UTC leap second offset (18 s since 2017-01-01). Update if IERS adds a
 * leap second. GPS time does not count leap seconds; UTC does. */
#define GPS_UTC_LEAP_SECONDS  18UL

static const struct device *const m_rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));

/* Set once the RTC has been synced from the network this session. Guards
 * against re-requesting DeviceTimeReq, which would pile up MAC commands and
 * push the uplink over the payload limit (Length error). */
static bool m_time_synced;

int app_clock_init(void)
{
	if (!device_is_ready(m_rtc)) {
		LOG_ERR("RTC device not ready");
		return -ENODEV;
	}

	return 0;
}

void app_clock_request_sync(void)
{
#if defined(CONFIG_LORAWAN)
	if (m_time_synced) {
		/* Already have network time; don't queue another DeviceTimeReq. */
		return;
	}

	/* force_request=false: piggyback the MAC command on the next regular
	 * uplink instead of sending an extra empty message. */
	int ret = lorawan_request_device_time(false);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lorawan_request_device_time", ret);
	} else {
		LOG_INF("DeviceTimeReq queued");
	}
#endif /* defined(CONFIG_LORAWAN) */
}

void app_clock_handle_downlink(uint8_t flags)
{
#if defined(CONFIG_LORAWAN)
	if (!(flags & LORAWAN_TIME_UPDATED)) {
		return;
	}

	uint32_t gps_time = 0;
	int ret = lorawan_device_time_get(&gps_time);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lorawan_device_time_get", ret);
		return;
	}

	uint32_t unix_time = gps_time + GPS_UNIX_EPOCH_OFFSET - GPS_UTC_LEAP_SECONDS;

	ret = app_clock_set_unix(unix_time);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_clock_set_unix", ret);
		return;
	}

	m_time_synced = true;
	LOG_INF("RTC synced from network: unix=%u", unix_time);
#else
	ARG_UNUSED(flags);
#endif /* defined(CONFIG_LORAWAN) */
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

	return rtc_set_time(m_rtc, &rtc_tm);
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

	app_clock_request_sync();
	shell_print(sh, "DeviceTimeReq requested; RTC updates on the next downlink");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_clock,
	SHELL_CMD_ARG(get, NULL, "Read RTC time.", cmd_clock_get, 1, 0),
	SHELL_CMD_ARG(set, NULL, "Set RTC time. Usage: set <unix>", cmd_clock_set, 2, 0),
	SHELL_CMD_ARG(sync, NULL, "Request network time (LoRaWAN DeviceTimeReq).", cmd_clock_sync,
		      1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(clock, &sub_clock, "RTC wall-clock commands.", NULL);
#endif /* defined(CONFIG_SHELL) */
