/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "app_log.h"
#include "app_radio_lrw.h"
#include "app_radio.h"

#if defined(CONFIG_RADIO_P2P)
#include "app_radio_p2p.h"
#endif

#include <zephyr/logging/log.h>

#include <errno.h>

LOG_MODULE_REGISTER(app_radio, LOG_LEVEL_INF);

/* Kept out of a static entirely when CONFIG_RADIO_P2P=n: with P2P not even
 * compiled in, the radio is always running LoRaWAN by construction (radio_mode's
 * only other option, OFF, is handled inside app_lrw itself, #271), so tracking
 * a runtime "which one did we pick" has no observable use — and every byte
 * counts on the flash-tight debug build (doc/p2p.md §11). */
#if defined(CONFIG_RADIO_P2P)
static enum app_radio_kind m_kind = APP_RADIO_LORAWAN;

static inline bool is_p2p(void)
{
	return m_kind == APP_RADIO_P2P;
}
#endif

int app_radio_init(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_P2P) {
		m_kind = APP_RADIO_P2P;
		LOG_INF("Radio: P2P (raw LoRa)");
		return app_p2p_init();
	}
	m_kind = APP_RADIO_LORAWAN;
#else
	if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_P2P) {
		LOG_WRN("radio-mode=p2p but CONFIG_RADIO_P2P=n; falling back to LoRaWAN");
	}
#endif
#if defined(CONFIG_LORAWAN)
	LOG_INF("Radio: LoRaWAN");
	return app_lrw_init();
#else
	/* Neither transport compiled in for this radio_mode. Only reachable on a
	 * P2P-only build (CONFIG_LORAWAN=n, #118 phase 2 flash budget) whose
	 * radio_mode is not p2p -- which includes `off`, the factory default
	 * (app_config.yml, #350), so a factory-reset bench node lands here.
	 *
	 * Degrade rather than fail. main.c treats a non-zero app_radio_init() as
	 * fatal and die()s into a 60 s reboot loop, leaving about nine seconds of
	 * shell per cycle to fix the setting in -- which is how F-36 was found.
	 * Every other app_radio_* entry point already has a safe #else tail
	 * (is_ready -> false, send -> -ENODEV) and app_report.c gates on
	 * app_radio_is_ready(), so an idle radio is a state the rest of the
	 * application already understands. Production compiles both transports
	 * (app/Kconfig: default y), so this branch never exists there. */
	LOG_ERR("Radio: no transport for radio-mode %d in this image; radio idle",
		g_app_config.radio_mode);
	return 0;
#endif
}

void app_radio_start(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_p2p_start();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_lrw_join();
#endif
}

#if defined(CONFIG_SHELL)
void app_radio_rejoin(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_p2p_rejoin();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_lrw_join(); /* already unconditional -- see app_radio_start() above */
#endif
}
#endif /* defined(CONFIG_SHELL) */

enum app_radio_kind app_radio_get_kind(void)
{
#if defined(CONFIG_RADIO_P2P)
	return m_kind;
#else
	return APP_RADIO_LORAWAN;
#endif
}

enum app_lrw_state app_radio_get_state(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_p2p_is_ready() ? APP_LRW_STATE_HEALTHY : APP_LRW_STATE_IDLE;
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_lrw_get_state();
#else
	return APP_LRW_STATE_IDLE;
#endif
}

bool app_radio_is_ready(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_p2p_is_ready();
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_lrw_is_ready();
#else
	return false;
#endif
}

uint8_t app_radio_get_max_payload(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_p2p_get_max_payload();
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_lrw_get_max_payload();
#else
	return 0;
#endif
}

void app_radio_send_telemetry(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_p2p_send_telemetry();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_lrw_send_telemetry();
#endif
}

void app_radio_send_telemetry_now(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_p2p_send_telemetry(); /* no pre-send jitter to skip */
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_lrw_send_telemetry_now();
#endif
}

int app_radio_queue_response(uint8_t port, const uint8_t *buf, size_t len)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_p2p_queue_response(port, buf, len);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_lrw_queue_response(port, buf, len);
#else
	return -ENODEV;
#endif
}

int app_radio_send_alarm(const uint8_t *buf, size_t len)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_p2p_send_alarm(buf, len);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_lrw_send_alarm(buf, len);
#else
	return -ENODEV;
#endif
}

void app_radio_register_ready_cb(void (*cb)(void))
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_p2p_register_ready_cb(cb);
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_lrw_register_ready_cb(cb);
#endif
}

void app_radio_suspend(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_p2p_suspend();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_lrw_suspend();
#endif
}
