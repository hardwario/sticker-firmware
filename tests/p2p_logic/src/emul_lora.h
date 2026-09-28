/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fake LoRa driver's test surface (src/emul_lora.c).
 */

#ifndef EMUL_LORA_H_
#define EMUL_LORA_H_

#include <stddef.h>
#include <stdint.h>

#define TEST_LORA_SENT_MAX 16

struct test_lora_frame {
	uint8_t buf[255];
	uint8_t len;
};

typedef void (*test_lora_responder_t)(const uint8_t *frame, uint32_t len);

extern int test_lora_send_ret;
extern uint8_t test_lora_last_frame[255];
extern uint32_t test_lora_last_len;
extern uint32_t test_lora_send_count;
extern test_lora_responder_t test_lora_responder;

/* Queue a frame for the next receiver the module arms. */
void test_lora_rx_push(const uint8_t *buf, size_t len, int16_t rssi, int8_t snr);
/* No responder, no fault, nothing queued. The counters keep running. */
void test_lora_reset(void);
/* Send number `n` (0-based, test_lora_send_count before it went out). */
const struct test_lora_frame *test_lora_sent_frame(uint32_t n);

#endif /* EMUL_LORA_H_ */
