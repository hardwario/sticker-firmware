/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_PYQ1648_H_
#define APP_PYQ1648_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_pyq1648_callback)(void *user_data);

void app_pyq1648_set_callback(app_pyq1648_callback callback, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* APP_PYQ1648_H_ */
