/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_VERSION_H_
#define APP_VERSION_H_

#include <stdbool.h>
#include <zephyr/sys/util.h> /* IS_ENABLED */

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware identity has two orthogonal axes:
 *   1) version + source ("where the build came from") — injected by CI via
 *      -DAPP_VERSION_* / -DAPP_BUILD_TYPE (see app/CMakeLists.txt):
 *        tag v1.3.4 -> 1/3/4, build type MAIN
 *        branch/PR  -> 0/0/0, build type DEV
 *        local      -> 0/0/0, build type CUSTOM (CMake default)
 *   2) configuration debug vs release — from CONFIG_FW_DEBUG (debug.conf sets
 *      it y, release leaves it n), see app_version_is_debug().
 * The fallbacks below keep IDE/standalone tooling happy. */
#ifndef APP_VERSION_MAJOR
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 0
#define APP_BUILD_TYPE    2
#endif

/* Source of the build (orthogonal to the debug/release config). */
enum app_build_type {
	APP_BUILD_TYPE_MAIN   = 0, /* tagged build from main */
	APP_BUILD_TYPE_DEV    = 1, /* CI build from a branch / PR */
	APP_BUILD_TYPE_CUSTOM = 2, /* local / modified firmware */
};

static inline const char *app_build_type_str(enum app_build_type type)
{
	switch (type) {
	case APP_BUILD_TYPE_MAIN:
		return "main";
	case APP_BUILD_TYPE_DEV:
		return "dev";
	case APP_BUILD_TYPE_CUSTOM:
		return "custom";
	default:
		return "unknown";
	}
}

/* True for a debug build (debug.conf), false for release. */
static inline bool app_version_is_debug(void)
{
	return IS_ENABLED(CONFIG_FW_DEBUG);
}

#ifdef __cplusplus
}
#endif

#endif /* APP_VERSION_H_ */
