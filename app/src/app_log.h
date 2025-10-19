/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LOG_H_
#define APP_LOG_H_

/* Zephyr includes */
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

#ifdef CONFIG_APP_VERBOSE_LOGGING
#define LOG_INF_PARAM_BOOL(name, value)                                                            \
	LOG_INF("Parameter `" name "`: %s", (value) ? "true" : "false")
#define LOG_INF_PARAM_INT(name, value)   LOG_INF("Parameter `" name "`: %d", (value))
#define LOG_INF_PARAM_FLOAT(name, value) LOG_INF("Parameter `" name "`: %.2f", (double)(value))
#define LOG_INF_PARAM_STR(name, value)   LOG_INF("Parameter `" name "`: %s", (value))
#else
#define LOG_INF_PARAM_BOOL(name, value)
#define LOG_INF_PARAM_INT(name, value)
#define LOG_INF_PARAM_FLOAT(name, value)
#define LOG_INF_PARAM_STR(name, value)
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_LOG_H_ */
