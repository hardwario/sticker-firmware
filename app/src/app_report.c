/*
 * Copyright (c) 2025 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"
#include "app_counters.h"
#include "app_history.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_report.h"
#include "app_sensor.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Standard includes */
#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_report, LOG_LEVEL_DBG);

/* Own work queue so the sensor read (app_sensor_sample) and the history flash
 * write (app_history_capture) never run on the LoRaWAN TX work queue or the
 * system work queue (the latter also drives LoRaMacProcess()).
 *
 * 3072 B (was 2048): the report cycle nests sensor I2C reads + an NVS flash write
 * (app_history_capture / app_counters_save); release builds carry no stack canary
 * so an overflow corrupts RAM silently. Headroom over the measured high-water
 * mark; revisit with CONFIG_INIT_STACKS on hardware (#187). */
static K_THREAD_STACK_DEFINE(m_work_stack, 3072);
static struct k_work_q m_work_q;

static struct k_timer m_report_timer; /* interval_report cadence */
/* Two entry points into the same report body, distinguished by whether the run
 * is on the fixed cadence or an ad-hoc trigger. Only the periodic path captures
 * history and re-arms the cadence, so off-cadence triggers (alarm / force_send /
 * sample / link-ready kick) can neither append an out-of-cadence record nor
 * shift the timer — history stays at exactly interval_report (#H1). */
static struct k_work m_periodic_work; /* fixed-cadence cycle: sample + capture + send */
static struct k_work m_trigger_work;  /* ad-hoc cycle: sample + send, no history capture */

#if defined(CONFIG_WATCHDOG)
/* Liveness heartbeat (#182): mirror of the app_lrw guard for the report queue, so
 * a wedge in the sample/capture path is caught by the IWDG too. */
#define REPORT_HEARTBEAT_PERIOD_SEC 5
#define REPORT_HEARTBEAT_TIMEOUT_MS 30000
static int m_wdog_channel = -1;
static struct k_work_delayable m_heartbeat_work;

static void heartbeat_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	app_wdog_ping(m_wdog_channel);
	k_work_schedule_for_queue(&m_work_q, &m_heartbeat_work,
				  K_SECONDS(REPORT_HEARTBEAT_PERIOD_SEC));
}
#endif /* defined(CONFIG_WATCHDOG) */

/* (Re)arm the periodic cadence for the next report. One-shot + manual restart
 * (like the old app_lrw m_send_timer) so a multi-frame snapshot in app_lrw
 * doesn't get a second cycle stacked behind it. */
static void schedule_next_report(void)
{
	/* FIXED cadence, no jitter. This timer also drives app_sensor_sample() and
	 * app_history_capture() below, and history replay reconstructs each record's
	 * time as base + ordinal * interval_report — a *fixed* interval. Jittering the
	 * period would make the stored samples land at 60..66 s (for a 60 s interval)
	 * while replay assumes exactly 60 s, drifting every timestamp cumulatively
	 * (the old signed jitter averaged out; a one-sided jitter biases it). Fleet-
	 * uplink de-correlation is instead a random *pre-send* delay applied in app_lrw
	 * (app_lrw_send_telemetry), which shifts only the transmission, not the sample/
	 * history-capture cadence (#267). */
	k_timer_start(&m_report_timer, K_SECONDS(g_app_config.interval_report), K_FOREVER);
}

/* One report cycle. `periodic` is true only on the fixed-cadence timer path;
 * ad-hoc triggers pass false and therefore neither capture history nor re-arm
 * the cadence, so the history inter-record interval stays exactly
 * interval_report regardless of how many alarms / force_sends / samples fire in
 * between (#H1). */
static void run_report(bool periodic)
{
	/* Re-arm the cadence up front (periodic path only) so a skipped cycle still
	 * keeps ticking; a trigger must NOT restart it, or the next periodic record
	 * would land < interval_report after the previous one. */
	if (periodic) {
		schedule_next_report();
	}

	/* Persist the pulse totalizers at the report cadence (dirty-flagged, no-op
	 * when unchanged). Done before the link/calibration gates below so counters
	 * survive a power loss even while the device is not joined. */
	app_counters_save(false);

	/* Block normal reporting during calibration mode. */
	if (g_app_config.calibration) {
		return;
	}

	/* Sample BEFORE the link gate, so store-and-forward works offline: a device
	 * that has not joined (weak/absent gateway) still records history at the
	 * report cadence and replays it after join. Only the transport is link-gated
	 * below. (Same rationale as app_counters_save above.) */

	/* Lazy sampling: when interval_sample == 0 the sensors are read here, in the
	 * report cycle, instead of on a dedicated sensor timer. */
	if (!g_app_config.interval_sample) {
		app_sensor_sample();
	}

	/* Capture one history record — ONLY on the fixed cadence, so records are
	 * spaced at exactly interval_report and replay's base + ord*interval time
	 * reconstruction holds. Self-skips while a replay is active (#126). */
	if (periodic) {
		app_history_capture();
	}

	/* State-gated cadence: skip the UPLINK while the link is joining/
	 * reconnecting/disabled. app_lrw kicks us (report_kick) on the link-ready
	 * edge to resume promptly and drain the buffered history. */
	if (!app_lrw_is_ready()) {
		LOG_DBG("Report skipped: link not ready (%d)", app_lrw_get_state());
		return;
	}

	/* Hand off to the transport: app_lrw composes the snapshot and splits it
	 * into DR-budget frames (LC piggyback + duty-cycle retry live there). */
	app_lrw_send_telemetry();
}

static void periodic_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	run_report(true);
}

static void trigger_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	run_report(false);
}

static void report_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit_to_queue(&m_work_q, &m_periodic_work);
}

/* Link-ready edge from the transport (join success / history-replay finish):
 * send an immediate report to drain the buffered history, but leave the fixed
 * cadence (and its history capture) untouched. */
static void report_kick(void)
{
	k_work_submit_to_queue(&m_work_q, &m_trigger_work);
}

void app_report_trigger(void)
{
	k_work_submit_to_queue(&m_work_q, &m_trigger_work);
}

void app_report_suspend(void)
{
	/* Stop the cadence so it can't fire and re-arm the radio during poweroff.
	 * Pending work on m_work_q is abandoned (the MCU is about to shut down;
	 * wake is a clean boot). */
	k_timer_stop(&m_report_timer);
}

int app_report_init(void)
{
	k_work_queue_init(&m_work_q);
	k_work_queue_start(&m_work_q, m_work_stack, K_THREAD_STACK_SIZEOF(m_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_work_init(&m_periodic_work, periodic_work_handler);
	k_work_init(&m_trigger_work, trigger_work_handler);
	k_timer_init(&m_report_timer, report_timer_handler, NULL);

#if defined(CONFIG_WATCHDOG)
	m_wdog_channel = app_wdog_register(REPORT_HEARTBEAT_TIMEOUT_MS);
	if (m_wdog_channel < 0) {
		LOG_ERR_CALL_FAILED_INT("app_wdog_register", m_wdog_channel);
	}
	k_work_init_delayable(&m_heartbeat_work, heartbeat_work_handler);
	k_work_schedule_for_queue(&m_work_q, &m_heartbeat_work, K_NO_WAIT);
#endif /* defined(CONFIG_WATCHDOG) */

	/* Arm the cadence at boot so the counter backup (report_work_handler runs
	 * app_counters_save before the link gate) fires even on a device that never
	 * joins — the worst-case lost-pulse window is interval_report regardless of
	 * the link state. Reporting itself still self-skips at the link gate until
	 * joined; app_lrw's ready kick re-arms with an immediate report on join. */
	schedule_next_report();
	app_lrw_register_ready_cb(report_kick);

	return 0;
}
