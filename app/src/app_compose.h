/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_COMPOSE_H_
#define APP_COMPOSE_H_

/* Standard includes */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int app_compose(uint8_t *buf, size_t size, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* APP_COMPOSE_H_ */
