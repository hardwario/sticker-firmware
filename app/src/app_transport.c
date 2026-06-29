/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_transport.h"

#if defined(CONFIG_APP_LORA_P2P)
#include "app_p2p.h"
#endif

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_transport, LOG_LEVEL_INF);

static enum app_transport_kind m_kind = APP_TRANSPORT_LORAWAN;

static inline bool is_p2p(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	return m_kind == APP_TRANSPORT_P2P;
#else
	return false;
#endif
}

int app_transport_init(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (g_app_config.transport == APP_CONFIG_TRANSPORT_P2P) {
		m_kind = APP_TRANSPORT_P2P;
		LOG_INF("Transport: P2P (raw LoRa)");
		return app_p2p_init();
	}
#else
	if (g_app_config.transport == APP_CONFIG_TRANSPORT_P2P) {
		LOG_WRN("transport=p2p but CONFIG_APP_LORA_P2P=n; falling back to LoRaWAN");
	}
#endif
	m_kind = APP_TRANSPORT_LORAWAN;
	LOG_INF("Transport: LoRaWAN");
	return app_lrw_init();
}

void app_transport_start(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		app_p2p_start();
		return;
	}
#endif
	app_lrw_join();
}

enum app_transport_kind app_transport_get_kind(void)
{
	return m_kind;
}

enum app_lrw_state app_transport_get_state(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_is_ready() ? APP_LRW_STATE_HEALTHY : APP_LRW_STATE_IDLE;
	}
#endif
	return app_lrw_get_state();
}

bool app_transport_is_ready(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_is_ready();
	}
#endif
	return app_lrw_is_ready();
}

uint8_t app_transport_get_max_payload(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_get_max_payload();
	}
#endif
	return app_lrw_get_max_payload();
}

void app_transport_send_telemetry(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		app_p2p_send_telemetry();
		return;
	}
#endif
	app_lrw_send_telemetry();
}

int app_transport_queue_response(uint8_t port, const uint8_t *buf, size_t len)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_queue_response(port, buf, len);
	}
#endif
	return app_lrw_queue_response(port, buf, len);
}

int app_transport_send_alarm(const uint8_t *buf, size_t len)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_send_alarm(buf, len);
	}
#endif
	return app_lrw_send_alarm(buf, len);
}

void app_transport_register_ready_cb(void (*cb)(void))
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		app_p2p_register_ready_cb(cb);
		return;
	}
#endif
	app_lrw_register_ready_cb(cb);
}

void app_transport_suspend(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		app_p2p_suspend();
		return;
	}
#endif
	app_lrw_suspend();
}
