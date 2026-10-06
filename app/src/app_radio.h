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

#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Link state of the active radio, shared by both backends (doc/plan/439 T1).
 * The values are the wire values of Response.RadioState.State
 * (app_config.proto), so keep the order. LoRaWAN: IDLE before the first join, JOINING, HEALTHY,
 * RECONNECT after the link was lost, DISABLED when the DevEUI is all-zero (#98). P2P: IDLE when
 * unpaired and not joining (after a Detach), JOINING for a boot/forced join, HEALTHY when paired,
 * RECONNECT for a self-heal/RejoinRequest join, DISABLED when radio_appkey or radio_deveui is
 * all-zero. WARNING is app_radio's on either radio: HEALTHY after
 * APP_RADIO_LINK_WARNING_THRESHOLD failed link checks in a row (app_radio_link_result()). */
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
 * check, calibration's ABP join, …) stay direct app_radio_lrw_* calls
 * guarded by CONFIG_LORAWAN, unaffected by radio_mode. */

enum app_radio_kind {
	APP_RADIO_LORAWAN,
	APP_RADIO_P2P,
};

struct k_work_q;

/* The radio work queue (doc/plan/439 T2a): one queue for whichever backend runs.
 * Both backends do all their TX, RX windows, state changes and command handling
 * on it, so each backend's state is touched by one thread only. Started before
 * main() (SYS_INIT), so calibration mode, which brings LoRaWAN up without
 * app_radio_init(), gets it too. */
struct k_work_q *app_radio_work_q(void);

/* Flash writes vs radio exchanges. The STM32WLE5 stalls on every flash
 * program/erase (interrupts included), which lands a TX's receive windows late
 * and loses the Ack or the JoinAccept. A backend brackets each exchange -- TX
 * start until its receive windows closed -- with app_radio_air_begin()/_end();
 * every flash writer (NVS settings, history ring) brackets its write with
 * app_radio_flash_hold()/_release(). A writer waits for a running exchange (at
 * most 10 s), an exchange for a running or waiting writer (at most 1 s); past
 * the cap either goes ahead. Writers on the radio or the system work queue
 * never wait (the exchange and LoRaMacProcess() run there). */
void app_radio_flash_hold(void);
void app_radio_flash_release(void);
/* Radio work queue. */
void app_radio_air_begin(void);
void app_radio_air_end(void);

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
	uint32_t duty_blocked_s;  /* 0 = not held by the duty cycle */
	bool has_airtime;         /* the ledger runs (T2d: both radios) */
	uint32_t airtime_hour_ms; /* air of the trailing hour */
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
/* The step of the current (re)join episode. (The failure streak is app_radio's
 * own: app_radio_link_result().) */
void app_radio_set_join_attempts(uint32_t n);
/* Sends are (true) / are no longer (false) held by the duty cycle; the hold
 * time runs from the first `true`. */
void app_radio_set_duty_held(bool held);
/* True when the link can carry an uplink now. */
bool app_radio_is_ready(void);

/* Application-payload budget (bytes) for the next uplink. */
uint8_t app_radio_get_max_payload(void);

/* Compose + send a telemetry snapshot (triggered by app_report) after the fleet
 * pre-send jitter (#267): a random delay of up to min(interval_report / 10,
 * 10 s), the same policy for both radios. A `periodic` report (its wall-clock
 * slot) first waits the node's stable uplink phase, derived from its DevEUI
 * within min(interval_report - jitter - 1 s, 60 s) (O9), so a fleet does not
 * send in the same seconds of every interval. Phase and jitter live on the
 * transmission, never on the report cadence (history timestamps follow the
 * fixed cadence). After a link-up the first report waits for the announce
 * instead and leaves right after it, without phase or jitter: Info,
 * settings-info, telemetry. */
void app_radio_send_telemetry(bool periodic);

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
 * (within one interval + margin, or a hold of known length still running) and
 * for no longer than the 1 h window plus a margin: the radio is alive and
 * throttled, and a rejoin would only reset the LoRaMac band credits (F29).
 * Pure, so both backends and the tests share it. */
#define APP_RADIO_STALE_FACTOR              4
#define APP_RADIO_STALE_DC_HOLD_MAX_MS      (75LL * 60 * 1000)
#define APP_RADIO_STALE_DC_RECENT_MARGIN_MS (3LL * 60 * 1000)

enum app_radio_stale {
	APP_RADIO_STALE_OK = 0,  /* an uplink left recently enough (or no clock) */
	APP_RADIO_STALE_HOLD_DC, /* stale, but the duty cycle explains it: wait */
	APP_RADIO_STALE_REJOIN,  /* stale with no duty-cycle excuse: force rejoin */
};

/* Duty-cycle hold streak: first and most recent held send, and the end of the
 * latest hold of known length (uptime ms, 0 = none). */
struct app_radio_stale_dc {
	int64_t since_ms;
	int64_t last_ms;
	int64_t until_ms;
};

/* Record a send attempt: `held` = refused / deferred by the duty cycle extends the
 * streak, `sent` clears it, anything else (a radio error) leaves it. */
static inline void app_radio_stale_note(struct app_radio_stale_dc *dc, bool sent, bool held,
					int64_t now_ms)
{
	if (sent) {
		dc->since_ms = 0;
		dc->last_ms = 0;
		dc->until_ms = 0;
	} else if (held) {
		if (dc->since_ms == 0) {
			dc->since_ms = now_ms;
		}
		dc->last_ms = now_ms;
	}
}

/* A send the duty ledger holds for `hold_ms`: nothing is tried again until the
 * hold ends, so the streak stays recent until then (T2d: a DR0 hold lasts up to
 * the hour, far past one interval + margin). */
static inline void app_radio_stale_note_hold(struct app_radio_stale_dc *dc, int64_t now_ms,
					     int64_t hold_ms)
{
	app_radio_stale_note(dc, false, true, now_ms);
	if (now_ms + hold_ms > dc->until_ms) {
		dc->until_ms = now_ms + hold_ms;
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
	int64_t recent_ms = dc->until_ms > dc->last_ms ? dc->until_ms : dc->last_ms;

	if (dc->since_ms != 0 && recent_ms != 0 &&
	    now_ms - recent_ms <= interval_ms + APP_RADIO_STALE_DC_RECENT_MARGIN_MS &&
	    now_ms - dc->since_ms < APP_RADIO_STALE_DC_HOLD_MAX_MS) {
		return APP_RADIO_STALE_HOLD_DC;
	}
	return APP_RADIO_STALE_REJOIN;
}

/* ---- Rejoin backoff (F2; shared by both radios) ---------------------------
 * A lost link is joined again after APP_RADIO_REJOIN_BACKOFF_BASE_MS, doubling
 * per attempt up to APP_RADIO_REJOIN_BACKOFF_MAX_MS; `attempt` counts from 0.
 * The caller spreads it with app_radio_backoff_jitter_ms(). Pure. */
#define APP_RADIO_REJOIN_BACKOFF_BASE_MS 60000U   /* first rejoin round: 60 s */
#define APP_RADIO_REJOIN_BACKOFF_MAX_MS  3600000U /* cap: 1 h */

static inline uint32_t app_radio_rejoin_backoff_ms(uint32_t attempt)
{
	uint32_t ms = APP_RADIO_REJOIN_BACKOFF_BASE_MS;

	for (uint32_t i = 0; i < attempt && ms < APP_RADIO_REJOIN_BACKOFF_MAX_MS; i++) {
		ms *= 2;
	}
	return MIN(ms, APP_RADIO_REJOIN_BACKOFF_MAX_MS);
}

/* +/-25 % of `base_ms` around `wait_ms` (M-1): a fleet that lost its link
 * together (a gateway or Hub outage) must not rejoin in lockstep. Never under
 * `floor_ms` (P2P: the duty-cycle wait the jitter may not undercut). */
static inline int64_t app_radio_backoff_jitter_ms(int64_t wait_ms, int64_t floor_ms,
						  uint32_t base_ms, uint32_t rand32)
{
	int64_t jittered = wait_ms - (int64_t)(base_ms / 4) + (int64_t)(rand32 % (base_ms / 2 + 1));

	return MAX(jittered, floor_ms > 0 ? floor_ms : 0);
}

/* ---- Confirmed uplinks (doc/plan/460 §2.6, T2c; shared by both radios) ------
 * A confirmed frame the backend sent without an Ack goes again after a random
 * 1..2^n s (n = the retry, 1-based) on top of any duty-cycle wait, like
 * LoRaWAN's ACK_TIMEOUT (2 +/- 1 s), APP_RADIO_ACK_MAX_RETRIES times at most.
 * The spread keeps two nodes rebooted together from locking into each other's
 * RX1 (F-P2P-4). Pure. */
#define APP_RADIO_ACK_MAX_RETRIES    3
#define APP_RADIO_ACK_BACKOFF_MIN_MS 1000U

static inline uint32_t app_radio_ack_backoff_ms(uint32_t retry, uint32_t rand32)
{
	uint32_t n = CLAMP(retry, 1U, (uint32_t)APP_RADIO_ACK_MAX_RETRIES);
	uint32_t max_ms = APP_RADIO_ACK_BACKOFF_MIN_MS << n;

	return APP_RADIO_ACK_BACKOFF_MIN_MS + rand32 % (max_ms - APP_RADIO_ACK_BACKOFF_MIN_MS);
}

/* ---- Link-check cadence (PF-1, decision #23; shared by both radios) ---------
 * The report with 0-based index `report_idx` since the last link-up is a link
 * check when it is the first one or every `interval`-th after it (0, N, 2N ...),
 * and every report is one while the link is WARNING (#424). `interval` <= 0
 * turns the periodic check off. LoRaWAN rides a LinkCheckReq on the report,
 * P2P sends it CONFIRMED. Pure, so both backends and the tests share it. */
static inline bool app_radio_link_check_due(uint32_t report_idx, int interval, bool warning)
{
	if (warning) {
		return true;
	}
	if (interval <= 0) {
		return false;
	}
	return (report_idx % (uint32_t)interval) == 0U;
}

/* ---- Time on air (doc/plan/460 §2.7, T2d; shared by both radios) ------------
 * Air time in ms of a LoRa frame with `len` PHY-payload bytes at `sf` and
 * `bw_hz`: CR 4/5, an 8-symbol preamble (12 at SF5/SF6, which the SX126x driver
 * forces), explicit header, CRC on. The arithmetic and the rounding up are
 * LoRaMac's RadioTimeOnAir(), so the duty ledger charges a LoRaWAN frame
 * exactly what the MAC charges its band. Integer-only (no FPU). Pure. */
static inline uint32_t app_radio_lora_toa_ms(uint32_t sf, uint32_t bw_hz, size_t len)
{
	bool low_dr = (bw_hz == 125000U && sf >= 11) || (bw_hz == 250000U && sf == 12);
	int32_t num = 8 * (int32_t)MIN(len, 255U) + 16 - 4 * (int32_t)sf + 20;
	int32_t den = 4 * (int32_t)sf;
	/* Preamble + 12 (4 sync symbols and the 8 base payload symbols); the 1/4
	 * sync symbol is added below. */
	uint32_t n_sym = 8 + 12;

	if (sf <= 6) {
		n_sym = 12 + 12 + 2; /* SF5/SF6: the longer preamble, 2 symbols more */
	} else {
		num += 8;
		den = low_dr ? 4 * ((int32_t)sf - 2) : den;
	}
	n_sym += (uint32_t)((MAX(num, 0) + den - 1) / den) * 5;

	/* (n_sym + 1/4) symbols of 2^sf / bw_hz s, in ms, rounded up. */
	uint64_t num_ms = (uint64_t)(4 * n_sym + 1) * (1U << (sf - 2)) * 1000U;

	return (uint32_t)((num_ms + bw_hz - 1) / bw_hz);
}

/* ---- Duty cycle (doc/plan/460 §2.7, T2d; shared by both radios) -------------
 * One exact sliding-hour ledger in app_radio for the running radio: every
 * frame is charged with its time on air when it leaves, and app_radio sends a
 * frame only when the air of the trailing hour plus this frame fits the
 * limit. A held frame waits exactly until it fits (-EAGAIN with that wait). */
#define APP_RADIO_DUTY_WINDOW_MS 3600000U /* the sliding window: one hour */
#define APP_RADIO_DUTY_1PCT_MS   36000U   /* 1 % of it */

/* The allowance per sliding hour of the EU868 sub-band `freq_hz` is in, by the
 * band plan of LoRaMac's RegionEU868 (ETSI EN 300 220): 863-865 MHz 0.1 %,
 * 865-868.6 MHz 1 %, 868.7-869.2 MHz 0.1 %, 869.4-869.65 MHz 10 %, 869.7-870 MHz
 * 1 %. A frequency in a gap or outside the band gets the strictest, 0.1 %. Pure. */
static inline uint32_t app_radio_duty_budget_ms(uint32_t freq_hz)
{
	if ((freq_hz >= 865000000U && freq_hz <= 868600000U) ||
	    (freq_hz >= 869700000U && freq_hz <= 870000000U)) {
		return APP_RADIO_DUTY_1PCT_MS;
	}
	if (freq_hz >= 869400000U && freq_hz <= 869650000U) {
		return APP_RADIO_DUTY_1PCT_MS * 10;
	}
	return APP_RADIO_DUTY_1PCT_MS / 10;
}

/* Ring capacity: one entry per transmission still inside the window. When the
 * ring is full the two OLDEST entries are folded into one (summed air, the
 * later end time) instead of making the frame wait for a slot, so the air-time
 * budget, not the entry count, is the only limit (F-P2P-1). The folded entry
 * leaves the window a little later than its older half would have, which can
 * only over-count air, never under-count it: every sliding hour still stays
 * within the limit. doc/p2p.md §6. */
#define APP_RADIO_DUTY_LEDGER_ENTRIES 48

struct app_radio_duty_entry {
	uint32_t end_ms; /* uptime (ms, truncated) at which the frame finished */
	uint32_t air_ms; /* its time on air; a folded entry holds the sum of two */
};

struct app_radio_duty {
	struct app_radio_duty_entry entries[APP_RADIO_DUTY_LEDGER_ENTRIES];
	uint32_t budget_ms; /* air allowed per sliding hour; 0 = no limit */
	uint8_t head;       /* index of the oldest entry */
	uint8_t count;      /* entries in use */
};

/* The running radio's limit per sliding hour (app_radio_duty_budget_ms(); 0 =
 * none, a LoRaWAN region without a duty cycle, where the air is still counted
 * for RadioState). Empties the ledger. From the backend's init. */
void app_radio_duty_init(uint32_t budget_ms);

/* Radio work queue: how long `air_ms` of air must still wait, 0 = it may go
 * now. While it waits, the sends count as duty-held (RadioState
 * duty_blocked_s). app_radio checks every frame it sends; a backend checks
 * only a frame of its own (a P2P JoinRequest). */
int64_t app_radio_duty_wait_ms(uint32_t air_ms);

/* Radio work queue: `air_ms` of air just went out (or may have: a failed TX
 * that keyed the PA). Charged to the ledger; the duty cycle holds nothing
 * now. */
void app_radio_duty_charge(uint32_t air_ms);

/* ---- Uplink frames, queues and scheduler (doc/plan/460 §2.2, F4) ----------
 * One TX path for both radios. Answers (command responses, Info,
 * settings-info) and alarm batches wait in two queues of
 * APP_RADIO_TX_QUEUE_DEPTH; telemetry is a request composed at send time.
 * One work item on the radio work queue sends them one frame per run in the
 * order answer > alarm > telemetry and applies one rule to every send result
 * (struct app_radio_backend.send). Frames wait while the link is down. */

enum app_radio_frame_kind {
	APP_RADIO_FRAME_ANSWER,
	APP_RADIO_FRAME_ALARM,
	APP_RADIO_FRAME_TELEMETRY,
	APP_RADIO_FRAME_HISTORY,
};

/* What a queued answer is, for the over-budget recovery (#409 3g). */
enum app_radio_frame_tag {
	APP_RADIO_TAG_OTHER = 0,    /* no recovery (shell-injected, error frames) */
	APP_RADIO_TAG_CMD_RESPONSE, /* answer to a downlink command: carries its seq */
	APP_RADIO_TAG_INFO,         /* autonomous Info (announce / clock_sync) */
	APP_RADIO_TAG_SETTINGS,     /* autonomous settings-info (#412) */
};

#define APP_RADIO_FRAME_CONFIRMED  BIT(0) /* wait for the Ack: TOWER flags, LoRaWAN MType */
/* A link check: LoRaWAN rides a LinkCheckReq on it; on P2P it is confirmed and
 * its Ack is the check, so only this frame unacknowledged is a failed one. */
#define APP_RADIO_FRAME_LINK_CHECK BIT(1)
#define APP_RADIO_FRAME_MORE       BIT(2) /* telemetry: more frames of this report follow */

/* Slot size of the answer and alarm queues. */
#define APP_RADIO_TX_SLOT_SIZE   64
#define APP_RADIO_TX_QUEUE_DEPTH 4

struct app_radio_frame {
	uint8_t kind;  /* enum app_radio_frame_kind */
	uint8_t tag;   /* enum app_radio_frame_tag */
	uint8_t port;  /* LoRaWAN fPort of an answer (0 = the command port) */
	uint8_t flags; /* APP_RADIO_FRAME_* */
	/* Retries of a confirmed frame so far (0 = its first TX). On a retry P2P
	 * sends the same counter again; LoRaWAN takes a new FCnt. */
	uint8_t attempt;
	uint16_t len; /* 0 = no payload: LoRaWAN flushes its pending MAC answers */
	uint8_t *buf;
};

/* What a send left for the scheduler besides its return value. */
struct app_radio_tx_result {
	uint32_t wait_ms; /* -EAGAIN: when to try again (0 = the default 15 s) */
	uint8_t budget;   /* -EMSGSIZE: the budget the frame did not fit */
};

/* The part of a backend the common TX path drives. Selected once by
 * app_radio_init() from radio_mode. Every hook runs on the radio work queue. */
struct app_radio_backend {
	/* Send one frame now; blocks for the TX and its receive windows. Returns
	 * 0 when it was transmitted, or:
	 *   -EAGAIN   duty-cycle held (or a LoRaWAN MAC flush went instead): try
	 *             again after res->wait_ms, not counted as a failure
	 *   -ETIMEDOUT a confirmed frame went out, but no Ack came: app_radio
	 *             sends it again (T2c), nothing else in between
	 *   -EBUSY    the radio cannot send now (P2P listen mode): wait, the
	 *             backend kicks app_radio_tx_kick() when it can
	 *   -ENOTCONN no session: wait for the next link-up
	 *   -EMSGSIZE over res->budget: recovered by kind
	 *   other     radio / MAC error: retried after 15 s, 8 times at most */
	int (*send)(const struct app_radio_frame *f, struct app_radio_tx_result *res);
	/* Payload budget of the next uplink; 0 = none now (pending LoRaWAN MAC
	 * answers fill the frame). */
	uint8_t (*budget)(void);
	/* The link carries uplinks now; false keeps every frame waiting. */
	bool (*tx_ready)(void);
	/* A report starts: its APP_RADIO_FRAME_CONFIRMED / _LINK_CHECK flags.
	 * `due`: app_radio's cadence makes this report a link check (LoRaWAN
	 * rides a LinkCheckReq on it, P2P sends it confirmed). A link check rides
	 * the first frame only. */
	uint8_t (*report_flags)(bool due);
	/* Link state of the backend (enum app_radio_state), never WARNING:
	 * app_radio lays its link supervision over HEALTHY. */
	enum app_radio_state (*get_state)(void);
	/* WARNING's recovery rung, one per failed link check: LoRaWAN restores
	 * the default TX power, then steps one DR down; P2P steps the TX power up
	 * towards p2p-tx-power. Returns false at the floor. */
	bool (*warning_step)(void);
	/* Give the session up and join again with backoff. `forced`: the M-2
	 * stale-uplink watchdog (LoRaWAN ABP re-activates too); otherwise the
	 * WARNING budget ran out. Returns 0, or -ENOTSUP when this node cannot
	 * rejoin (LoRaWAN ABP, P2P unprovisioned). */
	int (*rejoin)(bool forced);
	/* A network time is wanted (app_radio_time_request()): LoRaWAN queues a
	 * DeviceTimeReq onto the next uplink, P2P a TimeReq onto its next 0x91
	 * control uplink, the TimeAns rides a later report's PENDING. Neither
	 * sends a report of its own. The
	 * backend calls app_radio_time_event() when the time lands. */
	void (*time_request)(void);
	/* Time on air (ms) of an uplink carrying `len` payload bytes now, for the
	 * duty ledger: P2P at its SF with header and tag, LoRaWAN at its DR with
	 * the frame header and the pending MAC answers (T2d). */
	uint32_t (*airtime_ms)(size_t len);
	/* The kinds sent confirmed, BIT(enum app_radio_frame_kind). Alarms are
	 * not the backend's: radio-alarm-ack decides them on both radios. */
	uint8_t confirm_kinds;
	/* enum app_cmd_transport of this radio's command downlinks. */
	uint8_t cmd_transport;
	/* Pause between the frames of one report (covers the receive windows). */
	uint16_t frame_gap_ms;
};

/* Queue an answer or an alarm batch (any thread). Returns 0, -EINVAL, -EMSGSIZE
 * (over APP_RADIO_TX_SLOT_SIZE) or -ENOMEM (queue full). */
int app_radio_tx_queue(enum app_radio_frame_kind kind, enum app_radio_frame_tag tag, uint8_t port,
		       const uint8_t *buf, size_t len);

/* Radio work queue: run the scheduler now (link-up, the radio can send again).
 * A send already waiting for its time keeps it. */
void app_radio_tx_kick(void);

/* An answer is still queued or being sent (the post-command drain). */
bool app_radio_tx_answer_pending(void);

/* An alarm batch frame is still queued or being sent (the post-command drain,
 * #462). */
bool app_radio_tx_alarm_pending(void);

/* Free alarm slots, APP_RADIO_TX_QUEUE_DEPTH when none is queued. A batch that
 * needs more waits held in app_alarm; taking the next alarm frame releases it
 * (app_alarm_flush_held(), #462). */
uint32_t app_radio_tx_alarm_free(void);

/* A confirmed frame went out without its Ack and waits for a retry: nothing
 * else is sent meanwhile, and a post-command reboot waits too (T2c). */
bool app_radio_ack_pending(void);

/* Free answer slots (a page stream leaves two for other answers). */
uint32_t app_radio_tx_answer_free(void);

/* Radio work queue: payload room (bytes) of the next answer, at most
 * `buf_size` and APP_RADIO_TX_SLOT_SIZE. */
size_t app_radio_tx_answer_cap(size_t buf_size);

/* ---- Downlink commands (doc/plan/460 §2.5, F3) ----------------------------
 * One path for both radios. The backend hands over an authenticated command
 * downlink; app_radio runs it through app_cmd_handle(), queues the answer, and
 * streams the remaining pages of an answer that does not fit (one page every
 * 2 s while two answer slots stay free). A deferred action (save, reboot,
 * reset, rejoin) runs through app_cmd_run_action() only once the answer was
 * delivered: re-checked every 8 s while an answer is queued or a confirmed
 * frame is in flight, at most 6 times. */

/* Radio work queue: dispatch a command downlink. */
void app_radio_downlink(const uint8_t *buf, size_t len);

/* ---- Link supervision (doc/plan/460 §2.4, F2; decision #22 §3.4) ---------
 * One machine for both radios. A link check is a report the cadence picked
 * (app_radio_link_check_due()): LoRaWAN rides a LinkCheckReq on it, P2P sends
 * it confirmed; the backend reports its outcome, and any authenticated
 * downlink counts as a success. APP_RADIO_LINK_WARNING_THRESHOLD failures in
 * a row -> WARNING: the session is kept, every report is a link check, and
 * each failed one takes the backend's recovery rung (warning_step). Once the
 * rungs are exhausted, radio-link-check-fail-rejoin failures in WARNING give
 * the session up (rejoin). One success -> HEALTHY. */
#define APP_RADIO_LINK_WARNING_THRESHOLD 3

struct app_radio_link {
	uint32_t reports;       /* reports sent since the link-up (the cadence index) */
	uint32_t fail_streak;   /* failed link checks in a row */
	uint32_t warning_fails; /* failures in WARNING towards radio-link-check-fail-rejoin */
	bool warning;
};

/* Radio work queue: the link came up (LoRaWAN join, P2P paired): supervision
 * and the M-2 stale-uplink clock start afresh. */
void app_radio_link_up(void);

/* Radio work queue: a link check succeeded (`ok`, or any authenticated
 * downlink) or failed (a confirmed frame unacknowledged after all its retries,
 * app_radio's own; LoRaWAN: no LinkCheckAns / no gateway). Counts
 * APP_RADIO_CNT_FAIL; ignored unless the backend is HEALTHY. */
void app_radio_link_result(bool ok);

/* Any thread: the next report is a link check whatever the cadence. */
void app_radio_force_link_check(void);

/* Snapshot of the supervision counters (shell). */
void app_radio_get_link(struct app_radio_link *link);

/* Radio work queue, M-2: a send left (`sent`) or was refused by the duty cycle
 * (`duty_held`); anything else (a radio error) leaves the hold streak. */
void app_radio_note_send(bool sent, bool duty_held);

/* Radio work queue, M-2: an uplink that owns the radio left (a history frame:
 * a replay holds telemetry back, so its frames prove the channel instead). */
void app_radio_note_uplink(void);

/* History replay (ReqHistory, doc/plan/460 §2.5, F3c): stream the records of
 * [from_unix, to_unix] back as HistoryFrame answers carrying `seq`, one frame
 * per run of the radio work queue, 3 s apart (confirmed where the backend
 * confirms APP_RADIO_FRAME_HISTORY). Telemetry waits for the end; alarms and
 * answers do not. Radio work queue (the command path). Returns 0 when the
 * stream started -- or one is already running: it answers the request, which
 * P2P can re-deliver from inside a frame's own Ack wait -- -EAGAIN when the
 * link is down, -EMSGSIZE when records exist but not one fits the budget, or
 * -ENODATA for an empty window. */
int app_radio_history_replay_start(uint32_t from_unix, uint32_t to_unix, uint32_t seq);

/* Start the radio work queue's liveness heartbeat (#182) and, on it, the M-2
 * stale-uplink watchdog. From each backend's init (calibration brings
 * LoRaWAN up without app_radio_init()); a second call does nothing. */
void app_radio_heartbeat_start(void);

/* Radio work queue: a step that blocks it for long (a P2P receive window)
 * feeds the liveness channel itself, as the heartbeat cannot run meanwhile. */
void app_radio_heartbeat_feed(void);

#if defined(CONFIG_ZTEST)
/* Run the common TX path on `be` (tests/radio_common). */
void app_radio_test_set_backend(const struct app_radio_backend *be);
/* Drop every queued frame and the report in progress. */
void app_radio_test_tx_reset(void);
/* Forget the supervision state and the M-2 clock. */
void app_radio_test_link_reset(void);
/* One M-2 stale-uplink check at uptime `now_ms`, as the heartbeat runs it. */
void app_radio_test_stale_tick(int64_t now_ms);
/* Cancel the downlink and announce work items and clear their state. */
void app_radio_test_cmd_reset(void);
/* Forget any exchange and flash writer the gate still counts. */
void app_radio_test_air_reset(void);
/* Cancel the clock_sync work items and forget a pending request. */
void app_radio_test_clock_sync_reset(void);
/* The pure ledger under app_radio_duty_*() (tests/radio_common). */
void app_radio_ledger_init(struct app_radio_duty *d, uint32_t budget_ms);
void app_radio_ledger_charge(struct app_radio_duty *d, int64_t now_ms, uint32_t air_ms);
int64_t app_radio_ledger_wait_ms(struct app_radio_duty *d, int64_t now_ms, uint32_t air_ms);
uint32_t app_radio_ledger_used_ms(const struct app_radio_duty *d, int64_t now_ms);
/* Charge the running ledger with `air_ms` of air that ended at uptime `end_ms`. */
void app_radio_test_duty_charge_at(int64_t end_ms, uint32_t air_ms);
#endif

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
 * then sends -- after a random spread of up to min(interval_report / 2, 30 s),
 * so that nodes rebooted together do not all transmit at once -- the Info
 * (seq 0) followed by the settings-info ConfigDump, each paged for the
 * current budget, and only then the first telemetry (held until every page is
 * queued; 60 s after the spread at the latest). A frame that does not fit
 * yet, or that waits for a running page stream, stays pending and goes out on
 * a later run on the radio work queue: when a page stream ends, when the
 * backend reports room (app_radio_announce_kick()), and every 5 s while
 * something stays pending. */
void app_radio_announce(void);

/* Backend: room may have appeared (LoRaWAN DR rise); run a pending announce
 * now. No-op when nothing is pending. */
void app_radio_announce_kick(void);

/* Something of the announce is still to be sent, or its last pages are still
 * streaming (the held first telemetry waits for the run after the stream). */
bool app_radio_announce_pending(void);

/* Backend: a queued announce frame had to be dropped (the budget fell under
 * it); arm it again for the next run. */
void app_radio_announce_rearm(bool settings);

/* Boot/join order for data (Info -> settings-info -> data): how long a data
 * frame (alarm batch) must still wait. 0 = send now; > 0 = ms until the
 * announce's fallback deadline (the announce normally releases it earlier via
 * app_alarm_flush_held()); -1 = the link is down, the next link-up's announce
 * releases it. */
int32_t app_radio_data_hold_ms(void);

/* Backend work queue only: an Info carrying `seq` (the clock_sync answer),
 * paged like the announce. When not even page 0 can go now, the seq-0
 * announce Info is armed instead. Returns 0 or a negative errno. */
int app_radio_send_info(uint32_t seq);

/* clock_sync with an empty body: re-sync the RTC from the network and answer
 * with an Info carrying `seq` once the time has landed -- the same shape on
 * both radios, no extra uplink: LoRaWAN's DeviceTimeReq rides on the next
 * uplink and the answer comes in its downlink; P2P sends a TimeReq in a 0x91
 * control uplink and the TimeAns rides a later report's PENDING
 * (app_radio_time_request()). A network
 * time that landed less than 60 s ago is fresh: the Info goes at once (PF-2).
 * A newer request before the time lands takes over the seq. Any thread. */
void app_radio_clock_sync(uint32_t seq);

/* Ask the network for the wall-clock time, whatever the radio (any thread):
 * the backend's time_request() on the radio work queue. The time lands via
 * app_radio_time_event(). Also asked by app_radio itself at a link-up while
 * no network time has landed since boot. No-op without a radio. */
void app_radio_time_request(void);

/* Backend, any context: a network time has landed and gone to app_clock (the
 * LoRaWAN DeviceTimeAns, the P2P Ack time tail). Ends the request; a pending
 * clock_sync is answered with its Info, on the radio work queue (F3d). */
void app_radio_time_event(void);

/* A network time was requested and has not landed yet (P2P sets TIME_REQ). */
bool app_radio_time_wanted(void);

/* A clock_sync waits for a network time. */
bool app_radio_clock_sync_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_RADIO_H_ */
