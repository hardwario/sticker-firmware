/*
 * Copyright (c) 2024 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LRW_H_
#define APP_LRW_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_lrw_join(void);
int app_lrw_send(const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* APP_LRW_H_ */
