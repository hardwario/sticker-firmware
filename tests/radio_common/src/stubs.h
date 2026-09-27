/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STUBS_H_
#define STUBS_H_

#include <zephyr/sys/util.h>

#include <stddef.h>
#include <stdint.h>

#define STUB_COMPOSE_MAX_FRAMES 8

extern size_t g_compose_frames[STUB_COMPOSE_MAX_FRAMES];
extern size_t g_compose_n_frames;
extern int g_compose_calls;
extern int g_compose_budget_zero_calls;
extern uint8_t g_compose_last_budget;
extern int g_compose_reset_calls;
extern uint8_t g_compose_report_id;

extern uint32_t g_budget_error_seq;
extern size_t g_budget_error_cap;
extern int g_budget_error_calls;
extern int g_alarm_flush_calls;

void stubs_reset(void);

#endif /* STUBS_H_ */
