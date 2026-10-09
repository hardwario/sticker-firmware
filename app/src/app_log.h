/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LOG_H_
#define APP_LOG_H_

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_ERR_CALL_FAILED(func)          LOG_ERR("Call `" func "` failed")
#define LOG_ERR_CALL_FAILED_INT(func, val) LOG_ERR("Call `" func "` failed: %d", val)
#define LOG_ERR_CALL_FAILED_STR(func, val) LOG_ERR("Call `" func "` failed: %s", val)
#define LOG_ERR_CALL_FAILED_CTX_INT(func, ctx, val)                                                \
	LOG_ERR("Call `" func "` failed (" ctx "): %d", val)
#define LOG_ERR_CALL_FAILED_CTX_STR(func, ctx, val)                                                \
	LOG_ERR("Call `" func "` failed (" ctx "): %s", val)

/* Say why the device is about to reboot, and give the log backend time to drain.
 *
 * `Reset cause: 0x%08x` cannot tell a sys_reboot() from an NRST -- both read
 * PIN|SOFTWARE (0x03) -- so a reboot that logs nothing is indistinguishable
 * afterwards from a fault or a hand on the reset line. Two unexplained resets
 * on the 2026-09-10 bench could not be attributed for exactly that reason.
 *
 * WRN, not INF, so the line survives debug.conf's CONFIG_LOG_MAX_LEVEL=2. The
 * 100 ms sleep is the flush app_power_suspend() already takes before
 * sys_poweroff(): the bench image is CONFIG_LOG_MODE_DEFERRED, so without it
 * the message can still be in the ring when the core goes down. */
#define LOG_WRN_REBOOTING(reason)                                                                  \
	do {                                                                                       \
		LOG_WRN("Rebooting: " reason);                                                     \
		k_sleep(K_MSEC(100));                                                              \
	} while (0)

/* Print a float without CONFIG_CBPRINTF_FP_SUPPORT: scale to fixed decimals and
 * emit sign + integer + zero-padded fraction as plain %d args. Match the decimal
 * count in the format string, e.g.
 *     LOG_INF("T %s%d.%02d C", APP_FP2(t));   // was "%.2f"
 *     LOG_INF("lux %d", APP_FP0(x));          // was "%.0f"
 * Only the formatting changes; float arithmetic is unaffected. */
#define APP_FP_SCALE_(v, s) ((int)((float)(v) * (float)(s) + ((float)(v) < 0 ? -0.5f : 0.5f)))
#define APP_FP_SIGN_(v)     (((v) < 0) ? "-" : "")
#define APP_FP_ABS_(x)      ((x) < 0 ? -(x) : (x))
#define APP_FP0(v)          APP_FP_SCALE_(v, 1) /* matches %.0f -> %d */
#define APP_FP1(v)                                                                                 \
	APP_FP_SIGN_(v), APP_FP_ABS_(APP_FP_SCALE_(v, 10)) / 10,                                   \
		APP_FP_ABS_(APP_FP_SCALE_(v, 10)) % 10 /* %.1f -> %s%d.%01d */
#define APP_FP2(v)                                                                                 \
	APP_FP_SIGN_(v), APP_FP_ABS_(APP_FP_SCALE_(v, 100)) / 100,                                 \
		APP_FP_ABS_(APP_FP_SCALE_(v, 100)) % 100 /* %.2f -> %s%d.%02d */
#define APP_FP3(v)                                                                                 \
	APP_FP_SIGN_(v), APP_FP_ABS_(APP_FP_SCALE_(v, 1000)) / 1000,                               \
		APP_FP_ABS_(APP_FP_SCALE_(v, 1000)) % 1000 /* %.3f -> %s%d.%03d */

#ifdef CONFIG_APP_VERBOSE_LOGGING
#define LOG_INF_PARAM_BOOL(name, value)                                                            \
	LOG_INF("Parameter `" name "`: %s", (value) ? "true" : "false")
#define LOG_INF_PARAM_INT(name, value)   LOG_INF("Parameter `" name "`: %d", (value))
#define LOG_INF_PARAM_FLOAT(name, value) LOG_INF("Parameter `" name "`: %s%d.%02d", APP_FP2(value))
#define LOG_INF_PARAM_STR(name, value)   LOG_INF("Parameter `" name "`: %s", (value))
#else
#define LOG_INF_PARAM_BOOL(name, value)
#define LOG_INF_PARAM_INT(name, value)
#define LOG_INF_PARAM_FLOAT(name, value)
#define LOG_INF_PARAM_STR(name, value)
#endif /* CONFIG_APP_VERBOSE_LOGGING */

#ifdef __cplusplus
}
#endif

#endif /* APP_LOG_H_ */
