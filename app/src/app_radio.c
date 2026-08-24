/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_radio.h"

#if defined(CONFIG_APP_LORA_P2P)
#include "app_p2p.h"
#endif

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_radio, LOG_LEVEL_INF);

/* Kept out of a static entirely when CONFIG_APP_LORA_P2P=n: with P2P not even
 * compiled in, the radio is always running LoRaWAN by construction (radio_mode's
 * only other option, OFF, is handled inside app_lrw itself, #271), so tracking
 * a runtime "which one did we pick" has no observable use — and every byte
 * counts on the flash-tight debug build (doc/p2p.md §11). */
#if defined(CONFIG_APP_LORA_P2P)
static enum app_radio_kind m_kind = APP_RADIO_LORAWAN;

static inline bool is_p2p(void)
{
	return m_kind == APP_RADIO_P2P;
}
#endif

int app_radio_init(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_P2P) {
		m_kind = APP_RADIO_P2P;
		LOG_INF("Radio: P2P (raw LoRa)");
		return app_p2p_init();
	}
	m_kind = APP_RADIO_LORAWAN;
#else
	if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_P2P) {
		LOG_WRN("radio-mode=p2p but CONFIG_APP_LORA_P2P=n; falling back to LoRaWAN");
	}
#endif
	LOG_INF("Radio: LoRaWAN");
	return app_lrw_init();
}

void app_radio_start(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		app_p2p_start();
		return;
	}
#endif
	app_lrw_join();
}

enum app_radio_kind app_radio_get_kind(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	return m_kind;
#else
	return APP_RADIO_LORAWAN;
#endif
}

enum app_lrw_state app_radio_get_state(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_is_ready() ? APP_LRW_STATE_HEALTHY : APP_LRW_STATE_IDLE;
	}
#endif
	return app_lrw_get_state();
}

bool app_radio_is_ready(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_is_ready();
	}
#endif
	return app_lrw_is_ready();
}

uint8_t app_radio_get_max_payload(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_get_max_payload();
	}
#endif
	return app_lrw_get_max_payload();
}

void app_radio_send_telemetry(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		app_p2p_send_telemetry();
		return;
	}
#endif
	app_lrw_send_telemetry();
}

int app_radio_queue_response(uint8_t port, const uint8_t *buf, size_t len)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_queue_response(port, buf, len);
	}
#endif
	return app_lrw_queue_response(port, buf, len);
}

int app_radio_send_alarm(const uint8_t *buf, size_t len)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		return app_p2p_send_alarm(buf, len);
	}
#endif
	return app_lrw_send_alarm(buf, len);
}

void app_radio_register_ready_cb(void (*cb)(void))
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		app_p2p_register_ready_cb(cb);
		return;
	}
#endif
	app_lrw_register_ready_cb(cb);
}

void app_radio_suspend(void)
{
#if defined(CONFIG_APP_LORA_P2P)
	if (is_p2p()) {
		app_p2p_suspend();
		return;
	}
#endif
	app_lrw_suspend();
}
