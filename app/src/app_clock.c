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

/* Standard includes */
#include <errno.h>
#include <stdint.h>
#include <time.h>

LOG_MODULE_REGISTER(app_clock, LOG_LEVEL_INF);

/* GPS epoch (1980-01-06) to Unix epoch (1970-01-01) offset in seconds. */
#define GPS_UNIX_EPOCH_OFFSET 315964800UL
/* GPS-UTC leap second offset (18 s since 2017-01-01). Update if IERS adds a
 * leap second. GPS time does not count leap seconds; UTC does. */
#define GPS_UTC_LEAP_SECONDS  18UL

static const struct device *const m_rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));

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
	int ret = lorawan_request_device_time(true);
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

	time_t unix_time = (time_t)gps_time + GPS_UNIX_EPOCH_OFFSET - GPS_UTC_LEAP_SECONDS;

	struct tm tm_utc;
	gmtime_r(&unix_time, &tm_utc);

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

	ret = rtc_set_time(m_rtc, &rtc_tm);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("rtc_set_time", ret);
		return;
	}

	LOG_INF("RTC synced from network: unix=%lld", (long long)unix_time);
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
