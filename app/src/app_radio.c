/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_cmd.h"
#include "app_config.h"
#include "app_log.h"
#include "app_radio_lrw.h"
#include "app_radio.h"

#if defined(CONFIG_RADIO_P2P)
#include "app_radio_p2p.h"
#endif

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <errno.h>

LOG_MODULE_REGISTER(app_radio, LOG_LEVEL_INF);

/* Kept out of a static entirely when CONFIG_RADIO_P2P=n: with P2P not even
 * compiled in, the radio is always running LoRaWAN by construction (radio_mode's
 * only other option, OFF, is handled inside app_radio_lrw itself, #271), so tracking
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
		return app_radio_p2p_init();
	}
	m_kind = APP_RADIO_LORAWAN;
#else
	if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_P2P) {
		LOG_WRN("radio-mode=p2p but CONFIG_RADIO_P2P=n; falling back to LoRaWAN");
	}
#endif
#if defined(CONFIG_LORAWAN)
	LOG_INF("Radio: LoRaWAN");
	return app_radio_lrw_init();
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
		app_radio_p2p_start();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_join();
#endif
}

void app_radio_rejoin(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_rejoin();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_join(); /* already unconditional -- see app_radio_start() above */
#endif
}

enum app_radio_kind app_radio_get_kind(void)
{
#if defined(CONFIG_RADIO_P2P)
	return m_kind;
#else
	return APP_RADIO_LORAWAN;
#endif
}

enum app_radio_state app_radio_get_state(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_get_state();
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_get_state();
#else
	return APP_RADIO_STATE_IDLE;
#endif
}

bool app_radio_last_downlink(int16_t *rssi, int8_t *snr, uint32_t *age_s)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_last_downlink(rssi, snr, age_s);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_last_downlink(rssi, snr, age_s);
#else
	ARG_UNUSED(rssi);
	ARG_UNUSED(snr);
	ARG_UNUSED(age_s);
	return false;
#endif
}

bool app_radio_is_ready(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_is_ready();
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_is_ready();
#else
	return false;
#endif
}

uint8_t app_radio_get_max_payload(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_get_max_payload();
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_get_max_payload();
#else
	return 0;
#endif
}

void app_radio_send_telemetry(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_send_telemetry();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_send_telemetry();
#endif
}

void app_radio_send_telemetry_now(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_send_telemetry(); /* no pre-send jitter to skip */
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_send_telemetry_now();
#endif
}

int app_radio_queue_response(uint8_t port, const uint8_t *buf, size_t len)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_queue_response(port, buf, len);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_queue_response(port, buf, len);
#else
	return -ENODEV;
#endif
}

int app_radio_send_alarm(const uint8_t *buf, size_t len)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_send_alarm(buf, len);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_send_alarm(buf, len);
#else
	return -ENODEV;
#endif
}

void app_radio_register_ready_cb(void (*cb)(void))
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_register_ready_cb(cb);
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_register_ready_cb(cb);
#endif
}

void app_radio_suspend(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_suspend();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_suspend();
#endif
}

/* ---- Boot/join announce --------------------------------------------------- */

#define ANNOUNCE_INFO     BIT(0)
#define ANNOUNCE_SETTINGS BIT(1)

/* The response buffers of both backends are 64 B (APP_RADIO_LRW_RESPONSE_BUF_SIZE,
 * P2P_TX_BUF_SIZE); the budget below caps the page size further. */
#define ANNOUNCE_BUF_SIZE 64

static atomic_t m_announce;

/* Have the backend call app_radio_announce_run() on its work queue. */
static void announce_kick(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_announce_kick();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_announce_kick();
#endif
}

/* Payload budget (bytes) of the next response frame, at most `buf_size`. */
static size_t response_cap(size_t buf_size)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_response_cap(buf_size);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_response_cap(buf_size);
#else
	return 0;
#endif
}

static int queue_announce(bool settings, const uint8_t *buf, size_t len)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		return app_radio_p2p_queue_announce(settings, buf, len);
	}
#endif
#if defined(CONFIG_LORAWAN)
	return app_radio_lrw_queue_announce(settings, buf, len);
#else
	ARG_UNUSED(settings);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return -ENODEV;
#endif
}

/* Page 0 went out and more pages follow: start the backend's page stream. */
static void page_stream_kick(void)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_page_stream_kick();
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_page_stream_kick();
#endif
}

/* Queue page 0 of an Info (settings = false) or settings-info, and start the
 * page stream when more pages follow. */
static int announce_frame(bool settings, uint32_t seq)
{
	uint8_t buf[ANNOUNCE_BUF_SIZE];
	size_t cap = response_cap(sizeof(buf));
	size_t len;
	bool more = false;
	int ret = settings ? app_cmd_build_config_status(buf, cap, &len, &more)
			   : app_cmd_build_info_seq(seq, buf, cap, &len, &more);

	if (ret) {
		return ret;
	}
	ret = queue_announce(settings, buf, len);
	if (ret) {
		if (more) {
			app_cmd_stream_cancel(); /* page 0 never left */
		}
		return ret;
	}
	if (more) {
		page_stream_kick();
	}
	return 0;
}

void app_radio_announce(void)
{
	atomic_or(&m_announce, ANNOUNCE_INFO | ANNOUNCE_SETTINGS);
	announce_kick();
}

bool app_radio_announce_pending(void)
{
	return atomic_get(&m_announce) != 0;
}

void app_radio_announce_rearm(bool settings)
{
	atomic_or(&m_announce, settings ? ANNOUNCE_SETTINGS : ANNOUNCE_INFO);
}

bool app_radio_announce_run(void)
{
	enum app_radio_state state = app_radio_get_state();

	if (state != APP_RADIO_STATE_HEALTHY && state != APP_RADIO_STATE_WARNING) {
		return false; /* the next link-up re-announces from scratch */
	}
	if (app_cmd_stream_active()) {
		return true; /* run again when the running page stream ends */
	}

	if (atomic_get(&m_announce) & ANNOUNCE_INFO) {
		if (announce_frame(false, 0) == 0) {
			atomic_and(&m_announce, ~ANNOUNCE_INFO);
			LOG_INF("Info announced");
		}
		if (app_cmd_stream_active()) {
			return true; /* settings-info follows once these pages are out */
		}
	}
	if (atomic_get(&m_announce) & ANNOUNCE_SETTINGS) {
		if (announce_frame(true, 0) == 0) {
			atomic_and(&m_announce, ~ANNOUNCE_SETTINGS);
			LOG_INF("Settings-info announced");
		}
	}
	return atomic_get(&m_announce) != 0;
}

int app_radio_send_info(uint32_t seq)
{
	int ret = announce_frame(false, seq);

	if (ret) {
		/* Not even one Info field fits now: the announce Info goes later. */
		atomic_or(&m_announce, ANNOUNCE_INFO);
	}
	return ret;
}

void app_radio_clock_sync(uint32_t seq)
{
#if defined(CONFIG_RADIO_P2P)
	if (is_p2p()) {
		app_radio_p2p_clock_sync(seq);
		return;
	}
#endif
#if defined(CONFIG_LORAWAN)
	app_radio_lrw_clock_sync(seq);
#else
	ARG_UNUSED(seq);
#endif
}
