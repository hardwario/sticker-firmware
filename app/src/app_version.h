/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_VERSION_H_
#define APP_VERSION_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware version + build type baked into the image. The build injects these
 * (CI passes -DAPP_VERSION_* / -DAPP_BUILD_TYPE, see app/CMakeLists.txt):
 *   tag v1.3.4 -> 1/3/4, build type RELEASE
 *   branch/PR  -> 0/0/0, build type DEV
 * Local builds without the override default to 0/0/0, build type CUSTOM.
 * The fallbacks below keep IDE/standalone tooling happy. */
#ifndef APP_VERSION_MAJOR
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 0
#define APP_BUILD_TYPE    2
#endif

enum app_build_type {
	APP_BUILD_TYPE_RELEASE = 0, /* tagged release build */
	APP_BUILD_TYPE_DEV     = 1, /* CI build from a branch / PR */
	APP_BUILD_TYPE_CUSTOM  = 2, /* local / modified firmware */
};

static inline const char *app_build_type_str(enum app_build_type type)
{
	switch (type) {
	case APP_BUILD_TYPE_RELEASE:
		return "release";
	case APP_BUILD_TYPE_DEV:
		return "dev";
	case APP_BUILD_TYPE_CUSTOM:
		return "custom";
	default:
		return "unknown";
	}
}

#ifdef __cplusplus
}
#endif

#endif /* APP_VERSION_H_ */
