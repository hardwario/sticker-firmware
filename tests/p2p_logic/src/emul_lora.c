/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake LoRa driver so app_radio_p2p.c links and DEVICE_DT_GET(DT_ALIAS(lora0))
 * resolves on native_sim. It records every frame sent, and a test-installed
 * responder -- the suite's gateway -- can answer one: the frames it pushes are
 * handed to the next receiver armed, lora_recv() (the JoinAccept window) or
 * lora_recv_async() (the TOWER ACK and downlink windows), at once or
 * test_lora_rx_delay_ms after the receiver was armed.
 */

#define DT_DRV_COMPAT hardwario_lora_emul

#include "emul_lora.h"

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>

#include <string.h>

/* What emul_send() returns. 0 (a sent frame) unless a test is exercising a radio
 * fault; the test resets it. */
int test_lora_send_ret;
/* The last frame handed to the radio and the number of sends. */
uint8_t test_lora_last_frame[255];
uint32_t test_lora_last_len;
uint32_t test_lora_send_count;
/* Every sent frame, test_lora_sent[n % TEST_LORA_SENT_MAX] for send n. */
struct test_lora_frame test_lora_sent[TEST_LORA_SENT_MAX];
/* Called with every frame sent; may push the answer with test_lora_rx_push(). */
test_lora_responder_t test_lora_responder;

/* Queued frames reach an armed receiver this long after it was armed (0 = at
 * once): an answer late for a short window but inside a long one. */
uint32_t test_lora_rx_delay_ms;

#define RX_FIFO_DEPTH 4

static struct test_lora_frame m_rx_fifo[RX_FIFO_DEPTH];
static int16_t m_rx_rssi[RX_FIFO_DEPTH];
static int8_t m_rx_snr[RX_FIFO_DEPTH];
static size_t m_rx_count;

void test_lora_rx_push(const uint8_t *buf, size_t len, int16_t rssi, int8_t snr)
{
	if (m_rx_count >= RX_FIFO_DEPTH || len > sizeof(m_rx_fifo[0].buf)) {
		return;
	}
	memcpy(m_rx_fifo[m_rx_count].buf, buf, len);
	m_rx_fifo[m_rx_count].len = (uint8_t)len;
	m_rx_rssi[m_rx_count] = rssi;
	m_rx_snr[m_rx_count] = snr;
	m_rx_count++;
}

static const struct device *m_rx_dev;
static lora_recv_cb m_rx_cb;
static void *m_rx_user;

static void rx_deliver(void)
{
	for (size_t i = 0; i < m_rx_count; i++) {
		m_rx_cb(m_rx_dev, m_rx_fifo[i].buf, m_rx_fifo[i].len, m_rx_rssi[i], m_rx_snr[i],
			m_rx_user);
	}
	m_rx_count = 0;
}

static void rx_delay_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	if (m_rx_cb != NULL) {
		rx_deliver();
	}
}

K_TIMER_DEFINE(m_rx_delay_timer, rx_delay_expired, NULL);

void test_lora_reset(void)
{
	k_timer_stop(&m_rx_delay_timer);
	test_lora_send_ret = 0;
	test_lora_responder = NULL;
	test_lora_rx_delay_ms = 0;
	m_rx_cb = NULL;
	m_rx_count = 0;
}

const struct test_lora_frame *test_lora_sent_frame(uint32_t n)
{
	return &test_lora_sent[n % TEST_LORA_SENT_MAX];
}

static int emul_config(const struct device *dev, struct lora_modem_config *config)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(config);
	return 0;
}

static int emul_send(const struct device *dev, uint8_t *data, uint32_t data_len)
{
	ARG_UNUSED(dev);
	if (test_lora_send_ret != 0) {
		return test_lora_send_ret;
	}

	struct test_lora_frame *f = &test_lora_sent[test_lora_send_count % TEST_LORA_SENT_MAX];

	test_lora_last_len = MIN(data_len, sizeof(test_lora_last_frame));
	memcpy(test_lora_last_frame, data, test_lora_last_len);
	f->len = (uint8_t)test_lora_last_len;
	memcpy(f->buf, data, f->len);
	test_lora_send_count++;
	/* A frame heard before a receiver runs is lost, as on the air. */
	m_rx_count = 0;
	if (test_lora_responder) {
		test_lora_responder(data, data_len);
	}
	return 0;
}

static int emul_send_async(const struct device *dev, uint8_t *data, uint32_t data_len,
			   struct k_poll_signal *async)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data);
	ARG_UNUSED(data_len);
	ARG_UNUSED(async);
	return -ENOTSUP;
}

static int emul_recv(const struct device *dev, uint8_t *data, uint8_t size, k_timeout_t timeout,
		     int16_t *rssi, int8_t *snr)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(timeout);

	if (m_rx_count == 0) {
		return -ETIMEDOUT;
	}

	uint8_t len = MIN(m_rx_fifo[0].len, size);

	memcpy(data, m_rx_fifo[0].buf, len);
	if (rssi) {
		*rssi = m_rx_rssi[0];
	}
	if (snr) {
		*snr = m_rx_snr[0];
	}
	m_rx_count = 0; /* one frame per window */
	return len;
}

static int emul_recv_async(const struct device *dev, lora_recv_cb cb, void *user_data)
{
	k_timer_stop(&m_rx_delay_timer);
	m_rx_dev = dev;
	m_rx_cb = cb;
	m_rx_user = user_data;
	if (cb == NULL) {
		m_rx_count = 0; /* receiver off: what it did not take is gone */
		return 0;
	}
	if (test_lora_rx_delay_ms > 0) {
		k_timer_start(&m_rx_delay_timer, K_MSEC(test_lora_rx_delay_ms), K_NO_WAIT);
		return 0;
	}
	rx_deliver();
	return 0;
}

static int emul_test_cw(const struct device *dev, uint32_t frequency, int8_t tx_power,
			uint16_t duration)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(frequency);
	ARG_UNUSED(tx_power);
	ARG_UNUSED(duration);
	return -ENOTSUP;
}

static const struct lora_driver_api emul_lora_api = {
	.config = emul_config,
	.send = emul_send,
	.send_async = emul_send_async,
	.recv = emul_recv,
	.recv_async = emul_recv_async,
	.test_cw = emul_test_cw,
};

static int emul_lora_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

DEVICE_DT_INST_DEFINE(0, emul_lora_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &emul_lora_api);
