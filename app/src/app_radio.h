/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_RADIO_H_
#define APP_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Link state of the active radio, shared by both backends (doc/plan/439 T1).
 * The values are the wire values of Response.RadioState.State
 * (app_config.proto), so keep the order. LoRaWAN: IDLE before the first join, JOINING, HEALTHY,
 * WARNING while link checks fail, RECONNECT after the link was lost, DISABLED when the DevEUI is
 * all-zero (#98). P2P: IDLE when unpaired and not joining (after a Detach), JOINING for a
 * boot/forced join, HEALTHY when paired, WARNING after P2P_WARNING_FAIL_THRESHOLD failed confirmed
 * cycles, RECONNECT for a self-heal/RejoinRequest join, DISABLED when lrw_appkey or lrw_deveui is
 * all-zero. */
enum app_radio_state {
	APP_RADIO_STATE_IDLE,
	APP_RADIO_STATE_JOINING,
	APP_RADIO_STATE_HEALTHY,
	APP_RADIO_STATE_WARNING,
	APP_RADIO_STATE_RECONNECT,
	APP_RADIO_STATE_DISABLED,
};

/* Radio facade (#118): one image links both the LoRaWAN stack (app_radio_lrw) and
 * the raw-LoRa P2P stack (app_radio_p2p, when CONFIG_RADIO_P2P=y). The active
 * one is chosen at boot from the `radio_mode` config parameter (off/lorawan/p2p)
 * and never changes at runtime (the SX126x radio is shared). The
 * radio-agnostic layers (app_report, app_compose, app_alarm, main) call
 * through this facade; the few LoRaWAN-only operations (join NVM reset, link
 * check, history replay, calibration's ABP join, …) stay direct app_radio_lrw_* calls
 * guarded by CONFIG_LORAWAN, unaffected by radio_mode. */

enum app_radio_kind {
	APP_RADIO_LORAWAN,
	APP_RADIO_P2P,
};

/* Read `radio_mode` from config and bring up the chosen stack (app_radio_lrw_init or
 * app_radio_p2p_init). Falls back to LoRaWAN if P2P is selected but not compiled in.
 * `radio_mode == off` also routes to app_radio_lrw_init(), which its own
 * radio_disabled() check keeps radio-silent. Returns 0 or a negative errno. */
int app_radio_init(void);

/* Start the link at boot: LoRaWAN always (re)joins; P2P only starts the join
 * handshake if NVS has no valid pairing yet, otherwise just marks ready --
 * a P2P session persists across a normal power cycle (doc/p2p.md §7), so
 * boot-time app_radio_p2p_start() must not waste a JoinRequest on every reboot. */
void app_radio_start(void);

/* Force a fresh join attempt RIGHT NOW, regardless of current state --
 * unlike app_radio_start(), an existing P2P pairing is not treated as
 * sufficient. LoRaWAN already behaves this way unconditionally (every
 * app_radio_lrw_join() call deinits and rejoins); this is what makes `join`
 * genuinely symmetric across both stacks -- the shell command and the lrw_join
 * command (NFC / downlink). A successful P2P JoinAccept simply overwrites the
 * old pairing via pairing_persist(), so this never needs a reboot or NVS
 * wipe. */
void app_radio_rejoin(void);

/* Which stack was selected at boot. */
enum app_radio_kind app_radio_get_kind(void);

/* Link state of the active radio (see enum app_radio_state): drives the status
 * LED, RadioState.state and the device_status radio bits for either backend. */
enum app_radio_state app_radio_get_state(void);

/* Link state and diagnostics of the active radio (Response.RadioState, #446),
 * one shape for LoRaWAN and P2P. app_radio owns the data: each backend pushes
 * the facts as they happen through the app_radio_note_* / app_radio_set_* /
 * app_radio_count() calls below, and every reader (Info, get_radio_state, the
 * shell) takes a snapshot with app_radio_get_status() -- nobody reads a
 * backend directly. The has_* flags mark values the active radio has
 * reported. */

/* Link events counted since boot, with the same meaning on both radios. */
enum app_radio_counter {
	APP_RADIO_CNT_TX,     /* uplink transmission (retransmissions in, joins out) */
	APP_RADIO_CNT_RX,     /* downlink received (counted by app_radio_note_downlink) */
	APP_RADIO_CNT_RETRY,  /* retransmission */
	APP_RADIO_CNT_FAIL,   /* uplink never confirmed (P2P Ack / LoRaWAN LinkCheck) */
	APP_RADIO_CNT_TX_ERR, /* send refused by the MAC or failed in the radio */
	APP_RADIO_CNT_JOIN,   /* join attempt */
	APP_RADIO_CNT_COUNT,
};

struct app_radio_status {
	enum app_radio_state state;
	/* Radio parameters of the uplinks now (app_radio_set_params). */
	uint8_t sf;          /* spreading factor, 0 = unknown */
	bool has_datarate;   /* LoRaWAN */
	uint8_t datarate;    /* DR index */
	bool has_tx_power;   /* tx_power_dbm valid */
	int8_t tx_power_dbm; /* conducted TX power (dBm), capped by the PA */
	/* Last downlink as measured by the node (app_radio_note_downlink). */
	bool has_dl;
	int16_t dl_rssi;
	int8_t dl_snr;
	uint32_t dl_age_s;
	bool has_dl_unix; /* the RTC is synced */
	uint32_t dl_unix_time;
	/* Last uplink as received by the other side. */
	bool has_ul_rssi; /* P2P: RSSI/SNR the Hub reported in the last Ack */
	int16_t ul_rssi;
	int8_t ul_snr;
	bool has_ul_margin; /* LoRaWAN: last LinkCheckAns */
	uint8_t ul_margin;
	uint8_t ul_gw_count;
	/* Current or last session (app_radio_set_session). */
	bool has_session;
	uint32_t dev_addr;
	uint32_t fcnt_up; /* next uplink frame counter */
	/* Link health now. */
	uint32_t fail_streak;
	uint32_t join_attempts;
	uint32_t duty_blocked_s; /* 0 = not held by the duty cycle */
	bool has_airtime;        /* P2P */
	uint32_t airtime_hour_ms;
	/* Counters since boot (enum app_radio_counter) and their time base. */
	uint32_t uptime_s;
	uint32_t cnt[APP_RADIO_CNT_COUNT];
};

/* Snapshot for a reader: the pushed values plus the link state, the ages and
 * the downlink's wall-clock time computed now. */
void app_radio_get_status(struct app_radio_status *st);

/* Backend push API. Callable from any thread (spinlock inside). */
void app_radio_count(enum app_radio_counter c);
/* An authenticated downlink was received: its node-measured RSSI (dBm) and SNR
 * (dB); also counts APP_RADIO_CNT_RX. */
void app_radio_note_downlink(int16_t rssi, int8_t snr);
/* Radio parameters now; datarate < 0 = none (P2P). TX power is capped by the PA. */
void app_radio_set_params(uint8_t sf, int datarate, int8_t tx_power_dbm);
/* P2P: the RSSI/SNR the Hub measured on the last acknowledged uplink. */
void app_radio_set_uplink_rssi(int16_t rssi, int8_t snr);
/* LoRaWAN: the last LinkCheckAns. */
void app_radio_set_uplink_margin(uint8_t margin, uint8_t gw_count);
/* The session's address and next uplink frame counter. */
void app_radio_set_session(uint32_t dev_addr, uint32_t fcnt_up);
/* Consecutive unconfirmed uplinks, and the step of the current (re)join episode. */
void app_radio_set_fail_streak(uint32_t n);
void app_radio_set_join_attempts(uint32_t n);
/* Sends are (true) / are no longer (false) held by the duty cycle; the hold
 * time runs from the first `true`. */
void app_radio_set_duty_held(bool held);
/* P2P: airtime used in the sliding hour (duty ledger). */
void app_radio_set_airtime(uint32_t ms);

/* True when the link can carry an uplink now. */
bool app_radio_is_ready(void);

/* Application-payload budget (bytes) for the next uplink. */
uint8_t app_radio_get_max_payload(void);

/* Compose + send a telemetry snapshot (triggered by app_report) after the fleet
 * pre-send jitter (#267): a random delay of up to min(interval_report / 10,
 * 10 s), the same policy for both radios. The jitter lives on the transmission,
 * never on the report cadence (history timestamps follow the fixed cadence). */
void app_radio_send_telemetry(void);

/* Same, for a host-requested uplink (force_send / sample, F14): no jitter, and a
 * jittered report still pending is folded into this send. */
void app_radio_send_telemetry_now(void);

/* Forget the network session on every radio stack before a reset-tier reboot
 * (factory_reset / vendor_reset / lrw_reset): the LoRaWAN NVM (frame counters,
 * DevNonce, session) and the P2P pairing. The P2P dev_nonce and frame counter
 * are kept -- the central's replay protection needs them to keep advancing. */
void app_radio_reset_link(void);

/* ---- Stale-uplink watchdog policy (M-2, F29; shared by both radios) -------
 * A joined station whose telemetry has not left for APP_RADIO_STALE_FACTOR x
 * interval_report is mute although its work queue drains: force a rejoin.
 * Unless the duty cycle explains it -- sends held by the duty cycle recently
 * (within one interval + margin) and for no longer than the 1 h window plus a
 * margin: the radio is alive and throttled, and a rejoin would only reset the
 * LoRaMac band credits (F29). Pure, so both backends and the tests share it. */
#define APP_RADIO_STALE_FACTOR              4
#define APP_RADIO_STALE_DC_HOLD_MAX_MS      (75LL * 60 * 1000)
#define APP_RADIO_STALE_DC_RECENT_MARGIN_MS (3LL * 60 * 1000)

enum app_radio_stale {
	APP_RADIO_STALE_OK = 0,  /* an uplink left recently enough (or no clock) */
	APP_RADIO_STALE_HOLD_DC, /* stale, but the duty cycle explains it: wait */
	APP_RADIO_STALE_REJOIN,  /* stale with no duty-cycle excuse: force rejoin */
};

/* Duty-cycle hold streak: first and most recent held send (uptime ms, 0 = none). */
struct app_radio_stale_dc {
	int64_t since_ms;
	int64_t last_ms;
};

/* Record a send attempt: `held` = refused / deferred by the duty cycle extends the
 * streak, `sent` clears it, anything else (a radio error) leaves it. */
static inline void app_radio_stale_note(struct app_radio_stale_dc *dc, bool sent, bool held,
					int64_t now_ms)
{
	if (sent) {
		dc->since_ms = 0;
		dc->last_ms = 0;
	} else if (held) {
		if (dc->since_ms == 0) {
			dc->since_ms = now_ms;
		}
		dc->last_ms = now_ms;
	}
}

/* `last_uplink_ms` = uptime of the last telemetry uplink (0 = none since the last
 * (re)join: no decision), `interval_s` = interval_report (0: no decision). */
static inline enum app_radio_stale app_radio_stale_check(int64_t now_ms, int64_t last_uplink_ms,
							 const struct app_radio_stale_dc *dc,
							 uint32_t interval_s)
{
	if (last_uplink_ms == 0 || interval_s == 0) {
		return APP_RADIO_STALE_OK;
	}

	int64_t interval_ms = (int64_t)interval_s * 1000;

	if (now_ms - last_uplink_ms <= interval_ms * APP_RADIO_STALE_FACTOR) {
		return APP_RADIO_STALE_OK;
	}
	if (dc->since_ms != 0 && dc->last_ms != 0 &&
	    now_ms - dc->last_ms <= interval_ms + APP_RADIO_STALE_DC_RECENT_MARGIN_MS &&
	    now_ms - dc->since_ms < APP_RADIO_STALE_DC_HOLD_MAX_MS) {
		return APP_RADIO_STALE_HOLD_DC;
	}
	return APP_RADIO_STALE_REJOIN;
}

/* Stage a command response for the next uplink. */
int app_radio_queue_response(uint8_t port, const uint8_t *buf, size_t len);

/* Stage an alarm-detail batch. */
int app_radio_send_alarm(const uint8_t *buf, size_t len);

/* Register the link-ready kick app_report uses to (re)start the cadence. */
void app_radio_register_ready_cb(void (*cb)(void));

/* Stop radio activity ahead of a deep-sleep poweroff. */
void app_radio_suspend(void);

/* ---- Boot/join announce (#412, #409 A5a, #425; doc/plan/439 T3) ----------
 * One path for both radios. When the link comes up (LoRaWAN join, P2P paired
 * at boot or by a JoinAccept) the backend calls app_radio_announce(); app_radio
 * then sends the Info (seq 0) followed by the settings-info ConfigDump, each
 * paged for the current budget. A frame that does not fit yet, or that waits
 * for a running page stream, stays pending and goes out on a later
 * app_radio_announce_run(): the backend runs it on its work queue whenever
 * room may have appeared (link up, DR rise, page stream end, queue space). */
void app_radio_announce(void);

/* Something of the announce is still to be sent. */
bool app_radio_announce_pending(void);

/* Backend: a queued announce frame had to be dropped (the budget fell under
 * it); arm it again for the next run. */
void app_radio_announce_rearm(bool settings);

/* Backend work queue only: send what is pending while the link is up. Returns
 * true while something stays pending that a later run can send. */
bool app_radio_announce_run(void);

/* Backend work queue only: an Info carrying `seq` (the clock_sync answer),
 * paged like the announce. When not even page 0 can go now, the seq-0
 * announce Info is armed instead. Returns 0 or a negative errno. */
int app_radio_send_info(uint32_t seq);

/* clock_sync with an empty body: re-sync the RTC from the network and answer
 * with an Info carrying `seq` once the time has landed -- the same shape on
 * both radios, no extra uplink forced: LoRaWAN's DeviceTimeReq rides on the
 * next uplink and the answer comes in its downlink; P2P uses the time tail of
 * the next uplink's Ack. */
void app_radio_clock_sync(uint32_t seq);

#ifdef __cplusplus
}
#endif

#endif /* APP_RADIO_H_ */
