/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * No-op fake LoRa driver so app_p2p.c links and DEVICE_DT_GET(DT_ALIAS(lora0))
 * resolves on native_sim. tests/p2p_logic exercises app_p2p.c's pure helpers
 * (framing, time-on-air, the duty governor), never the radio -- so every entry
 * point just returns success.
 */

#define DT_DRV_COMPAT hardwario_lora_emul

#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>

static int emul_config(const struct device *dev, struct lora_modem_config *config)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(config);
	return 0;
}

static int emul_send(const struct device *dev, uint8_t *data, uint32_t data_len)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data);
	ARG_UNUSED(data_len);
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
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	ARG_UNUSED(timeout);
	if (rssi) {
		*rssi = 0;
	}
	if (snr) {
		*snr = 0;
	}
	return -ETIMEDOUT;
}

static int emul_recv_async(const struct device *dev, lora_recv_cb cb, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(user_data);
	return -ENOTSUP;
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
