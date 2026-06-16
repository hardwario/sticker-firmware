/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "app_history.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_report.h"
#include "app_sensor.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_report, LOG_LEVEL_DBG);

/* Own work queue so the sensor read (app_sensor_sample) and the history flash
 * write (app_history_capture) never run on the LoRaWAN TX work queue or the
 * system work queue (the latter also drives LoRaMacProcess()). */
static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
static struct k_work_q m_work_q;

static struct k_timer m_report_timer; /* interval_report cadence */
static struct k_work m_report_work;   /* one report cycle (sample + capture + trigger) */

/* (Re)arm the periodic cadence for the next report. One-shot + manual restart
 * (like the old app_lrw m_send_timer) so a multi-frame snapshot in app_lrw
 * doesn't get a second cycle stacked behind it. */
static void schedule_next_report(void)
{
	int timeout = g_app_config.interval_report;
#if defined(CONFIG_ENTROPY_GENERATOR)
	timeout += (int32_t)sys_rand32_get() % (g_app_config.interval_report / 10);
#endif
	k_timer_start(&m_report_timer, K_SECONDS(timeout), K_FOREVER);
}

static void report_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Re-arm the cadence up front so a skipped cycle still keeps ticking. */
	schedule_next_report();

	/* Block normal reporting during calibration mode. */
	if (g_app_config.calibration) {
		return;
	}

	/* State-gated cadence: skip while the link is joining/reconnecting/disabled.
	 * app_lrw kicks us (report_kick) on the link-ready edge to resume promptly. */
	if (!app_lrw_is_ready()) {
		LOG_DBG("Report skipped: link not ready (%d)", app_lrw_get_state());
		return;
	}

	/* Lazy sampling: when interval_sample == 0 the sensors are read here, in the
	 * report cycle, instead of on a dedicated sensor timer. */
	if (!g_app_config.interval_sample) {
		app_sensor_sample();
	}

	/* Capture one history record at the report cadence (self-skips while a
	 * replay is active, #126). */
	app_history_capture();

	/* Hand off to the transport: app_lrw composes the snapshot and splits it
	 * into DR-budget frames (LC piggyback + duty-cycle retry live there). */
	app_lrw_send_telemetry();
}

static void report_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit_to_queue(&m_work_q, &m_report_work);
}

/* Link-ready edge from the transport (join success / history-replay finish):
 * resume the cadence with an immediate report. */
static void report_kick(void)
{
	k_work_submit_to_queue(&m_work_q, &m_report_work);
}

void app_report_trigger(void)
{
	k_work_submit_to_queue(&m_work_q, &m_report_work);
}

int app_report_init(void)
{
	k_work_queue_init(&m_work_q);
	k_work_queue_start(&m_work_q, m_work_stack, K_THREAD_STACK_SIZEOF(m_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_work_init(&m_report_work, report_work_handler);
	k_timer_init(&m_report_timer, report_timer_handler, NULL);

	/* Cadence is not started here — it begins on the first link-ready kick from
	 * app_lrw (join success), then re-arms itself each cycle. */
	app_lrw_register_ready_cb(report_kick);

	return 0;
}
