/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TEST_STUBS_H_
#define TEST_STUBS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEST_FRAMES_MAX 256

struct test_frame {
	uint8_t port;
	size_t len;
	uint8_t b[64];
};

extern struct test_frame test_frames[TEST_FRAMES_MAX];
extern volatile int test_nframes;
extern bool test_clock_has;
extern uint8_t test_budget;
extern uint32_t test_clock_unix;

#endif /* TEST_STUBS_H_ */
