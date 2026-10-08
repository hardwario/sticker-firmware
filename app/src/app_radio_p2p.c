/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_ccm.h"
#include "app_clock.h"
#include "app_cmd.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_log.h"
#include "app_radio_p2p.h"
#include "app_settings.h"
#include "app_version.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_radio_p2p, LOG_LEVEL_INF);

/* Internal helpers are `static` in the firmware but given external linkage
 * under CONFIG_ZTEST so tests/p2p_logic can unit-test the pure decision logic
 * (framing, time-on-air, the duty-cycle governor) directly, without a bench.
 * Same idiom as app_cmd.c's CONFIG_ZTEST test hook. See app_radio_p2p.h. */
#if defined(CONFIG_ZTEST)
#define P2P_TESTABLE
#else
#define P2P_TESTABLE static
#endif

/*
 * Wire (doc/plan/470): TOWER radio frames (app_radio_p2p.h) with two STICKER
 * application envelopes in their payload.
 *
 *   0x81 | port(1) | LoRaWAN fPort payload        data: telemetry 2, alarm 3,
 *                                                 answers 85, commands 86 (down)
 *   0x91 | { cmd(1) | len(1) | value(len) } x n   control (Capabilities, Hello,
 *                                                 LinkCheck, Time, Detach,
 *                                                 RejoinReq, the join)
 *
 * The node's address is the low 32 bits of its DevEUI; the gateway's is the
 * network's net_id, learnt in the JoinAccept. Every data-plane frame is sealed
 * under the session_key the join derived (§6.2), the join frames under
 * join_key. Neither key goes on the air.
 *
 * A confirmed frame is one TOWER confirmed send: up to P2P_REPS byte-identical
 * transmissions (same counter), each followed by a short ACK window armed at
 * TX-done. An ACK with PENDING keeps the receiver on for one gateway Data
 * frame -- a command or a control answer, ACKed back when it is confirmed.
 * An ACK with CTRL carries the answers to a LinkCheckReq / TimeReq itself, as a
 * TLV tail (plan §13.5); their ACK window is longer by TWR_ACK_TAIL_MAX.
 * An app_radio retry of a frame that got no ACK is a new send under a new
 * counter (TOWER semantics, plan §7.3).
 */

/* join_key = AES128-CMAC(app_key, "HIO-TWR-JOIN" || 0x01 || dev_eui(8,
 * MSB-first) || zero-pad to 32 B) seals the JoinRequest and the JoinAccept.
 * session_key = AES128-CMAC(app_key, "HIO-TWR-SES" || 0x01 || dev_nonce(4 BE)
 * || central_nonce(4 BE) || dev_eui(8) || zero-pad to 32 B) seals the data
 * plane, fresh on every join (plan §6.2). The DevEUI is taken MSB-first, as the
 * config stores it and as the hex string reads -- NOT LoRaMac's LSB-first OTAA
 * order. Both ends must agree byte for byte: pinned against the central by
 * tests/ccm/tower_join_kat.json. */
#define P2P_JOIN_KEY_LABEL    "HIO-TWR-JOIN"
#define P2P_SESSION_KEY_LABEL "HIO-TWR-SES"

#define P2P_FCNT_SUBTREE "p2pfc"
#define P2P_FCNT_KEY     "p2pfc/base"
#define P2P_FCNT_RESERVE 256u

/* ---- Join/session persistence (plan §6.3) ---- */

#define P2P_JOIN_SUBTREE    "p2pjoin"
#define P2P_JOIN_DNONCE_KEY "p2pjoin/dnonce"
#define P2P_JOIN_STATE_KEY  "p2pjoin/state"
/* net_id(4 LE) | session_key(16) | rx_delay_s(1) | tx_power_dbm(1, 0 = none).
 * join_settings_set() accepts only records of exactly this length, so the 24 B
 * record of the old P2P wire reads as invalid: the node boots UNPAIRED and
 * joins afresh, which is the flag day plan §10 asks for. */
#define P2P_JOIN_STATE_LEN  (4 + P2P_KEY_LEN + 1 + 1)

/* JoinRequest value byte 0. No product registry exists yet: 1 = STICKER. */
#define P2P_PRODUCT_TYPE_STICKER 1

/* Link supervision (decision #22 §3.4) is app_radio's, one machine for both
 * radios (doc/plan/460 F2). A failed link check is a link-check report
 * (report_flags) with no ACK after its APP_RADIO_ACK_MAX_RETRIES retries;
 * any authenticated downlink is a success. WARNING has no rung on P2P: TX power and SF are fixed
 * per network, with no adaptive data rate or power control (plan §7.4), so
 * the rejoin budget is all there is. A rejoin is a self-healing re-join on the
 * configured SF which, unlike the never-paired boot join (§5.2), skips the
 * 120 s fast phase and starts straight on the common exponential backoff
 * (app_radio_rejoin_backoff_ms()), to keep the duty budget and battery sane
 * over a long outage. */

/* JoinAccept window: the JoinAccept goes out rx_delay (1 s) after the
 * JoinRequest (plan §6.3, the RX1 model of the old wire), opened this many ms
 * early for node-side timing error. lora_recv() has no hardware symbol
 * timeout -- its "timeout" is a software deadline that aborts a reception in
 * flight -- so the window must outlast the whole expected frame: a preamble
 * budget, the frame's time-on-air and a trailing margin for a late central
 * (p2p_rx1_timeout_ms(); F-P2P-2 measured a Northbridge 65..78 ms late). */
#define P2P_RX1_OPEN_MARGIN_MS     25
#define P2P_RX1_WINDOW_SYMBOLS     12
#define P2P_RX1_TRAILING_MARGIN_MS 120
#define P2P_RX1_DELAY_DEFAULT_S    1
/* A JoinAccept's rx_delay outside 1..P2P_RX1_DELAY_MAX_S is refused. */
#define P2P_RX1_DELAY_MAX_S        15

/* TOWER timing on the lora profile (plan §5, P0 M2-M6). The gateway ACKs
 * P2P_TURNAROUND_MS after the uplink's RxDone and sends a pending downlink the
 * same time after the ACK's TxDone; the node ACKs a confirmed downlink the
 * same way. Windows follow p2p_twr_window_ms(). */
#define P2P_TURNAROUND_MS    20
#define P2P_REPS             3   /* transmissions of a confirmed frame */
#define P2P_BACKOFF_MAX_MS   100 /* random pause before a repetition */
#define P2P_WINDOW_MIN_MS    200
#define P2P_WINDOW_SYMBOLS   3
#define P2P_WINDOW_MARGIN_MS 20
/* sx12xx_lora_config() arms the radio's TX timeout at a fixed 4000 ms: a longer
 * frame is cut mid-air and lora_send() then blocks forever (P0). A 100 B frame
 * at SF12 is 3.7 s, so P2P_FRAME_MAX always fits; the check is a guard. */
#define P2P_TOA_MAX_MS       3950U
/* Between an exchange and the next TX, so the gateway is back in RX. */
#define P2P_TX_GAP_MS        200

/* Control envelope (plan §8.2): the uplinks this node sends on its own, one
 * confirmed 0x91 frame carrying every TLV pending (ctrl_work_handler()). */
#define P2P_CTRL_BIT_CAPS        BIT(0)
#define P2P_CTRL_BIT_HELLO       BIT(1)
#define P2P_CTRL_BIT_LINK_CHECK  BIT(2)
#define P2P_CTRL_BIT_TIME        BIT(3)
/* Capabilities + Hello leave this long after a link-up, at random: nodes that
 * all rebooted with the Hub do not answer it at once. */
#define P2P_CTRL_START_JITTER_MS 2000
#define P2P_CTRL_RETRY_MS        30000
#define P2P_CTRL_TRIES           3
/* A control frame waits while app_radio holds a confirmed frame for its retry:
 * taking a counter in between would send the retry under a new one (§7.3). */
#define P2P_CTRL_HOLD_MS         1000
/* p2p_exchange() `attempt` of a frame app_radio never retries. */
#define P2P_NOT_RETRIED          (-1)
/* A LinkCheckReq follows a report the cadence made a link check, after the
 * report's own frames. */
#define P2P_LINK_CHECK_DELAY_MS  5000
/* An older gateway leaves a TimeAns / LinkCheckAns to the central, which
 * queues it for the next PENDING (there is no Poll, plan H3.8); a LinkCheckAns
 * still missing after this many reports is taken as lost. */
#define P2P_ANSWER_REPORTS_MAX   3
/* A TimeAns applies to the TX-done of its TimeReq; one older than this is a
 * stale answer and dropped. */
#define P2P_TIME_ANS_MAX_AGE_MS  (2LL * 60 * 60 * 1000)

/* Capabilities value (plan §8.2). */
#define P2P_CAPS_PROTO_VERSION  1
#define P2P_PROFILE_FSK         BIT(0)
#define P2P_PROFILE_LORA        BIT(1)
#define P2P_POWER_CLASS_BATTERY 1

#define P2P_RX_QUEUE_DEPTH 2

static const struct device *const m_lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));

/* All work runs on the radio work queue, app_radio_work_q() (doc/plan/439 T2a). Its
 * 4096 B stack is sized for this module's deepest path, a command downlink:
 * p2p_deliver() -> app_radio_downlink() -> app_cmd_handle() and nanopb. */
static struct k_work_delayable m_join_work; /* JoinRequest attempt + retry */
static struct k_work_delayable m_ctrl_work; /* the 0x91 uplink (ctrl_work_handler) */

static int64_t m_link_idle_at; /* uptime before which no TX starts */
#if defined(CONFIG_SHELL)
static struct k_work m_rx_work; /* drain received frames (listen) */

/* ats radio ... debug/bench helpers (#118) */
static uint32_t m_debug_drop_acks; /* ats radio ack_drop: remaining forced ACK drops */

struct p2p_compose_result {
	uint8_t frame[P2P_FRAME_MAX];
	size_t frame_len;
	bool more;
	int ret; /* twr_seal()/app_compose_budget() error, 0 on success */
};

static struct p2p_compose_result m_debug_compose_result; /* ats radio compose dry-run */
static struct k_work m_debug_compose_work;
static void debug_compose_work_handler(struct k_work *work); /* defined near EOF */
#endif

static bool m_started;
/* app_radio_p2p_start() refused to run: radio_appkey is all-zero or the DevEUI
 * gives no usable address. Reported as APP_RADIO_STATE_DISABLED. */
static bool m_disabled;
static bool m_listening;
static void (*m_ready_cb)(void);

/* --- Persistent frame counter (nonce uniqueness across reboots) --- */
static uint32_t m_fcnt;          /* next counter value to use */
static uint32_t m_fcnt_reserved; /* persisted high-water; m_fcnt < this is durable */

/* --- Join/session state (plan §6.3) --- */
/* enum p2p_link_state is public (app_radio_p2p.h) so app_radio_p2p_get_info() can report it. */
static enum p2p_link_state m_link_state = P2P_LINK_UNPAIRED;
static uint32_t m_net_id; /* the gateway address; 0 until PAIRED */
static uint8_t m_session_key[P2P_KEY_LEN];
static uint8_t m_rx_delay_s = P2P_RX1_DELAY_DEFAULT_S;
/* D3: TX power the central assigned for this session in the JoinAccept. Falls
 * back to g_app_config.p2p_tx_power when unassigned. */
static bool m_session_tx_power_assigned;
static int8_t m_session_tx_power_dbm;
static uint32_t m_dev_nonce;      /* next JoinRequest counter; persisted, device lifetime */
static int64_t m_join_started_at; /* uptime ms; start of the current boot join window */

/* Spreading factor the radio is tuned to, seeded from the config at init: the
 * SF is fixed for the whole fleet (plan §7.4), and a config change reboots.
 * Initialised to the app_config.yml default rather than left at 0, so the
 * 2^SF arithmetic in p2p_toa_ms()/rx1_preamble_catch_ms() can never run on a
 * zero SF if anything reads it before app_radio_p2p_init() gets past its settings
 * loads to the seeding line. */
static uint8_t m_sf = SF_7;

/* Self-healing re-join state (B3, §7). m_join_slow names the retry POLICY,
 * not what triggered it: the slow policy is exponential backoff with no
 * boot-window cap; the fast one is the 120 s boot window with tight jitter.
 * A self-heal is the only thing that selects the slow policy today. */
static bool m_join_slow;         /* current JOINING episode uses the slow policy */
static uint8_t m_rejoin_attempt; /* backoff step within a slow-policy episode */

/* Gateway replay lane (plan §4): the highest gateway counter accepted this
 * session, shared by its ACKs and Data frames. RAM only, reset on a new
 * session -- a reboot cannot open a replay: the node listens only right after
 * its own uplink, and a downlink is accepted only after an ACK of that uplink's
 * never-used counter, which no recording can hold; that ACK lifts the lane
 * above every older gateway frame before the downlink window opens. */
static uint32_t m_gw_last;

/* The gateway's RSSI of this node's last acknowledged uplink (ACK payload). */
static int8_t m_last_ack_rssi;
static bool m_last_ack_valid;

/* The last LinkCheckAns: the central's view of this node's uplink. */
static struct {
	bool valid;
	int8_t rssi;
	int8_t snr;
	int8_t margin;
	uint8_t gw_count;
} m_lc;

/* --- Control envelope (plan §8.2) --- */
static atomic_t m_ctrl_pending; /* P2P_CTRL_BIT_*: TLVs the next 0x91 uplink carries */
static uint8_t m_ctrl_tries;    /* unacknowledged sends of the current set */
static uint32_t m_session_id;   /* Hello: random per boot */
/* The TimeReq that went out, for the TimeAns (plan §8.2): its counter and the
 * uptime of its TX-done, which the answer's time refers to. */
static uint32_t m_time_req_counter;
static int64_t m_time_req_done_ms;
static bool m_time_req_valid;
static bool m_lc_outstanding;     /* a LinkCheckReq was acknowledged, no answer yet */
static atomic_t m_answer_reports; /* confirmed reports sent for the outstanding answer */

/* The last confirmed app_radio frame (plan §7.3): an app_radio retry of it goes
 * again under the same counter, so a frame the gateway heard but whose ACKs
 * were all lost is a retransmission there (re-ACKed, not delivered again)
 * instead of a second report. Only while no other frame has taken a counter
 * since -- a lower counter would be a replay -- and only for the very same
 * plaintext (one counter never seals two plaintexts). */
static struct {
	bool valid;
	uint32_t counter;
	uint8_t len;
	uint8_t pt[TWR_PAYLOAD_MAX];
} m_retx;

/* One received frame, stamped in the RX callback. */
struct p2p_rx {
	uint32_t cyc; /* k_cycle_get_32() at the RxDone callback */
	int16_t rssi;
	int8_t snr;
	uint8_t len;
	uint8_t buf[P2P_FRAME_MAX];
};

K_MSGQ_DEFINE(m_rxq, sizeof(struct p2p_rx), P2P_RX_QUEUE_DEPTH, 4);
static struct p2p_rx m_rx_scratch; /* p2p_recv_cb()'s copy: the driver's one context */

/* A gateway Data frame received in this exchange (fresh), delivered once the
 * radio is released (p2p_deliver()). Radio work queue only. */
static uint8_t m_dl[TWR_PAYLOAD_MAX];

/* An ACK tail kept for p2p_deliver_xfer(): room for the answers the gateway
 * builds today (TWR_ACK_TAIL_MAX) and a few more; a longer one is cut here and
 * its last entry reported malformed. */
#define P2P_ACK_TAIL_BUF 32

/* What one exchange (p2p_exchange()) did. */
struct p2p_xfer {
	uint32_t counter; /* the frame's counter */
	int64_t done_ms;  /* uptime of the TX-done of its last transmission */
	uint8_t sent;     /* transmissions */
	bool acked;
	bool pending;                       /* the ACK announced a downlink */
	uint8_t ack_tail[P2P_ACK_TAIL_BUF]; /* the ACK's CTRL tail (plan §13.5) */
	uint8_t ack_tail_len;
	int16_t rssi; /* this node's RSSI/SNR of the ACK */
	int8_t snr;
	uint8_t dl_len; /* bytes waiting in m_dl, 0 = none */
	bool ack_owed;  /* a confirmed downlink to ACK */
	uint32_t dl_counter;
	int16_t dl_rssi;
	uint32_t dl_cyc;
};

/* RadioState (#446): every change of the backoff step is pushed to app_radio,
 * which is where readers take it from. */
static void set_rejoin_attempt(uint32_t n)
{
	m_rejoin_attempt = (uint8_t)MIN(n, UINT8_MAX);
	app_radio_set_join_attempts(m_rejoin_attempt);
}

/* The node's TOWER address, low32(DevEUI) (plan §6.1). Read live: the DevEUI
 * is the join identity too, so the two can never disagree. */
static uint32_t node_addr(void)
{
	return sys_get_be32(&g_app_config.radio_deveui[4]);
}

/* After a transmission: the parameters it went out with (the same precedence
 * as build_modem_config()) and, when paired, the session. */
static void publish_link(void)
{
	app_radio_set_params(m_sf, -1,
			     m_session_tx_power_assigned ? m_session_tx_power_dbm
							 : (int8_t)g_app_config.p2p_tx_power);
	if (m_link_state == P2P_LINK_PAIRED) {
		app_radio_set_session(node_addr(), m_fcnt);
	}
}

/* ======================================================================== */
/* Key derivation                                                            */
/* ======================================================================== */

/* The whole transport is rooted in app_key, so an all-zero radio_appkey is not
 * merely "unset" -- it is a PUBLICLY KNOWN root key. Anything derived under it
 * (join_key and every session_key) is forgeable by anyone in radio range: a
 * forged JoinAccept would pair the node into a hostile network and hand the
 * attacker the same session_key the node computes.
 *
 * It is also a genuinely reachable state, not a theoretical one. All-zero is
 * the config default, so any device set to `radio-mode p2p` before its
 * radio_appkey was provisioned lands here -- the ordinary case on a bench or a
 * P2P-only build. It is also what a factory_reset leaves behind (radio_appkey
 * is `persistent: [device_reset]` only and is absent from
 * app_config_factory_reset()'s preserve list).
 *
 * So the radio must not come up at all until the device is provisioned --
 * see app_radio_p2p_start()/app_radio_p2p_rejoin(). Refusing loudly rather than silently
 * idling follows radio_disabled()'s rule in app_radio_lrw.c (#271/#98): a
 * provisioning problem should surface, not masquerade as a radio that is
 * merely off. */
static bool app_key_is_set(void)
{
	for (size_t i = 0; i < sizeof(g_app_config.radio_appkey); i++) {
		if (g_app_config.radio_appkey[i] != 0) {
			return true;
		}
	}
	return false;
}

/* The same rule for radio_deveui: its low 32 bits are the node's address in
 * every frame header (plan §6.1) and the DevEUI is the central's lookup key and
 * a KDF input. An address of 0 or all-ones is reserved on the TOWER wire, and
 * an all-zero DevEUI -- an unprovisioned device -- gives 0, so this also
 * refuses the unprovisioned case. Without it a node would transmit
 * JoinRequests no central can have registered and join forever. */
static bool addr_is_valid(void)
{
	uint32_t addr = node_addr();

	return addr != TWR_ADDR_NONE && addr != TWR_ADDR_BROADCAST;
}

/* CMAC(app_key, label || 0x01 || fields || zero pad to 32 B): both keys have
 * this shape (two full CMAC blocks; app_ccm_cmac() handles multi-block
 * messages, see its RFC4493 vectors in tests/ccm). */
static void derive_key(const char *label, const uint8_t *fields, size_t fields_len,
		       uint8_t out[P2P_KEY_LEN])
{
	uint8_t block[32] = {0};
	size_t label_len = strlen(label);

	memcpy(block, label, label_len);
	block[label_len] = 0x01;
	memcpy(&block[label_len + 1], fields, fields_len);

	(void)app_ccm_cmac(g_app_config.radio_appkey, block, sizeof(block), out);
}

static void derive_join_key(uint8_t out[P2P_KEY_LEN])
{
	derive_key(P2P_JOIN_KEY_LABEL, g_app_config.radio_deveui, sizeof(g_app_config.radio_deveui),
		   out);
}

/* Rotates every join (fresh dev_nonce and central_nonce each time), which is
 * why restarting the frame counter at 0 on every new pairing is safe: CCM's
 * nonce-uniqueness requirement is on the (key, nonce) pair, not the nonce
 * alone. */
static void derive_session_key(uint32_t dev_nonce, uint32_t central_nonce, uint8_t out[P2P_KEY_LEN])
{
	uint8_t fields[4 + 4 + sizeof(g_app_config.radio_deveui)];

	sys_put_be32(dev_nonce, &fields[0]);
	sys_put_be32(central_nonce, &fields[4]);
	memcpy(&fields[8], g_app_config.radio_deveui, sizeof(g_app_config.radio_deveui));
	derive_key(P2P_SESSION_KEY_LABEL, fields, sizeof(fields), out);
}

/* ======================================================================== */
/* Frame counter persistence                                                */
/* ======================================================================== */

static int fcnt_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (!settings_name_steq(name, "base", &next) || next) {
		return -ENOENT;
	}

	uint32_t v;

	if (len != sizeof(v)) {
		return 0;
	}
	if (read_cb(cb_arg, &v, sizeof(v)) != (ssize_t)sizeof(v)) {
		return 0;
	}

	/* The stored value is last boot's reservation high-water: every counter
	 * below it may already be on the air, so resume from it (never below). */
	m_fcnt = v;
	m_fcnt_reserved = v;
	return 0;
}

static struct settings_handler m_fcnt_sh = {
	.name = P2P_FCNT_SUBTREE,
	.h_set = fcnt_settings_set,
};

/* Persist a new reservation high-water. FAIL-CLOSED (B9): the in-RAM watermark
 * is advanced ONLY if the durable write succeeded -- otherwise a reboot would
 * resume from the older persisted value and could hand out a counter this run
 * already used, and a repeated (key, nonce) pair is a full CCM break. Returns
 * 0 or the settings errno. */
static int fcnt_reserve(uint32_t high_water)
{
	int ret = settings_save_one(P2P_FCNT_KEY, &high_water, sizeof(high_water));

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save_one(p2pfc)", ret);
		return ret;
	}
	m_fcnt_reserved = high_water;
	return 0;
}

/* Hand out the next frame counter into `*counter_out`. Guarantees the value is
 * within the durably-reserved window before returning it (B9): if the window
 * must be extended and that durable write fails, refuses (returns the errno)
 * rather than reuse a counter across a reboot. Saturates at UINT32_MAX and
 * refuses rather than wrapping (a wrap repeats every (key, nonce) -- a rekey
 * via re-join is the intended recovery long before this is reachable). */
static int fcnt_next(uint32_t *counter_out)
{
	if (m_fcnt == UINT32_MAX) {
		LOG_ERR("P2P frame counter exhausted; refusing TX (re-join to rekey)");
		return -EOVERFLOW;
	}

	if (m_fcnt >= m_fcnt_reserved) {
		uint32_t target = (m_fcnt > UINT32_MAX - P2P_FCNT_RESERVE)
					  ? UINT32_MAX
					  : m_fcnt + P2P_FCNT_RESERVE;
		int ret = fcnt_reserve(target);

		if (ret) {
			return ret; /* fail closed -- do not use an unreserved counter */
		}
	}

	*counter_out = m_fcnt++;
	return 0;
}

/* The counter of a frame about to be sealed: TOWER reserves 0, which a new
 * session's first fcnt_next() returns. Spent here, before the TX (plan §4, U1). */
static int twr_counter_next(uint32_t *counter_out)
{
	int ret = fcnt_next(counter_out);

	if (ret == 0 && *counter_out == 0) {
		ret = fcnt_next(counter_out);
	}
	return ret;
}

#if defined(CONFIG_ZTEST)
/* Test hooks for the frame-counter fail-closed/saturation logic (B9). With the
 * CONFIG_SETTINGS_NONE backend in tests/p2p_logic, settings_save_one() fails,
 * so any path that must extend the reservation exercises the fail-closed
 * branch. */
void p2p_test_set_fcnt(uint32_t next, uint32_t reserved)
{
	m_fcnt = next;
	m_fcnt_reserved = reserved;
}

uint32_t p2p_test_get_fcnt(void)
{
	return m_fcnt;
}

int p2p_test_fcnt_next(uint32_t *counter_out)
{
	return fcnt_next(counter_out);
}
#endif /* defined(CONFIG_ZTEST) */

/* ======================================================================== */
/* Join/session persistence (plan §6.3)                                     */
/* ======================================================================== */

static int join_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "dnonce", &next) && !next) {
		uint32_t v;

		if (len == sizeof(v) && read_cb(cb_arg, &v, sizeof(v)) == (ssize_t)sizeof(v)) {
			m_dev_nonce = v;
		}
		return 0;
	}

	if (settings_name_steq(name, "state", &next) && !next) {
		uint8_t buf[P2P_JOIN_STATE_LEN];

		if (len == sizeof(buf) &&
		    read_cb(cb_arg, buf, sizeof(buf)) == (ssize_t)sizeof(buf)) {
			uint32_t net_id = sys_get_le32(&buf[0]);
			uint8_t tx_power = buf[5 + P2P_KEY_LEN];

			if (net_id == TWR_ADDR_NONE || net_id == TWR_ADDR_BROADCAST) {
				return 0; /* corrupt: boot UNPAIRED and join */
			}
			m_net_id = net_id;
			memcpy(m_session_key, &buf[4], P2P_KEY_LEN);
			m_rx_delay_s = CLAMP(buf[4 + P2P_KEY_LEN], 1, P2P_RX1_DELAY_MAX_S);
			/* 0 = the session carried no assignment. Range-check on
			 * the way back in too, so a corrupt record cannot push
			 * the PA outside its configured envelope. */
			m_session_tx_power_assigned = tx_power >= P2P_TX_POWER_MIN_DBM &&
						      tx_power <= P2P_TX_POWER_MAX_DBM;
			m_session_tx_power_dbm = m_session_tx_power_assigned ? (int8_t)tx_power : 0;
			m_link_state = P2P_LINK_PAIRED;
		}
		return 0;
	}

	return -ENOENT;
}

static struct settings_handler m_join_sh = {
	.name = P2P_JOIN_SUBTREE,
	.h_set = join_settings_set,
};

/* dev_nonce increments only on a JoinRequest attempt (rare -- not a per-frame
 * event like the data-plane frame counter), so a plain synchronous save is
 * cheap enough; no need for p2pfc's reservation-window trick. Never resets
 * across pairings: it is the JoinRequest's frame counter under join_key, which
 * is fixed for the device's life, so a repeated dev_nonce would repeat a
 * (key, nonce) pair -- and it is the central's replay handle. */
static int dnonce_persist(uint32_t v)
{
	int ret = settings_save_one(P2P_JOIN_DNONCE_KEY, &v, sizeof(v));

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save_one(p2pjoin/dnonce)", ret);
		return ret;
	}
	m_dev_nonce = v;
	return 0;
}

/* The per-session state that must not outlive a session: the replay lane (the
 * next gateway counts from 1 under a new key), the link numbers and the
 * outstanding control requests. */
static void session_reset(void)
{
	m_gw_last = 0;
	m_last_ack_valid = false;
	m_lc.valid = false;
	m_lc_outstanding = false;
	m_time_req_valid = false;
	atomic_clear(&m_answer_reports);
	atomic_clear(&m_ctrl_pending);
	m_ctrl_tries = 0;
	m_retx.valid = false;
}

/* Persist a successful JoinAccept's pairing state and switch the module to
 * PAIRED. Resets the data-plane frame counter to 0 -- safe because
 * session_key is fresh (see derive_session_key()) and keeps the on-air
 * counter values small. */
static int pairing_persist(const struct p2p_join_accept *ja, const uint8_t session_key[P2P_KEY_LEN])
{
	uint8_t buf[P2P_JOIN_STATE_LEN];

	sys_put_le32(ja->net_id, &buf[0]);
	memcpy(&buf[4], session_key, P2P_KEY_LEN);
	buf[4 + P2P_KEY_LEN] = ja->rx_delay_s;
	buf[5 + P2P_KEY_LEN] = ja->tx_power_assigned ? (uint8_t)ja->tx_power_dbm : 0;

	int ret = settings_save_one(P2P_JOIN_STATE_KEY, buf, sizeof(buf));

	if (ret) {
		/* The join is retried: a session only in RAM would leave the node
		 * JOINING with nothing scheduled (review of #400). */
		LOG_ERR_CALL_FAILED_INT("settings_save_one(p2pjoin/state)", ret);
		return ret;
	}

	m_net_id = ja->net_id;
	memcpy(m_session_key, session_key, P2P_KEY_LEN);
	m_rx_delay_s = ja->rx_delay_s;
	m_session_tx_power_assigned = ja->tx_power_assigned;
	m_session_tx_power_dbm = ja->tx_power_dbm;
	m_link_state = P2P_LINK_PAIRED;
	session_reset();

	/* Fresh pairing: counter restarts at 0 under the just-rotated session_key,
	 * so reuse vs. the old session is impossible. Best-effort reserve here --
	 * if it fails, the old (higher) persisted watermark conservatively still
	 * covers these low counters, and fcnt_next() re-reserves fail-closed once
	 * m_fcnt catches up to it. */
	m_fcnt = 0;
	(void)fcnt_reserve(P2P_FCNT_RESERVE);
	return 0;
}

/* Tear the pairing down: drop the persisted session and return the module to
 * UNPAIRED, live, without a reboot. The inverse of pairing_persist().
 *
 * Shared by the `ats radio unjoin` shell path (which reboots afterwards
 * anyway) and the Detach downlink (plan §6.5), which must take effect
 * immediately -- the central has already dropped the session, so every further
 * uplink would be shouting at a network that is no longer listening. Dropping
 * to UNPAIRED (and clearing m_started) is what stops the report cadence:
 * app_report.c::run_report gates the uplink on app_radio_is_ready() ->
 * app_radio_p2p_is_ready() (paired and started), so the cadence timer keeps
 * running harmlessly while nothing is transmitted.
 *
 * The answers and alarms app_radio still queues are plaintext and wait for the
 * next session, as over LoRaWAN across a rejoin.
 *
 * NEVER touches m_dev_nonce (see dnonce_persist()) or m_fcnt: the nonce must
 * keep advancing across pairings, and the counter is reset by the NEXT
 * pairing_persist() under a freshly derived key.
 *
 * The RAM state is cleared even if the NVS delete fails: honouring the
 * Detach matters more than the record, and a stale record only means the
 * next boot comes up PAIRED into a dead session, which the self-heal path
 * (§7) already recovers from. Returns the settings_delete() result. */
static int pairing_clear(void)
{
	int ret = settings_delete(P2P_JOIN_STATE_KEY);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_delete(p2pjoin/state)", ret);
	}

	m_link_state = P2P_LINK_UNPAIRED;
	m_started = false;
	m_session_tx_power_assigned = false;
	m_session_tx_power_dbm = 0;
	session_reset();

	return ret;
}

/* ======================================================================== */
/* Radio configuration                                                      */
/* ======================================================================== */

/* Fixed in phase 1 (kept out of config to fit the dual-stack flash budget):
 * the common P2P defaults of 125 kHz bandwidth and 4/5 coding rate. The
 * receiver must match these. Frequency / SF / TX power stay configurable. */
#define P2P_BANDWIDTH    BW_125_KHZ
#define P2P_BANDWIDTH_HZ 125000u
#define P2P_CODING_RATE  CR_4_5 /* app_radio_lora_toa_ms() assumes 4/5 */

static int sf_from_cfg(void)
{
	return CLAMP(g_app_config.p2p_spreading_factor, SF_6, SF_12);
}

static void build_modem_config(struct lora_modem_config *c, bool tx)
{
	memset(c, 0, sizeof(*c));
	c->frequency = g_app_config.p2p_frequency;
	c->bandwidth = P2P_BANDWIDTH;
	c->datarate = (enum lora_datarate)m_sf;
	c->coding_rate = P2P_CODING_RATE;
	c->preamble_len = 8;
	/* An assigned session power overrides the local config: the central owns
	 * the link budget across the whole network, the node only its own
	 * default (D3). */
	c->tx_power = m_session_tx_power_assigned ? m_session_tx_power_dbm
						  : (int8_t)g_app_config.p2p_tx_power;
	c->tx = tx;
	c->iq_inverted = false;
	c->public_network = false;
}

static int radio_configure(bool tx)
{
	struct lora_modem_config config;

	build_modem_config(&config, tx);
	int ret = lora_config(m_lora_dev, &config);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lora_config", ret);
	}
	return ret;
}

/* LoRa time-on-air in ms for spreading factor `sf` on the fixed P2P PHY: BW
 * 125 kHz, CR 4/5, preamble 8 symbols, explicit header (doc/p2p.md §3.3). The
 * common formula of app_radio, so the duty ledger charges both radios alike.
 * Pure -- exposed to tests/p2p_logic. */
P2P_TESTABLE uint32_t p2p_toa_ms(int sf, uint8_t payload_len)
{
	return app_radio_lora_toa_ms((uint32_t)sf, P2P_BANDWIDTH_HZ, payload_len);
}

/* Time-on-air at the SF the radio is currently tuned to. */
static uint32_t frame_toa_ms(size_t frame_len)
{
	return p2p_toa_ms(m_sf, (uint8_t)MIN(frame_len, (size_t)UINT8_MAX));
}

/* Preamble-catch / open-timing-slop budget in ms, P2P_RX1_WINDOW_SYMBOLS
 * symbols at `sf`/BW -- only ONE component of the JoinAccept window (see
 * p2p_rx1_timeout_ms()). Pure -- exposed to tests/p2p_logic. */
P2P_TESTABLE uint32_t rx1_preamble_catch_ms(int sf)
{
	uint64_t tsym_us = ((uint64_t)(1u << sf) * 1000000ULL) / P2P_BANDWIDTH_HZ;

	return (uint32_t)((tsym_us * P2P_RX1_WINDOW_SYMBOLS + 500) / 1000);
}

/* Full lora_recv() timeout of the JoinAccept window at `sf` for a frame of
 * `expected_frame_len` bytes: preamble-catch budget + that frame's whole
 * time-on-air + a trailing margin. Pure -- exposed to tests/p2p_logic. */
P2P_TESTABLE uint32_t p2p_rx1_timeout_ms(int sf, uint8_t expected_frame_len)
{
	return rx1_preamble_catch_ms(sf) + p2p_toa_ms(sf, expected_frame_len) +
	       P2P_RX1_TRAILING_MARGIN_MS;
}

/* TOWER receive window (plan §5, P0 finding "ACK window") in ms, counted from
 * the moment the node's receiver runs: the peer's turnaround, the awaited
 * frame's time-on-air and three symbols of RxDone latency on both sides (P0
 * measured ~2 symbols + 1.5 ms; a fixed-ms margin missed every ACK at SF12),
 * plus a fixed margin -- never under P2P_WINDOW_MIN_MS. The ACK window uses the
 * 28 B ACK, the downlink window after PENDING the largest frame, P2P_FRAME_MAX.
 * Pure -- exposed to tests/p2p_logic. */
P2P_TESTABLE uint32_t p2p_twr_window_ms(int sf, uint8_t frame_len)
{
	uint32_t sym_ms = DIV_ROUND_UP(BIT(sf) * 8U, 1000U); /* BW125: 2^SF / 125 kHz */

	return MAX((uint32_t)P2P_WINDOW_MIN_MS, P2P_TURNAROUND_MS + p2p_toa_ms(sf, frame_len) +
							P2P_WINDOW_SYMBOLS * sym_ms +
							P2P_WINDOW_MARGIN_MS);
}

/* Open a bounded RX window `rx1_delay_s - open_margin_ms` after `tx_end_ms`
 * (uptime ms), sleeping until it opens then blocking on lora_recv() for
 * p2p_rx1_timeout_ms(expected_frame_len) -- the JoinAccept's. Runs on the
 * radio work queue, which it blocks for ~rx1_delay_s + the frame's ToA; an
 * explicit watchdog feed covers this since app_radio's periodic heartbeat
 * cannot run until it returns. Restores TX radio config before returning
 * either way. Returns the received length (>=0) or a negative errno (notably a
 * timeout if nothing arrived within the window). */
static int p2p_rx_window(int64_t tx_end_ms, uint8_t rx1_delay_s, uint8_t expected_frame_len,
			 uint8_t *buf, size_t buf_size, int16_t *rssi, int8_t *snr)
{
	int64_t open_at = tx_end_ms + (int64_t)rx1_delay_s * 1000 - P2P_RX1_OPEN_MARGIN_MS;
	int64_t sleep_ms = open_at - k_uptime_get();

	app_radio_heartbeat_feed();

	if (sleep_ms > 0) {
		k_sleep(K_MSEC(sleep_ms));
	}

	int ret = radio_configure(false);

	if (ret) {
		app_radio_air_end();
		return ret;
	}

	ret = lora_recv(m_lora_dev, buf, (uint8_t)MIN(buf_size, 255),
			K_MSEC(p2p_rx1_timeout_ms(m_sf, expected_frame_len)), rssi, snr);

	(void)radio_configure(true);
	app_radio_air_end(); /* the exchange send_join_request() began */

	return ret;
}

/* ======================================================================== */
/* Join retry policy                                                        */
/* ======================================================================== */

/* How long to wait before the next JoinRequest, or < 0 for "the boot window is
 * over" -- which hands the episode to the slow policy rather than ending it
 * (join_window_expired). Pure -- exposed to tests/p2p_logic. The caller adds
 * jitter.
 *
 * The slow policy (§7, selected by a self-heal) has no window: a paired device
 * recovers for its whole life, so it always gets its exponential backoff -- or
 * the duty wait, whichever is longer. Taking only the backoff meant a round the
 * duty ledger had refused (the JoinRequest never reached the air) woke into the
 * same refusal having spent a backoff step on nothing.
 *
 * The fast policy (a boot join, §5.2) has a 120 s deadline, and that deadline
 * has to bound the wait as well as the retrying: `duty_wait_ms` can run to a
 * full hour (APP_RADIO_DUTY_WINDOW_MS), which once kept a node JOINING for
 * minutes past its window (bench 2026-09-10). Capping at the remaining window
 * makes the next wake-up the one that reports it -- the caller's jitter lands
 * it just past the edge, which is exactly when the hand-over to the slow
 * policy is due. */
P2P_TESTABLE int64_t p2p_join_retry_delay_ms(bool slow, int64_t elapsed_ms, int64_t duty_wait_ms,
					     uint32_t backoff_ms, uint32_t jitter_ms)
{
	if (slow) {
		/* Whichever is longer. A round the duty ledger refused never
		 * reached the air, so waiting only the backoff wakes it into
		 * the same refusal, one backoff step poorer. Both terms are
		 * bounded by the sliding hour, so this is too. */
		return MAX((int64_t)backoff_ms, duty_wait_ms > 0 ? duty_wait_ms : 0);
	}

	int64_t remaining = (int64_t)P2P_JOIN_BOOT_WINDOW_MS - elapsed_ms;

	if (remaining <= 0) {
		return -1;
	}

	int64_t wait = (duty_wait_ms > 0) ? duty_wait_ms : 0;

	if (wait + (int64_t)jitter_ms >= remaining) {
		return remaining;
	}
	return wait;
}

/* Duty-cycle wait (ms) of the common ledger before a `wire_len`-byte frame can
 * be sent at the current SF -- 0 if it can go now. */
static int64_t duty_wait_ms_for(size_t wire_len)
{
	return app_radio_duty_wait_ms(frame_toa_ms(wire_len));
}

/* Start a JOINING episode and schedule the first JoinRequest. `slow` selects
 * which retry policy join_work_handler() opens with: the fast one runs with
 * tight jitter until the 120 s boot window closes and then hands over to the
 * slow policy (§5.2); the slow one starts there directly, on exponential
 * backoff (§7). Neither gives up. Shared by app_radio_p2p_start(), app_radio_p2p_rejoin()
 * (shell), and the self-heal trigger below. */
static void start_join_episode(bool slow)
{
	m_join_slow = slow;
	set_rejoin_attempt(0);
	m_link_state = P2P_LINK_JOINING;
	m_join_started_at = k_uptime_get();
	/* reschedule, not schedule: a slow-backoff retry may be pending for up to
	 * an hour, and k_work_schedule_for_queue() is a no-op while the item is
	 * already scheduled -- the state rewritten just above would then sit
	 * unread until that timer fired. The operator's join must pre-empt it. */
	k_work_reschedule_for_queue(app_radio_work_q(), &m_join_work, K_NO_WAIT);
}

/* A link check succeeded: an ACK or any other authenticated downlink. */
static void note_uplink_acked(void)
{
	app_radio_link_result(true);
}

/* struct app_radio_backend.warning_step: no rung on P2P. TX power and SF stay
 * as the network set them (plan §7.4); WARNING goes straight to the rejoin
 * budget. */
static bool p2p_warning_step(void)
{
	return false;
}

/* struct app_radio_backend.rejoin: the self-healing re-join (§7) on the slow
 * policy. Refused while unprovisioned: no JoinRequest can succeed under an
 * all-zero app_key (§4) or without a valid address. */
/* Only the lora PHY profile exists so far; fsk (TOWER-native GFSK) is plan
 * #470 P5. Every path that would transmit refuses rather than send LoRa to an
 * fsk network. */
static bool phy_is_supported(void)
{
	return g_app_config.p2p_modulation == APP_CONFIG_P2P_MODULATION_LORA;
}

static int p2p_tx_rejoin(bool forced)
{
	ARG_UNUSED(forced);

	if (!app_key_is_set()) {
		LOG_ERR("P2P self-heal refused: radio_appkey is all-zero (unprovisioned)");
		return -ENOTSUP;
	}
	if (!addr_is_valid()) {
		LOG_ERR("P2P self-heal refused: radio_deveui gives no valid address");
		return -ENOTSUP;
	}
	if (!phy_is_supported()) {
		LOG_ERR("P2P self-heal refused: p2p-modulation fsk is not supported yet");
		return -ENOTSUP;
	}
	LOG_WRN("P2P: self-healing re-join (§7)");
	start_join_episode(true);
	return 0;
}

/* ======================================================================== */
/* TOWER frame codec                                                        */
/* ======================================================================== */

/* doc/plan/470 §4; checked byte for byte against tests/ccm/tower_frame_kat.json
 * (upstream tower-radio-core / tower-net-core) in tests/p2p_logic. */
P2P_TESTABLE void twr_hdr_put(uint8_t out[TWR_HDR_LEN], const struct twr_hdr *h)
{
	out[0] = (uint8_t)((TWR_VERSION << 5) | (h->type & 0x1F));
	out[1] = h->flags;
	sys_put_le32(h->src, &out[2]);
	sys_put_le32(h->dest, &out[6]);
	sys_put_le32(h->counter, &out[10]);
}

P2P_TESTABLE int twr_hdr_get(const uint8_t *frame, size_t frame_len, struct twr_hdr *h)
{
	if (frame_len < TWR_HDR_LEN + TWR_TAG_LEN || frame_len > P2P_FRAME_MAX) {
		return -EMSGSIZE;
	}
	if ((frame[0] >> 5) != TWR_VERSION) {
		return -EPROTO;
	}
	h->type = frame[0] & 0x1F;
	h->flags = frame[1];
	h->src = sys_get_le32(&frame[2]);
	h->dest = sys_get_le32(&frame[6]);
	h->counter = sys_get_le32(&frame[10]);
	return 0;
}

/* src(4 LE) | counter(4 LE) | bulk_idx(3 LE) | 0x0000. No direction byte: the
 * two directions differ by src. No bulk frames yet, so bulk_idx is 0. */
P2P_TESTABLE void twr_nonce(uint8_t nonce[TWR_NONCE_LEN], uint32_t src, uint32_t counter)
{
	memset(nonce, 0, TWR_NONCE_LEN);
	sys_put_le32(src, &nonce[0]);
	sys_put_le32(counter, &nonce[4]);
}

P2P_TESTABLE int twr_seal(const uint8_t key[16], const struct twr_hdr *h, const uint8_t *pt,
			  size_t pt_len, uint8_t *frame, size_t frame_size, size_t *frame_len)
{
	size_t len = TWR_HDR_LEN + pt_len + TWR_TAG_LEN;

	if (len > frame_size || len > P2P_FRAME_MAX) {
		return -EMSGSIZE;
	}

	uint8_t nonce[TWR_NONCE_LEN];

	twr_hdr_put(frame, h);
	twr_nonce(nonce, h->src, h->counter);

	int ret = app_ccm_encrypt_and_tag(key, nonce, sizeof(nonce), frame, TWR_HDR_LEN, pt, pt_len,
					  &frame[TWR_HDR_LEN], &frame[TWR_HDR_LEN + pt_len],
					  TWR_TAG_LEN);

	if (ret) {
		return ret;
	}
	*frame_len = len;
	return 0;
}

P2P_TESTABLE int twr_open(const uint8_t key[16], const uint8_t *frame, size_t frame_len,
			  struct twr_hdr *h, uint8_t *pt, size_t pt_size, size_t *pt_len)
{
	int ret = twr_hdr_get(frame, frame_len, h);

	if (ret) {
		return ret;
	}

	size_t ct_len = frame_len - TWR_HDR_LEN - TWR_TAG_LEN;

	if (ct_len > pt_size) {
		return -EMSGSIZE;
	}

	uint8_t nonce[TWR_NONCE_LEN];

	twr_nonce(nonce, h->src, h->counter);
	ret = app_ccm_auth_decrypt(key, nonce, sizeof(nonce), frame, TWR_HDR_LEN,
				   &frame[TWR_HDR_LEN], ct_len, &frame[frame_len - TWR_TAG_LEN],
				   TWR_TAG_LEN, pt);
	if (ret) {
		return ret;
	}
	*pt_len = ct_len;
	return 0;
}

/* acked(4 LE) | rssi(i8) | flags(PENDING bit 0, CTRL bit 1) [| TLV...]. Any
 * payload of at least 4 B is an ACK: the rule that keeps appended fields
 * interop-safe. With CTRL the rest is a 0x91 TLV list (plan §13.5), pointed to
 * in `pt`; without it trailing bytes are ignored. */
P2P_TESTABLE int twr_parse_ack(const uint8_t *pt, size_t pt_len, struct twr_ack *ack)
{
	if (pt_len < 4) {
		return -EMSGSIZE;
	}
	ack->acked = sys_get_le32(pt);
	ack->rssi = pt_len > 4 ? (int8_t)pt[4] : 0;
	ack->pending = pt_len > 5 && (pt[5] & TWR_ACK_PENDING);
	ack->tail = NULL;
	ack->tail_len = 0;
	if (pt_len > TWR_ACK_PAYLOAD_LEN && (pt[5] & TWR_ACK_CTRL)) {
		ack->tail = &pt[TWR_ACK_PAYLOAD_LEN];
		ack->tail_len = pt_len - TWR_ACK_PAYLOAD_LEN;
	}
	return 0;
}

/* The replay rule on an authenticated gateway frame (plan §4): above the lane
 * is fresh, equal a retransmission (re-ACK, never re-deliver), below -- and the
 * reserved counter 0 -- a replay. Pure -- exposed to tests/p2p_logic. */
P2P_TESTABLE int p2p_replay_check(uint32_t last, uint32_t counter)
{
	if (counter == 0 || counter < last) {
		return -EALREADY;
	}
	return counter == last ? P2P_RX_REPEAT : P2P_RX_FRESH;
}

/* ======================================================================== */
/* Control envelope codec (plan §8.2)                                       */
/* ======================================================================== */

/* The next entry of a TLV list (the 0x91 payload after its envelope byte) at
 * `*off`: returns 1 with `*tlv` filled and `*off` advanced, 0 at the end of the
 * list, -EBADMSG when an entry runs past it. An unknown cmd is the caller's to
 * skip; its length carries the parser over it. Pure -- exposed to
 * tests/p2p_logic. */
P2P_TESTABLE int p2p_tlv_next(const uint8_t *buf, size_t len, size_t *off, struct p2p_tlv *tlv)
{
	if (*off >= len) {
		return 0;
	}
	if (len - *off < 2 || len - *off - 2 < buf[*off + 1]) {
		return -EBADMSG;
	}
	tlv->cmd = buf[*off];
	tlv->len = buf[*off + 1];
	tlv->val = &buf[*off + 2];
	*off += 2 + tlv->len;
	return 1;
}

/* JoinAccept value (plan §6.3): net_id(4 LE) | central_nonce(4 LE) |
 * rx_delay(1) | tx_power(1) | reserved(3). A longer value is accepted and the
 * tail ignored (the TLV length carries appended fields). `hdr_src` is the
 * frame's src, which must be the net_id it assigns. Refused: a reserved
 * net_id, an rx_delay outside 1..P2P_RX1_DELAY_MAX_S. A tx_power outside
 * P2P_TX_POWER_MIN_DBM..MAX is warned about and ignored -- the JoinAccept is
 * otherwise valid and authenticated, and refusing to pair over a byte this
 * release cannot honour would strand the node. Pure -- exposed to
 * tests/p2p_logic. */
P2P_TESTABLE int p2p_parse_join_accept(const uint8_t *val, size_t len, uint32_t hdr_src,
				       struct p2p_join_accept *out)
{
	if (len < P2P_JOIN_ACCEPT_VALUE_LEN) {
		return -EBADMSG;
	}

	uint32_t net_id = sys_get_le32(&val[0]);
	uint8_t rx_delay = val[8];
	uint8_t tx_power = val[9];

	if (net_id != hdr_src || net_id == TWR_ADDR_NONE || net_id == TWR_ADDR_BROADCAST) {
		LOG_WRN("JoinAccept: net_id %08x (frame src %08x) refused", net_id, hdr_src);
		return -EBADMSG;
	}
	if (rx_delay < 1 || rx_delay > P2P_RX1_DELAY_MAX_S) {
		LOG_WRN("JoinAccept: rx_delay %u s outside 1..%u, refused", rx_delay,
			P2P_RX1_DELAY_MAX_S);
		return -EBADMSG;
	}

	out->net_id = net_id;
	out->central_nonce = sys_get_le32(&val[4]);
	out->rx_delay_s = rx_delay;
	out->tx_power_assigned = false;
	out->tx_power_dbm = 0;

	if (tx_power >= P2P_TX_POWER_MIN_DBM && tx_power <= P2P_TX_POWER_MAX_DBM) {
		out->tx_power_assigned = true;
		out->tx_power_dbm = (int8_t)tx_power;
	} else if (tx_power != 0) {
		LOG_WRN("JoinAccept assigns %u dBm TX power: outside %d..%d, ignoring", tx_power,
			P2P_TX_POWER_MIN_DBM, P2P_TX_POWER_MAX_DBM);
	}
	return 0;
}

/* The Unix time now for a TimeAns (plan §8.2) of `unix_s` + `frac`/256 s at
 * the TX-done of its TimeReq, `elapsed_ms` ago; rounded to the second. Pure --
 * exposed to tests/p2p_logic. */
P2P_TESTABLE uint32_t p2p_time_at(uint32_t unix_s, uint8_t frac, int64_t elapsed_ms)
{
	int64_t ms = (int64_t)frac * 1000 / 256 + MAX(elapsed_ms, 0);

	return unix_s + (uint32_t)((ms + 500) / 1000);
}

/* ======================================================================== */
/* Exchange: one uplink, its ACK and a pending downlink                     */
/* ======================================================================== */

/* lora_send() failed. The driver reports a TX timeout as -EAGAIN and a busy
 * modem as -EBUSY, which app_radio would read as "duty cycle refused" and
 * "listen mode": a dead or wedged modem was then retried every few ms (review
 * of #400). A radio fault is -EIO here. The PA may have been keyed, so its air
 * is charged to the ledger (over-counting only errs towards compliance). */
static int tx_send_failed(size_t wire_len)
{
	app_radio_count(APP_RADIO_CNT_TX_ERR);
	app_radio_duty_charge(frame_toa_ms(wire_len));
	return -EIO;
}

/* The radio's RxDone callback, the driver's own context: copy, stamp, queue.
 * The exchange drains the queue; in listen mode m_rx_work does. A frame over
 * the MTU is none of ours. */
static void p2p_recv_cb(const struct device *dev, uint8_t *data, uint16_t size, int16_t rssi,
			int8_t snr, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	struct p2p_rx *rx = &m_rx_scratch;

	if (size > sizeof(rx->buf)) {
		return;
	}
	rx->cyc = k_cycle_get_32();
	rx->rssi = rssi;
	rx->snr = snr;
	rx->len = (uint8_t)size;
	memcpy(rx->buf, data, size);
	if (k_msgq_put(&m_rxq, rx, K_NO_WAIT) != 0) {
		return;
	}
#if defined(CONFIG_SHELL)
	if (m_listening) {
		k_work_submit_to_queue(app_radio_work_q(), &m_rx_work);
	}
#endif
}

#if defined(CONFIG_LORA_SEND_RECV_ASYNC)
/* The fork driver's send-then-receive (plan §13.1 "Node TX->RX without
 * sleep"): the radio goes from TX-done straight to reception, TCXO on, and
 * stamps both moments -- the receiver runs ~0.7 ms after TX-done instead of
 * ~22 ms. Returns -ENOSYS when the driver lacks it. */
static bool m_no_fast;

static int p2p_send_recv(uint8_t *frame, size_t flen, uint32_t air, uint32_t *done_cyc,
			 uint32_t *arm_cyc)
{
	static struct k_poll_signal sig;
	struct k_poll_event evt =
		K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &sig);
	struct lora_turnaround tm = {0};
	unsigned int signaled;
	int result = 0;

	/* RX first: the driver keeps it for the turnaround; TX last sets power. */
	int ret = radio_configure(false);

	if (ret == 0) {
		ret = radio_configure(true);
	}
	if (ret) {
		return ret;
	}

	k_poll_signal_init(&sig);
	ret = lora_send_recv_async(m_lora_dev, frame, (uint32_t)flen, p2p_recv_cb, NULL, &sig, &tm);
	if (ret == -ENOSYS) {
		return ret;
	}
	if (ret == 0) {
		ret = k_poll(&evt, 1, K_MSEC(2 * air + 50));
		k_poll_signal_check(&sig, &signaled, &result);
		ret = ret ? ret : result;
	}
	if (ret) {
		(void)lora_recv_async(m_lora_dev, NULL, NULL);
		LOG_ERR_CALL_FAILED_INT("lora_send_recv_async", ret);
		return tx_send_failed(flen);
	}
	app_radio_duty_charge(air);
	*done_cyc = tm.tx_done_cyc;
	*arm_cyc = tm.rx_armed_cyc;
	return 0;
}
#endif /* defined(CONFIG_LORA_SEND_RECV_ASYNC) */

/* One transmission of `frame`; with `rx` the radio is left receiving into
 * p2p_recv_cb(). `*done_cyc` is the TX-done, `*arm_cyc` when reception ran.
 * The caller holds app_radio_air_begin(). */
static int p2p_tx_once(uint8_t *frame, size_t flen, uint32_t air, bool rx, uint32_t *done_cyc,
		       uint32_t *arm_cyc)
{
	int ret;

#if defined(CONFIG_LORA_SEND_RECV_ASYNC)
	if (rx && !m_no_fast) {
		ret = p2p_send_recv(frame, flen, air, done_cyc, arm_cyc);
		if (ret != -ENOSYS) {
			return ret;
		}
		m_no_fast = true; /* the two-call path from now on */
	}
#endif
	ret = radio_configure(true);
	if (ret) {
		return ret;
	}
	ret = lora_send(m_lora_dev, frame, (uint32_t)flen);
	*done_cyc = k_cycle_get_32();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lora_send", ret);
		return tx_send_failed(flen);
	}
	app_radio_duty_charge(air);
	if (!rx) {
		return 0;
	}
	ret = radio_configure(false);
	if (ret == 0) {
		ret = lora_recv_async(m_lora_dev, p2p_recv_cb, NULL);
	}
	*arm_cyc = k_cycle_get_32();
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lora_recv_async", ret);
	}
	return ret;
}

/* The deadline of a window of `win_ms` that started at `from_cyc`. */
static k_timepoint_t window_end(uint32_t win_ms, uint32_t from_cyc)
{
	uint32_t win_us = win_ms * 1000U;
	uint32_t gone_us = k_cyc_to_us_floor32(k_cycle_get_32() - from_cyc);

	return sys_timepoint_calc(K_USEC(win_us - MIN(win_us, gone_us)));
}

/* Wait until `end` for an authenticated frame from the gateway to this node.
 * Returns its replay verdict (P2P_RX_FRESH / P2P_RX_REPEAT) with the frame
 * opened into `h` / `pt`, or -ETIMEDOUT at the deadline. Frames for other
 * addresses, failing the tag or below the lane are skipped. The lane is NOT
 * advanced here: the caller does, for the frame it consumes. */
static int p2p_wait_frame(k_timepoint_t end, struct p2p_rx *rx, struct twr_hdr *h, uint8_t *pt,
			  size_t *pt_len)
{
	while (k_msgq_get(&m_rxq, rx, sys_timepoint_timeout(end)) == 0) {
		/* The cleartext header first: no decrypt for foreign traffic. */
		if (twr_hdr_get(rx->buf, rx->len, h) || h->src != m_net_id ||
		    h->dest != node_addr()) {
			continue;
		}
		if (twr_open(m_session_key, rx->buf, rx->len, h, pt, TWR_PAYLOAD_MAX, pt_len)) {
			LOG_WRN("RX from the gateway: tag check failed (counter %u)", h->counter);
			continue;
		}

		int v = p2p_replay_check(m_gw_last, h->counter);

		if (v >= 0) {
			return v;
		}
		LOG_WRN("RX from the gateway: counter %u replayed (lane %u)", h->counter,
			m_gw_last);
	}
	return -ETIMEDOUT;
}

/* The ACK window of the transmission whose receiver ran at `arm_cyc`, longer
 * by `tail` bytes of airtime when the ACK may carry answers: true once a fresh
 * ACK of `counter` arrived. `ack->tail` points into a buffer kept until the next
 * call. */
static bool p2p_wait_ack(uint32_t counter, uint32_t arm_cyc, uint8_t tail, struct p2p_rx *rx,
			 struct twr_ack *ack)
{
	static uint8_t pt[TWR_PAYLOAD_MAX];
	k_timepoint_t end = window_end(p2p_twr_window_ms(m_sf, TWR_ACK_FRAME_LEN + tail), arm_cyc);
	struct twr_hdr h;
	size_t len;
	int v;

	while ((v = p2p_wait_frame(end, rx, &h, pt, &len)) >= 0) {
		if (v != P2P_RX_FRESH || h.type != TWR_TYPE_ACK || twr_parse_ack(pt, len, ack) ||
		    ack->acked != counter) {
			continue;
		}
		m_gw_last = h.counter;
#if defined(CONFIG_SHELL)
		if (m_debug_drop_acks > 0) {
			m_debug_drop_acks--;
			LOG_WRN("Debug: ACK (counter %u) dropped, %u drop(s) left", counter,
				m_debug_drop_acks);
			continue; /* as if lost: the window runs out, then a repetition */
		}
#endif
		return true;
	}
	return false;
}

/* PENDING (plan §9.1): the receiver stays on for one gateway Data frame, the
 * downlink window from the ACK's RxDone (`rx`). A fresh one goes to m_dl; a
 * confirmed one -- fresh or a retransmission -- is owed an ACK. */
static void p2p_wait_dl(struct p2p_rx *rx, struct p2p_xfer *x)
{
	k_timepoint_t end = window_end(p2p_twr_window_ms(m_sf, P2P_FRAME_MAX), rx->cyc);
	struct twr_hdr h;
	size_t len;
	int v;

	while ((v = p2p_wait_frame(end, rx, &h, m_dl, &len)) >= 0) {
		if (h.type != TWR_TYPE_DATA) {
			continue;
		}
		app_radio_note_downlink(rx->rssi, rx->snr);
		if (v == P2P_RX_FRESH) {
			m_gw_last = h.counter;
			x->dl_len = (uint8_t)len;
		} else {
			LOG_INF("Downlink (counter %u) repeated: ACK only", h.counter);
		}
		if (h.flags & TWR_FLAG_CONFIRMED) {
			x->ack_owed = true;
			x->dl_counter = h.counter;
			x->dl_rssi = rx->rssi;
			x->dl_cyc = rx->cyc;
		}
		return;
	}
	LOG_WRN("PENDING without a downlink");
}

/* ACK a confirmed downlink (plan §4): the node's own fresh counter,
 * acked = the downlink's, rssi = this node's RSSI of it, sent the turnaround
 * after its RxDone. Skipped when the duty ledger holds it: the gateway then
 * repeats the downlink, which a later exchange re-ACKs without re-delivery. */
static void p2p_auto_ack(const struct p2p_xfer *x)
{
	static uint8_t frame[TWR_ACK_FRAME_LEN];
	uint8_t pt[TWR_ACK_PAYLOAD_LEN];
	uint32_t air = frame_toa_ms(TWR_ACK_FRAME_LEN);
	uint32_t counter;
	size_t flen;

	if (app_radio_duty_wait_ms(air) > 0) {
		LOG_WRN("Downlink ACK (counter %u) held by the duty cycle", x->dl_counter);
		return;
	}
	if (twr_counter_next(&counter)) {
		return;
	}

	const struct twr_hdr h = {
		.type = TWR_TYPE_ACK,
		.flags = 0,
		.src = node_addr(),
		.dest = m_net_id,
		.counter = counter,
	};

	sys_put_le32(x->dl_counter, &pt[0]);
	pt[4] = (uint8_t)(int8_t)CLAMP(x->dl_rssi, INT8_MIN, INT8_MAX);
	pt[5] = 0;
	if (twr_seal(m_session_key, &h, pt, sizeof(pt), frame, sizeof(frame), &flen) ||
	    radio_configure(true)) {
		return;
	}

	uint32_t since_ms = k_cyc_to_ms_floor32(k_cycle_get_32() - x->dl_cyc);

	if (since_ms < P2P_TURNAROUND_MS) {
		k_sleep(K_MSEC(P2P_TURNAROUND_MS - since_ms));
	}
	if (lora_send(m_lora_dev, frame, (uint32_t)flen)) {
		(void)tx_send_failed(flen);
		return;
	}
	app_radio_duty_charge(air);
}

/* The counter an app_radio retry of `pt` goes under again (plan §7.3): the
 * last confirmed app_radio frame's, if it carried this very plaintext and no
 * frame has taken a counter since (m_fcnt is still right behind it). */
static bool retx_counter(const uint8_t *pt, size_t pt_len, uint32_t *counter)
{
	if (!m_retx.valid || m_retx.len != pt_len || memcmp(m_retx.pt, pt, pt_len) != 0 ||
	    m_fcnt != m_retx.counter + 1U) {
		return false;
	}
	*counter = m_retx.counter;
	return true;
}

/* One exchange (plan §7.1): seal `pt` under a fresh counter and send it; a
 * confirmed frame is sent up to P2P_REPS times (byte-identical, random backoff)
 * until its ACK arrives, then an announced downlink is received and ACKed. The
 * downlink stays in m_dl for p2p_deliver(), after the radio is released, and an
 * ACK's CTRL tail in `x` -- `ack_tail` is the extra ACK airtime a request
 * (LinkCheckReq, TimeReq) waits for, 0 otherwise (plan §13.5). `attempt` is
 * the app_radio retry of a confirmed frame (0 = its first send; a retry
 * reuses its counter while retx_counter() allows), P2P_NOT_RETRIED for a
 * frame app_radio never retries (a 0x91 control frame).
 * Returns 0 when the frame went out at least once (`x->acked` for a confirmed
 * one), -EAGAIN (duty held, nothing sent), -EBUSY (listen mode), -ENOTCONN
 * (not paired), -EMSGSIZE, or a counter / radio error. Radio work queue. */
static int p2p_exchange(const uint8_t *pt, size_t pt_len, bool confirmed, uint8_t ack_tail,
			int attempt, struct p2p_xfer *x)
{
	/* Radio work queue only, one exchange at a time: off its 4 KB stack. */
	static uint8_t frame[P2P_FRAME_MAX];
	static struct p2p_rx rx;
	struct twr_ack ack = {0};
	size_t flen = TWR_HDR_LEN + pt_len + TWR_TAG_LEN;
	uint32_t air = frame_toa_ms(flen);
	uint32_t counter;

	memset(x, 0, sizeof(*x));

	if (m_listening) {
		LOG_WRN("TX skipped: radio in listen mode");
		return -EBUSY;
	}
	if (m_link_state != P2P_LINK_PAIRED) {
		/* Callers gate on app_radio_p2p_is_ready(): defensive only. */
		LOG_ERR("TX skipped: not paired");
		return -ENOTCONN;
	}
	if (pt_len > TWR_PAYLOAD_MAX || air > P2P_TOA_MAX_MS) {
		LOG_ERR("Payload %zu B over the frame budget (%u ms air)", pt_len, air);
		return -EMSGSIZE;
	}

	int64_t gap_ms = m_link_idle_at - k_uptime_get();

	if (gap_ms > 0) {
		k_sleep(K_MSEC(gap_ms)); /* at most P2P_TX_GAP_MS */
	}
	if (app_radio_duty_wait_ms(air) > 0) {
		return -EAGAIN;
	}

	int ret = 0;

	if (attempt > 0 && retx_counter(pt, pt_len, &counter)) {
		LOG_INF("Retry %d under counter %u", attempt, counter);
	} else {
		ret = twr_counter_next(&counter);
		if (ret) {
			return ret; /* fail-closed: no durably-reserved counter available */
		}
	}

	const struct twr_hdr h = {
		.type = TWR_TYPE_DATA,
		.flags = confirmed ? TWR_FLAG_CONFIRMED : 0,
		.src = node_addr(),
		.dest = m_net_id,
		.counter = counter,
	};

	ret = twr_seal(m_session_key, &h, pt, pt_len, frame, sizeof(frame), &flen);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("twr_seal", ret);
		return ret;
	}
	x->counter = counter;
	m_retx.valid = confirmed && attempt != P2P_NOT_RETRIED;
	if (m_retx.valid) {
		m_retx.counter = counter;
		m_retx.len = (uint8_t)pt_len;
		memcpy(m_retx.pt, pt, pt_len);
	}

	for (uint8_t n = 0; n < (confirmed ? P2P_REPS : 1) && !x->acked; n++) {
		uint32_t done_cyc = 0;
		uint32_t arm_cyc = 0;

		if (n > 0) {
			k_sleep(K_MSEC(sys_rand32_get() % (P2P_BACKOFF_MAX_MS + 1)));
			if (app_radio_duty_wait_ms(air) > 0) {
				LOG_WRN("Repetition of counter %u held by the duty cycle", counter);
				break;
			}
		}
		app_radio_heartbeat_feed();
		k_msgq_purge(&m_rxq);
		app_radio_air_begin();
		ret = p2p_tx_once(frame, flen, air, confirmed, &done_cyc, &arm_cyc);
		if (ret) {
			app_radio_air_end();
			break;
		}
		x->sent++;
		x->done_ms = k_uptime_get() - k_cyc_to_ms_floor32(k_cycle_get_32() - done_cyc);
		app_radio_count(APP_RADIO_CNT_TX);

		if (confirmed) {
			if (p2p_wait_ack(counter, arm_cyc, ack_tail, &rx, &ack)) {
				x->acked = true;
				x->pending = ack.pending;
				x->ack_tail_len = (uint8_t)MIN(ack.tail_len, sizeof(x->ack_tail));
				memcpy(x->ack_tail, ack.tail, x->ack_tail_len);
				x->rssi = rx.rssi;
				x->snr = rx.snr;
				m_last_ack_rssi = ack.rssi;
				m_last_ack_valid = true;
				app_radio_note_downlink(rx.rssi, rx.snr);
				if (ack.pending) {
					p2p_wait_dl(&rx, x);
				}
			}
			(void)lora_recv_async(m_lora_dev, NULL, NULL);
			if (x->ack_owed) {
				p2p_auto_ack(x);
			}
		}
		app_radio_air_end();
	}

	(void)radio_configure(true);
	m_link_idle_at = k_uptime_get() + P2P_TX_GAP_MS;

	if (x->sent == 0) {
		return ret ? ret : -EAGAIN;
	}

	app_radio_note_send(true, false);
	publish_link();
	LOG_INF("TX counter %u, %zu B, %u ms air, n=%u%s%s%s%s", counter, flen, air, x->sent,
		!confirmed ? "" : (x->acked ? " ACK" : " no ACK"), x->ack_tail_len ? " +CTRL" : "",
		x->pending ? " PENDING" : "", x->dl_len ? " +DL" : "");
	if (x->acked) {
		LOG_INF("ACK rssi=%d (gateway) dl_rssi=%d dl_snr=%d", m_last_ack_rssi, x->rssi,
			x->snr);
	}
	return 0;
}

/* ======================================================================== */
/* Control envelope: uplinks and answers (plan §8.2)                        */
/* ======================================================================== */

/* Capabilities cmd bitmap: bit (id % 8) of byte (id / 8) for every core ID
 * this node implements, either direction -- 0x01..0x04, 0x07, 0x08, 0x10,
 * 0x20. */
static const uint8_t m_caps_cmds[8] = {0x9e, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};

/* The 0x91 payload for the P2P_CTRL_BIT_* set `bits` into `pt`
 * (TWR_PAYLOAD_MAX); returns its length. The full set is 30 B. */
static size_t ctrl_build(uint32_t bits, uint8_t *pt)
{
	size_t n = 0;

	pt[n++] = P2P_ENV_CTRL;
	if (bits & P2P_CTRL_BIT_CAPS) {
		pt[n++] = P2P_CTRL_CAPABILITIES;
		pt[n++] = P2P_CAPS_LEN;
		pt[n++] = P2P_CAPS_PROTO_VERSION;
		pt[n++] = P2P_FRAME_MAX;    /* MTU: the largest frame this node receives */
		pt[n++] = P2P_PROFILE_LORA; /* fsk refuses to start (P5) */
		memcpy(&pt[n], m_caps_cmds, sizeof(m_caps_cmds));
		n += sizeof(m_caps_cmds);
		pt[n++] = P2P_POWER_CLASS_BATTERY;
	}
	if (bits & P2P_CTRL_BIT_HELLO) {
		pt[n++] = P2P_CTRL_HELLO;
		pt[n++] = P2P_HELLO_LEN;
		sys_put_le32(m_session_id, &pt[n]);
		n += 4;
		/* The hwinfo RESET_* bits 0..7 (pin, software, brownout, POR,
		 * watchdog, debug, security, low-power wake). */
		pt[n++] = (uint8_t)app_cmd_get_reset_cause();
		pt[n++] = APP_VERSION_MAJOR;
		pt[n++] = APP_VERSION_MINOR;
		pt[n++] = APP_VERSION_PATCH;
		pt[n++] = 0;
	}
	if (bits & P2P_CTRL_BIT_LINK_CHECK) {
		pt[n++] = P2P_CTRL_LINK_CHECK; /* LinkCheckReq: empty */
		pt[n++] = 0;
	}
	if (bits & P2P_CTRL_BIT_TIME) {
		pt[n++] = P2P_CTRL_TIME; /* TimeReq: empty */
		pt[n++] = 0;
	}
	return n;
}

/* A request of `bits` was acknowledged by the gateway: the answer rides that
 * ACK's CTRL tail (plan §13.5, delivered after this) or, from an older gateway,
 * the central's queue on a later PENDING. */
static void ctrl_sent(uint32_t bits, const struct p2p_xfer *x)
{
	if (bits & P2P_CTRL_BIT_TIME) {
		m_time_req_counter = x->counter;
		m_time_req_done_ms = x->done_ms;
		m_time_req_valid = true;
		atomic_clear(&m_answer_reports);
	}
	if (bits & P2P_CTRL_BIT_LINK_CHECK) {
		m_lc_outstanding = true;
		atomic_clear(&m_answer_reports);
	}
}

/* Queue `bits` for the next 0x91 uplink, `delay_ms` from now unless one is
 * already scheduled -- then they ride that one. Any thread. */
static void ctrl_request(uint32_t bits, uint32_t delay_ms)
{
	atomic_or(&m_ctrl_pending, (atomic_val_t)bits);
	k_work_schedule_for_queue(app_radio_work_q(), &m_ctrl_work, K_MSEC(delay_ms));
}

static void p2p_deliver_xfer(const struct p2p_xfer *x);

/* The 0x91 uplink: one confirmed frame with every TLV pending. Acknowledged,
 * the set is done; otherwise it is sent again after P2P_CTRL_RETRY_MS, up to
 * P2P_CTRL_TRIES times, then dropped (a lost Hello is not worth the air; a
 * TimeReq or LinkCheckReq is asked again by the next request). */
static void ctrl_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	static uint8_t pt[TWR_PAYLOAD_MAX];

	if (!app_radio_p2p_is_ready()) {
		return; /* mark_ready() schedules it again */
	}

	uint32_t bits = (uint32_t)atomic_get(&m_ctrl_pending);

	if (bits == 0) {
		return;
	}
	if (app_radio_ack_pending()) {
		k_work_reschedule_for_queue(app_radio_work_q(), dwork, K_MSEC(P2P_CTRL_HOLD_MS));
		return;
	}

	size_t len = ctrl_build(bits, pt);
	struct p2p_xfer x;
	/* The gateway answers these requests in the ACK (plan §13.5). */
	bool asks = bits & (P2P_CTRL_BIT_LINK_CHECK | P2P_CTRL_BIT_TIME);
	int ret = p2p_exchange(pt, len, true, asks ? TWR_ACK_TAIL_MAX : 0, P2P_NOT_RETRIED, &x);

	if (ret == -EAGAIN) {
		k_work_reschedule_for_queue(
			app_radio_work_q(), dwork,
			K_MSEC(MAX(duty_wait_ms_for(TWR_HDR_LEN + len + TWR_TAG_LEN),
				   (int64_t)P2P_TX_GAP_MS)));
		return;
	}
	if (ret == -ENOTCONN) {
		return;
	}
	if (ret == 0 && x.acked) {
		atomic_and(&m_ctrl_pending, ~(atomic_val_t)bits);
		m_ctrl_tries = 0;
		ctrl_sent(bits, &x);
		note_uplink_acked();
		p2p_deliver_xfer(&x);
		if (atomic_get(&m_ctrl_pending) != 0) {
			k_work_reschedule_for_queue(app_radio_work_q(), dwork, K_NO_WAIT);
		}
		return;
	}
	if (++m_ctrl_tries < P2P_CTRL_TRIES) {
		k_work_reschedule_for_queue(app_radio_work_q(), dwork, K_MSEC(P2P_CTRL_RETRY_MS));
		return;
	}
	LOG_WRN("P2P control 0x%02x: no ACK after %u tries, dropped", bits, P2P_CTRL_TRIES);
	atomic_and(&m_ctrl_pending, ~(atomic_val_t)bits);
	m_ctrl_tries = 0;
}

/* TimeAns: unix(4 LE) | frac(1, 1/256 s) | req_counter(4 LE) -- the time at the
 * TX-done of the TimeReq sent under req_counter. An answer to any other frame
 * (an older request, a replay of the first) is dropped. */
static void time_ans(const uint8_t *val)
{
	uint32_t unix_s = sys_get_le32(&val[0]);
	uint32_t req = sys_get_le32(&val[5]);

	if (!m_time_req_valid || req != m_time_req_counter) {
		LOG_WRN("TimeAns for counter %u: no such TimeReq, dropped", req);
		return;
	}
	m_time_req_valid = false;

	int64_t age_ms = k_uptime_get() - m_time_req_done_ms;

	if (age_ms > P2P_TIME_ANS_MAX_AGE_MS) {
		LOG_WRN("TimeAns %lld ms after its TimeReq: stale, dropped", age_ms);
		return;
	}

	uint32_t now = p2p_time_at(unix_s, val[4], age_ms);

	if (app_clock_set_network_time(now) == 0) {
		LOG_INF("RTC synced from network: unix=%u (TimeReq %u, %lld ms ago)", now, req,
			age_ms);
	}
	/* The time landed: app_radio answers a pending clock_sync (PF-2). */
	app_radio_time_event();
}

/* LinkCheckAns: rssi(i8) | snr(i8) | margin(i8) | gw_count -- the central's
 * view of this node's uplinks, the numbers the TOWER ACK lacks (plan §7.6). */
static void link_check_ans(const uint8_t *val)
{
	m_lc.rssi = (int8_t)val[0];
	m_lc.snr = (int8_t)val[1];
	m_lc.margin = (int8_t)val[2];
	m_lc.gw_count = val[3];
	m_lc.valid = true;
	m_lc_outstanding = false;

	app_radio_set_uplink_rssi(m_lc.rssi, m_lc.snr);
	app_radio_set_uplink_margin((uint8_t)MAX(m_lc.margin, 0), m_lc.gw_count);
	app_radio_link_result(true);
	LOG_INF("LinkCheckAns rssi=%d snr=%d margin=%d gw=%u", m_lc.rssi, m_lc.snr, m_lc.margin,
		m_lc.gw_count);
}

/* A 0x91 downlink: the TLV list after the envelope byte -- or, `in_ack`, an
 * ACK's CTRL tail (plan §13.5), which answers requests only: anything but a
 * LinkCheckAns or TimeAns there is skipped, a command stays a downlink's. */
static void ctrl_downlink(const uint8_t *buf, size_t len, bool in_ack)
{
	struct p2p_tlv t;
	size_t off = 0;
	int ret;

	while ((ret = p2p_tlv_next(buf, len, &off, &t)) > 0) {
		if (in_ack && t.cmd != P2P_CTRL_LINK_CHECK && t.cmd != P2P_CTRL_TIME) {
			LOG_INF("ACK control 0x%02x (%u B): not an answer, skipped", t.cmd, t.len);
			continue;
		}
		switch (t.cmd) {
		case P2P_CTRL_CAPABILITIES:
			if (t.len >= 2) {
				LOG_INF("Central capabilities: proto %u, MTU %u", t.val[0],
					t.val[1]);
			}
			break;
		case P2P_CTRL_DETACH:
			LOG_WRN("Detach (reason %u): pairing cleared, radio idle until reboot "
				"or `join`",
				t.len ? t.val[0] : 0);
			(void)pairing_clear();
			return; /* the session is gone: nothing after it applies */
		case P2P_CTRL_REJOIN_REQ:
			/* The key that authenticated this outlives a config edit, so the
			 * identity could have been cleared since the join: re-joining
			 * would then put an all-zero app_key or DevEUI on the air. */
			if (!app_key_is_set() || !addr_is_valid()) {
				LOG_ERR("RejoinReq, but the device is unprovisioned: not "
					"re-joining");
				return;
			}
			LOG_WRN("RejoinReq (kind %u): re-joining", t.len ? t.val[0] : 0);
			/* Self-heal policy (§7): a paired node asked to rekey keeps
			 * trying past the 120 s cap, with backoff. */
			start_join_episode(true);
			return;
		case P2P_CTRL_LINK_CHECK:
			if (t.len >= P2P_LINK_CHECK_ANS_LEN) {
				link_check_ans(t.val);
			}
			break;
		case P2P_CTRL_TIME:
			if (t.len >= P2P_TIME_ANS_LEN) {
				time_ans(t.val);
			}
			break;
		default:
			LOG_INF("Control 0x%02x (%u B): not handled, skipped", t.cmd, t.len);
			break;
		}
	}
	if (ret < 0) {
		LOG_WRN("Control %s malformed at byte %zu", in_ack ? "ACK tail" : "downlink", off);
	}
}

/* A gateway Data frame's payload, once the radio is released: a command
 * (0x81 port 86) to app_cmd -- allow-lists and the M-3 gate unchanged -- or a
 * control list (0x91). */
static void p2p_deliver(const uint8_t *pt, size_t len)
{
	if (len >= 1 && pt[0] == P2P_ENV_CTRL) {
		ctrl_downlink(&pt[1], len - 1, false);
		return;
	}
	if (len > P2P_ENV_LEN && pt[0] == P2P_ENV_DATA && pt[1] == P2P_PORT_COMMAND) {
		LOG_INF("Command received (%zu B)", len - P2P_ENV_LEN);
		app_radio_downlink(&pt[P2P_ENV_LEN], len - P2P_ENV_LEN);
		return;
	}
	LOG_WRN("Downlink %02x %02x (%zu B): unknown envelope or port, dropped", len ? pt[0] : 0,
		len > 1 ? pt[1] : 0, len);
}

static void p2p_deliver_xfer(const struct p2p_xfer *x)
{
	/* First: a TimeAns here dates from x->done_ms, the TX-done of the
	 * transmission it answers -- ctrl_sent() has already recorded that. */
	if (x->ack_tail_len) {
		ctrl_downlink(x->ack_tail, x->ack_tail_len, true);
	}
	if (x->dl_len) {
		p2p_deliver(m_dl, x->dl_len);
	}
}

/* An answer the central queued (TimeAns, LinkCheckAns) rides the next report's
 * PENDING. A LinkCheckAns still missing after P2P_ANSWER_REPORTS_MAX reports
 * is lost and must not block the next LinkCheckReq; a late TimeAns is still
 * taken (time_ans() caps its age). */
static void answer_fetch_count(void)
{
	bool answer_wanted = (m_time_req_valid && app_radio_time_wanted()) || m_lc_outstanding;

	if (answer_wanted && atomic_inc(&m_answer_reports) >= P2P_ANSWER_REPORTS_MAX) {
		m_lc_outstanding = false;
	}
}

/* struct app_radio_backend.time_request: a TimeReq in the next 0x91 uplink;
 * the TimeAns lands via app_radio_time_event(). */
static void p2p_time_request(void)
{
	LOG_INF("Network time requested (TimeReq)");
	ctrl_request(P2P_CTRL_BIT_TIME, 0);
}

/* ---- TX backend: app_radio schedules, this sends one frame (doc/plan/460 F4) ---- */

/* struct app_radio_backend.send: the LoRaWAN fPort payload in the 0x81
 * envelope, on the LoRaWAN port (D-a). Radio work queue only. */
static int p2p_tx_send(const struct app_radio_frame *f, struct app_radio_tx_result *res)
{
	static uint8_t pt[TWR_PAYLOAD_MAX];
	bool confirmed = (f->flags & APP_RADIO_FRAME_CONFIRMED) != 0;
	struct p2p_xfer x;
	uint8_t port;

	switch (f->kind) {
	case APP_RADIO_FRAME_TELEMETRY:
		port = P2P_PORT_TELEMETRY;
		break;
	case APP_RADIO_FRAME_ALARM:
		port = P2P_PORT_ALARM;
		break;
	default:
		port = f->port ? f->port : P2P_PORT_RESPONSE;
		break;
	}
	if (f->len == 0) {
		return -EINVAL; /* no MAC to flush: the P2P budget is never 0 */
	}
	if (f->len > P2P_MAX_BODY) {
		res->budget = P2P_MAX_BODY;
		return -EMSGSIZE;
	}

	pt[0] = P2P_ENV_DATA;
	pt[1] = port;
	memcpy(&pt[P2P_ENV_LEN], f->buf, f->len);

	int ret = p2p_exchange(pt, P2P_ENV_LEN + f->len, confirmed, 0, f->attempt, &x);

	switch (ret) {
	case 0:
		break;
	case -EMSGSIZE:
		res->budget = P2P_MAX_BODY;
		return ret;
	case -EAGAIN:
		res->wait_ms = (uint32_t)MAX(
			duty_wait_ms_for(TWR_HDR_LEN + P2P_ENV_LEN + f->len + TWR_TAG_LEN), 0);
		return ret;
	default:
		/* -EBUSY: the radio listens (kicked when it ends); -ENOTCONN: no
		 * session (kicked at the next JoinAccept); other: a radio fault. */
		return ret;
	}
	if (!confirmed) {
		return 0;
	}
	if (!x.acked) {
		return -ETIMEDOUT; /* app_radio retries, under the same counter if it can */
	}
	note_uplink_acked(); /* a plain ACK counts as "link alive" (plan §8.2) */
	p2p_deliver_xfer(&x);
	return 0;
}

static uint8_t p2p_tx_budget(void)
{
	return P2P_MAX_BODY;
}

/* struct app_radio_backend.airtime_ms: a `len`-byte body on air at the current
 * SF, header, envelope and tag included. One transmission: a repetition
 * checks the ledger again. */
static uint32_t p2p_tx_airtime_ms(size_t len)
{
	return frame_toa_ms(TWR_HDR_LEN + P2P_ENV_LEN + len + TWR_TAG_LEN);
}

/* Once per report, at its first frame, kept for all its frames. Plan §7.2
 * (F6): every report goes CONFIRMED, so a downlink the central queued waits at
 * most one report interval for its PENDING. Only a report the cadence made a
 * link check (`due`) is flagged as one: unacknowledged, it alone is a failed
 * check. It also asks the central for the numbers (LinkCheckReq) after its
 * frames, unless an earlier one is still unanswered. */
static uint8_t p2p_tx_report_flags(bool due)
{
	if (due && !m_lc_outstanding) {
		ctrl_request(P2P_CTRL_BIT_LINK_CHECK, P2P_LINK_CHECK_DELAY_MS);
	}
	answer_fetch_count();
	return APP_RADIO_FRAME_CONFIRMED | (due ? APP_RADIO_FRAME_LINK_CHECK : 0);
}

const struct app_radio_backend app_radio_p2p_backend = {
	.send = p2p_tx_send,
	.budget = p2p_tx_budget,
	.tx_ready = app_radio_p2p_is_ready,
	.report_flags = p2p_tx_report_flags,
	.get_state = app_radio_p2p_get_state,
	.warning_step = p2p_warning_step,
	.rejoin = p2p_tx_rejoin,
	.time_request = p2p_time_request,
	.airtime_ms = p2p_tx_airtime_ms,
	/* §7.2: answers and history frames are confirmed, telemetry too
	 * (report_flags); alarms as radio-alarm-ack says (app_radio). */
	.confirm_kinds = BIT(APP_RADIO_FRAME_ANSWER) | BIT(APP_RADIO_FRAME_HISTORY),
	.frame_gap_ms = 0, /* P2P_TX_GAP_MS after an exchange is taken in p2p_exchange() */
	.cmd_transport = APP_CMD_TRANSPORT_P2P,
};

/* ======================================================================== */
/* Listen mode (reference receiver / diagnostics, CONFIG_SHELL)             */
/* ======================================================================== */

#if defined(CONFIG_SHELL)
/* Log the cleartext TOWER header of every frame heard: this mode has no key to
 * open them with. */
static void rx_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct p2p_rx rx;
	struct twr_hdr h;

	while (k_msgq_get(&m_rxq, &rx, K_NO_WAIT) == 0) {
		if (twr_hdr_get(rx.buf, rx.len, &h)) {
			LOG_INF("RX %u B, not a TOWER frame (RSSI %d dBm, SNR %d dB)", rx.len,
				rx.rssi, rx.snr);
			continue;
		}
		LOG_INF("RX type %u flags 0x%02x src %08x dest %08x counter %u, %u B (RSSI %d "
			"dBm, SNR %d dB)",
			h.type, h.flags, h.src, h.dest, h.counter, rx.len, rx.rssi, rx.snr);
	}
}

int app_radio_p2p_listen(bool enable)
{
	if (enable == m_listening) {
		return 0;
	}

	if (enable) {
		int ret = radio_configure(false);

		if (ret) {
			return ret;
		}
		m_listening = true; /* before RX: p2p_recv_cb() submits the drain */
		k_msgq_purge(&m_rxq);
		ret = lora_recv_async(m_lora_dev, p2p_recv_cb, NULL);
		if (ret) {
			m_listening = false;
			LOG_ERR_CALL_FAILED_INT("lora_recv_async", ret);
			return ret;
		}
		LOG_INF("P2P listen: ON");
	} else {
		(void)lora_recv_async(m_lora_dev, NULL, NULL);
		m_listening = false;
		(void)radio_configure(true);
		LOG_INF("P2P listen: OFF");
		app_radio_tx_kick(); /* frames the listen mode bounced with -EBUSY */
	}
	return 0;
}
#endif /* defined(CONFIG_SHELL) */

/* ======================================================================== */
/* Join handshake (plan §6.3)                                               */
/* ======================================================================== */

static void mark_ready(void)
{
	/* Paired: back to the fast policy. */
	m_join_slow = false;
	set_rejoin_attempt(0);

	m_started = true;
	/* The TX power assignment is NOT reset here: pairing_persist() has
	 * already installed this session's value (or cleared it), and
	 * mark_ready() also runs on the already-PAIRED boot shortcut, where the
	 * value restored from NVS is the one to keep. */

	/* Link level (plan §8.2): what this node understands and that it
	 * (re)started. A time still wanted from before the session changed is
	 * asked again, as session_reset() dropped it. Scheduled BEFORE the
	 * link-up below, whose own TimeReq (no network time yet) then rides this
	 * jittered frame instead of sending one at once. */
	m_ctrl_tries = 0;
	ctrl_request(P2P_CTRL_BIT_CAPS | P2P_CTRL_BIT_HELLO |
			     (app_radio_time_wanted() ? P2P_CTRL_BIT_TIME : 0),
		     sys_rand32_get() % P2P_CTRL_START_JITTER_MS);

	/* Link supervision and the M-2 clock start afresh; the first report of
	 * the session is the link check (§3.2). May ask for the network time. */
	app_radio_link_up();

	/* Link up (boot with a persisted pairing, or a fresh JoinAccept): the
	 * common Info + settings-info announce, as after a LoRaWAN join. Queued on
	 * the radio work queue ahead of the telemetry the ready callback kicks. */
	app_radio_announce();
	/* Frames parked while unpaired leave now, under this session. */
	app_radio_tx_kick();
	if (m_ready_cb) {
		m_ready_cb();
	}
}

/* Build one JoinRequest into `frame` (P2P_JOIN_REQ_LEN bytes): a TOWER Data
 * frame, src = this node, dest = 0, counter = dev_nonce, sealed under join_key,
 * payload 0x91 JoinReq (product_type | proto_version | dev_eui(8, MSB-first) |
 * fw major, minor, patch, 0). Factored out of send_join_request() so a ztest
 * pins the exact bytes against tests/ccm/tower_join_kat.json -- the on-air
 * frame and the tested frame are the same code. */
static int join_request_build(uint32_t dev_nonce, uint8_t frame[P2P_JOIN_REQ_LEN])
{
	uint8_t key[P2P_KEY_LEN];
	uint8_t pt[3 + P2P_JOIN_REQ_VALUE_LEN];
	size_t len;

	pt[0] = P2P_ENV_CTRL;
	pt[1] = P2P_CTRL_JOIN_REQ;
	pt[2] = P2P_JOIN_REQ_VALUE_LEN;
	pt[3] = P2P_PRODUCT_TYPE_STICKER;
	pt[4] = APP_PROTO_VERSION;
	memcpy(&pt[5], g_app_config.radio_deveui, sizeof(g_app_config.radio_deveui));
	pt[13] = APP_VERSION_MAJOR;
	pt[14] = APP_VERSION_MINOR;
	pt[15] = APP_VERSION_PATCH;
	pt[16] = 0;

	const struct twr_hdr h = {
		.type = TWR_TYPE_DATA,
		.flags = 0,
		.src = node_addr(),
		.dest = TWR_ADDR_NONE,
		.counter = dev_nonce,
	};

	derive_join_key(key);
	return twr_seal(key, &h, pt, sizeof(pt), frame, P2P_JOIN_REQ_LEN, &len);
}

/* Open and validate a JoinAccept for `dev_nonce` (plan §6.3): a TOWER Data
 * frame from the network to this node, counter = dev_nonce (echo), sealed under
 * join_key, whose 0x91 payload carries JoinAccept. Returns 0 with `*ja` filled,
 * or a negative errno. */
static int join_accept_open(uint32_t dev_nonce, const uint8_t *frame, size_t len,
			    struct p2p_join_accept *ja)
{
	uint8_t key[P2P_KEY_LEN];
	uint8_t pt[TWR_PAYLOAD_MAX];
	struct twr_hdr h;
	struct p2p_tlv t;
	size_t pt_len;
	size_t off = 0;
	int ret = twr_hdr_get(frame, len, &h);

	if (ret) {
		return ret;
	}
	if (h.type != TWR_TYPE_DATA || h.dest != node_addr() || h.counter != dev_nonce) {
		LOG_WRN("JoinAccept: header mismatch (type %u, dest %08x, counter %u, want %u)",
			h.type, h.dest, h.counter, dev_nonce);
		return -EBADMSG;
	}

	derive_join_key(key);
	if (twr_open(key, frame, len, &h, pt, sizeof(pt), &pt_len)) {
		LOG_WRN("JoinAccept: auth failed");
		return -EBADMSG;
	}
	if (pt_len < 1 || pt[0] != P2P_ENV_CTRL) {
		return -EBADMSG;
	}
	while ((ret = p2p_tlv_next(&pt[1], pt_len - 1, &off, &t)) > 0) {
		if (t.cmd == P2P_CTRL_JOIN_ACCEPT) {
			return p2p_parse_join_accept(t.val, t.len, h.src, ja);
		}
	}
	return -EBADMSG;
}

#if defined(CONFIG_ZTEST)
/* Test hooks for the join (plan §6.3). They build and open the frames and
 * derive the keys through exactly the code the radio path uses, so the KAT
 * vectors in tests/p2p_logic pin the shipped bytes rather than a re-spelling
 * of them. None transmits or persists. */
int p2p_test_build_join_request(uint32_t dev_nonce, uint8_t out[P2P_JOIN_REQ_LEN])
{
	return join_request_build(dev_nonce, out);
}

int p2p_test_open_join_accept(uint32_t dev_nonce, const uint8_t *frame, size_t len)
{
	struct p2p_join_accept ja;

	return join_accept_open(dev_nonce, frame, len, &ja);
}

void p2p_test_derive_join_key(uint8_t out[P2P_KEY_LEN])
{
	derive_join_key(out);
}

void p2p_test_derive_session_key(uint32_t dev_nonce, uint32_t central_nonce,
				 uint8_t out[P2P_KEY_LEN])
{
	derive_session_key(dev_nonce, central_nonce, out);
}
#endif /* defined(CONFIG_ZTEST) */

/* Send one JoinRequest (plan §6.3). Persists the advanced dev_nonce BEFORE
 * sending: once a JoinRequest *could* have reached the air, that nonce value
 * must never be reused -- it is the join_key frame counter -- even if the TX or
 * the round-trip afterward fails. Returns 0 (with `*used_nonce`/`*tx_end_ms`
 * set) or -EAGAIN (duty-cycle blocked) or an errno. */
static int send_join_request(uint32_t *used_nonce, int64_t *tx_end_ms)
{
	if (duty_wait_ms_for(P2P_JOIN_REQ_LEN) > 0) {
		return -EAGAIN;
	}

	/* TOWER reserves counter 0, the first dev_nonce of a new device. */
	uint32_t nonce_val = MAX(m_dev_nonce, 1U);

	/* Fail closed (review of #400): a dev_nonce the flash does not hold would
	 * be presented again after a reboot, and a recorded JoinAccept for it
	 * replayed -- the old session key back with the counter at 0. */
	if (dnonce_persist(nonce_val + 1) != 0) {
		return -EIO;
	}

	uint8_t frame[P2P_JOIN_REQ_LEN]; /* 39 B */
	int ret = join_request_build(nonce_val, frame);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("join_request_build", ret);
		return ret;
	}

	app_radio_air_begin(); /* until the JoinAccept window closed */
	ret = lora_send(m_lora_dev, frame, sizeof(frame));
	if (ret) {
		app_radio_air_end();
		LOG_ERR_CALL_FAILED_INT("lora_send", ret);
		return tx_send_failed(sizeof(frame));
	}

	int64_t end = k_uptime_get();
	uint32_t air = frame_toa_ms(sizeof(frame));

	app_radio_duty_charge(air);
	app_radio_count(APP_RADIO_CNT_JOIN);
	publish_link();

	LOG_INF("JoinRequest sent (addr %08x, dev_nonce %u, %u ms air)", node_addr(), nonce_val,
		air);

	*used_nonce = nonce_val;
	*tx_end_ms = end;
	return 0;
}

/* Wait for and process JoinAccept in the RX1 window following a JoinRequest.
 * On success, derives session_key and persists the new pairing (NVS + module
 * state, via pairing_persist()). Returns 0 on a valid, matching JoinAccept;
 * a negative errno otherwise (timeout, malformed frame, or auth failure --
 * all just mean "no accept this attempt", not a hard error). */
static int recv_join_accept(uint32_t dev_nonce, int64_t tx_end_ms)
{
	uint8_t buf[P2P_FRAME_MAX];
	struct p2p_join_accept ja;
	int16_t rssi;
	int8_t snr;
	int len = p2p_rx_window(tx_end_ms, P2P_RX1_DELAY_DEFAULT_S, P2P_JOIN_ACCEPT_LEN, buf,
				sizeof(buf), &rssi, &snr);

	if (len < 0) {
		return len;
	}

	int ret = join_accept_open(dev_nonce, buf, (size_t)len, &ja);

	if (ret) {
		return ret;
	}

	uint8_t session_key[P2P_KEY_LEN];

	derive_session_key(dev_nonce, ja.central_nonce, session_key);

	ret = pairing_persist(&ja, session_key);
	if (ret) {
		return ret;
	}
	LOG_INF("Joined: net_id=%08x addr=%08x rx_delay=%us (RSSI %d dBm, SNR %d dB)", ja.net_id,
		node_addr(), ja.rx_delay_s, rssi, snr);
	if (ja.tx_power_assigned) {
		LOG_INF("Session TX power assigned: %d dBm (config %d dBm)", ja.tx_power_dbm,
			g_app_config.p2p_tx_power);
	}
	return 0;
}

/* §5.2: the never-paired boot join's 120 s window is over. It ends the FAST
 * retry policy, not the episode -- the node keeps looking, on the same
 * exponential curve a self-heal uses (§7), converging to one JoinRequest an
 * hour. Going UNPAIRED and silent here is what stranded a node switched on
 * before its Hub: nothing short of a power cycle would ever have brought it
 * back.
 *
 * The curve restarts at its first step, because this is the first round of the
 * slow phase, not a continuation of anything. */
static void join_window_expired(void)
{
	LOG_WRN("P2P join: boot window (%d s) expired without a JoinAccept; continuing "
		"on slow backoff",
		P2P_JOIN_BOOT_WINDOW_MS / 1000);
	m_join_slow = true;
	set_rejoin_attempt(0);
}

static void join_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	if (m_link_state != P2P_LINK_JOINING) {
		return; /* paired (or reverted) while a retry was already in flight */
	}

	uint32_t used_nonce;
	int64_t tx_end;
	int ret = send_join_request(&used_nonce, &tx_end);
	/* Captured before recv_join_accept() overwrites `ret`: only the SEND can
	 * report the duty ledger's refusal, and that refusal is the one outcome
	 * that put nothing on the air. */
	bool duty_blocked = (ret == -EAGAIN);

	if (ret == 0) {
		ret = recv_join_accept(used_nonce, tx_end);
		if (ret == 0) {
			mark_ready();
			return; /* paired; no more retries */
		}
		LOG_INF("JoinAccept not received/invalid (%d); retrying", ret);
	} else if (!duty_blocked) {
		LOG_ERR_CALL_FAILED_INT("send_join_request", ret);
	}

	/* A JoinRequest that reached the air ends a round of the slow policy. So
	 * does a hard radio fault, which tried nothing but must still back off --
	 * otherwise a dead modem is retried every jitter interval, with a
	 * dev_nonce flash write each time. A duty bounce only waits for the
	 * ledger, without spending a backoff step. */
	bool round_end = !duty_blocked;

	int64_t duty_wait_ms = duty_blocked ? duty_wait_ms_for(P2P_JOIN_REQ_LEN) : 0;
	int64_t wait_ms = 0;
	uint32_t base = 0;

	if (!m_join_slow) {
		wait_ms = p2p_join_retry_delay_ms(false, k_uptime_get() - m_join_started_at,
						  duty_wait_ms, 0, P2P_JOIN_RETRY_JITTER_MS);
		/* < 0 means the boot window is over -- either it closed while this
		 * attempt was running, or the duty ledger cannot clear before it
		 * does, so the next wake-up would land past the deadline anyway.
		 * Either way the fast policy is finished and the slow one takes the
		 * episode from here; this is the only place that transition
		 * happens. */
		if (wait_ms < 0) {
			join_window_expired();
		}
	}

	if (m_join_slow) {
		/* No window on the slow policy (§7): the backoff curve. */
		if (round_end) {
			base = app_radio_rejoin_backoff_ms(m_rejoin_attempt);
		}
		wait_ms = p2p_join_retry_delay_ms(true, 0, duty_wait_ms, base,
						  P2P_JOIN_RETRY_JITTER_MS);
	}

	if (m_join_slow && round_end) {
		/* Exponential backoff between rounds, +/-25% jitter. A duty-cycle-
		 * blocked (-EAGAIN) round waits for the ledger instead when that is
		 * the longer of the two (p2p_join_retry_delay_ms), and the jitter
		 * may not undercut it (app_radio_backoff_jitter_ms). */
		if (m_rejoin_attempt < UINT8_MAX) {
			set_rejoin_attempt(m_rejoin_attempt + 1);
		}
		wait_ms =
			app_radio_backoff_jitter_ms(wait_ms, duty_wait_ms, base, sys_rand32_get());
	} else {
		wait_ms += sys_rand32_get() % P2P_JOIN_RETRY_JITTER_MS;
	}

	k_work_reschedule_for_queue(app_radio_work_q(), dwork, K_MSEC(wait_ms));
}

#if defined(CONFIG_ZTEST)
/* Initialise the work items once per test binary, the way app_radio_p2p_init()
 * does; the suite's stub of app_radio_work_q() provides the queue. Shared by
 * every setup hook below, as the suite runs many tests. */
static void test_queue_start_once(void)
{
	static bool started;

	if (started) {
		return;
	}
	k_work_init_delayable(&m_join_work, join_work_handler);
	k_work_init_delayable(&m_ctrl_work, ctrl_work_handler);
	started = true;
}

/* Put the module into a fresh boot-policy JOINING episode configured for
 * `cfg_sf`, with the duty ledger empty, so a test can read the state the first
 * JoinRequest will go out on before it drives a single step. */
void p2p_test_join_setup(int cfg_sf)
{
	test_queue_start_once();
	g_app_config.p2p_spreading_factor = cfg_sf;
	app_radio_duty_init(app_radio_duty_budget_ms(g_app_config.p2p_frequency));
	m_link_state = P2P_LINK_JOINING;
	m_join_slow = false;
	set_rejoin_attempt(0);
	m_join_started_at = k_uptime_get();
	m_sf = (uint8_t)sf_from_cfg();
	(void)radio_configure(true);
}

/* The answer-fetching state behind the confirmed reports (plan H3.8). */
void p2p_test_link_reset(void)
{
	atomic_clear(&m_answer_reports);
	m_time_req_valid = false;
	m_lc_outstanding = false;
}

/* Stop a join episode a case started, waiting out an attempt in progress. */
void p2p_test_join_stop(void)
{
	struct k_work_sync sync;

	m_link_state = P2P_LINK_UNPAIRED;
	(void)k_work_cancel_delayable_sync(&m_join_work, &sync);
}

void p2p_test_set_session_tx_power(bool assigned, int8_t dbm)
{
	m_session_tx_power_assigned = assigned;
	m_session_tx_power_dbm = dbm;
}

/* Run exactly one join_work_handler iteration on the caller's thread and cancel
 * the retry it scheduled, so the work-queue thread cannot run a second attempt
 * underneath the assertions. */
void p2p_test_join_step(void)
{
	join_work_handler(&m_join_work.work);
	(void)k_work_cancel_delayable(&m_join_work);
}

/* Force the link-state inputs of app_radio_p2p_get_state()/is_ready(), so the
 * mapping to the common app_radio state can be checked without driving a join
 * or a run of failed uplinks through the radio. */
void p2p_test_set_link(enum p2p_link_state state, bool started, bool slow, bool disabled)
{
	m_link_state = state;
	m_started = started;
	m_join_slow = slow;
	m_disabled = disabled;
}

/* Record a downlink as the exchange does after authenticating one. */
void p2p_test_note_downlink(int16_t rssi, int8_t snr)
{
	app_radio_note_downlink(rssi, snr);
}

/* Put the link where a node that was paired under older firmware boots: PAIRED
 * from the persisted record, but not yet started. */
void p2p_test_set_paired(void)
{
	m_link_state = P2P_LINK_PAIRED;
	m_started = false;
}

/* A live session under `net_id` / `key`, as a JoinAccept leaves it (without the
 * flash write the test backend refuses). */
void p2p_test_set_session(uint32_t net_id, const uint8_t key[P2P_KEY_LEN])
{
	test_queue_start_once();
	m_net_id = net_id;
	memcpy(m_session_key, key, P2P_KEY_LEN);
	m_link_state = P2P_LINK_PAIRED;
	m_started = true;
	session_reset();
}

void p2p_test_tx_reset(void)
{
	m_link_idle_at = 0;
	k_msgq_purge(&m_rxq);
	atomic_clear(&m_ctrl_pending);
	m_ctrl_tries = 0;
	m_retx.valid = false;
	(void)k_work_cancel_delayable(&m_ctrl_work);
}

/* Arm the join retry with a known delay, standing in for a slow-phase pass end
 * without spending a real pass to get there. */
void p2p_test_join_arm_retry(int64_t ms)
{
	k_work_reschedule_for_queue(app_radio_work_q(), &m_join_work, K_MSEC(ms));
}

/* Re-enter start_join_episode() the way the shell `join` verb does. */
void p2p_test_join_restart(void)
{
	start_join_episode(false);
}

/* How long the pending join retry still has to wait, or 0 if none is armed. */
int64_t p2p_test_join_pending_ms(void)
{
	return k_ticks_to_ms_floor64(k_work_delayable_remaining_get(&m_join_work));
}

void p2p_test_get_join(uint8_t *sf, bool *slow, uint8_t *rejoin, enum p2p_link_state *state)
{
	if (sf) {
		*sf = m_sf;
	}
	if (slow) {
		*slow = m_join_slow;
	}
	if (rejoin) {
		*rejoin = m_rejoin_attempt;
	}
	if (state) {
		*state = m_link_state;
	}
}

/* Move the episode's start back, so a test can reach the boot window's edge
 * without waiting 120 s for it. */
void p2p_test_set_join_started_at(int64_t at_ms)
{
	m_join_started_at = at_ms;
}

void p2p_test_set_dev_nonce(uint32_t v)
{
	m_dev_nonce = v;
}

uint32_t p2p_test_get_dev_nonce(void)
{
	return m_dev_nonce;
}

uint32_t p2p_test_get_gw_last(void)
{
	return m_gw_last;
}

/* One exchange as p2p_tx_send() runs it, for a payload the test composed:
 * 0, -ETIMEDOUT (confirmed, no ACK) or the exchange's errno; a downlink is
 * delivered. */
int p2p_test_uplink_tail(const uint8_t *pt, size_t pt_len, bool confirmed, uint8_t ack_tail)
{
	struct p2p_xfer x;
	int ret = p2p_exchange(pt, pt_len, confirmed, ack_tail, P2P_NOT_RETRIED, &x);

	if (ret) {
		return ret;
	}
	if (confirmed && !x.acked) {
		return -ETIMEDOUT;
	}
	p2p_deliver_xfer(&x);
	return 0;
}

int p2p_test_uplink(const uint8_t *pt, size_t pt_len, bool confirmed)
{
	return p2p_test_uplink_tail(pt, pt_len, confirmed, 0);
}

void p2p_test_ctrl_downlink(const uint8_t *val, size_t len)
{
	ctrl_downlink(val, len, false);
}

uint32_t p2p_test_ctrl_pending(void)
{
	return (uint32_t)atomic_get(&m_ctrl_pending);
}

/* Run the 0x91 uplink now on the caller's thread and cancel what it scheduled. */
void p2p_test_ctrl_run(void)
{
	ctrl_work_handler(&m_ctrl_work.work);
	(void)k_work_cancel_delayable(&m_ctrl_work);
}

void p2p_test_set_time_req(uint32_t counter, int64_t done_ms)
{
	m_time_req_counter = counter;
	m_time_req_done_ms = done_ms;
	m_time_req_valid = true;
}
#endif /* defined(CONFIG_ZTEST) */

/* ======================================================================== */
/* Public API                                                                */
/* ======================================================================== */

int app_radio_p2p_init(void)
{
	if (!device_is_ready(m_lora_dev)) {
		LOG_ERR("LoRa device not ready");
		return -ENODEV;
	}

	/* The allowance of the EU868 sub-band the channel is in: 1 % on the
	 * default 868.1 MHz, 0.1 % or 10 % on others (app_radio_duty_budget_ms). */
	app_radio_duty_init(app_radio_duty_budget_ms(g_app_config.p2p_frequency));

	int ret = settings_register(&m_fcnt_sh);

	if (ret && ret != -EEXIST) {
		LOG_ERR_CALL_FAILED_INT("settings_register", ret);
		return ret;
	}
	ret = settings_load_subtree(P2P_FCNT_SUBTREE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_load_subtree", ret);
		return ret;
	}

	ret = settings_register(&m_join_sh);
	if (ret && ret != -EEXIST) {
		LOG_ERR_CALL_FAILED_INT("settings_register", ret);
		return ret;
	}
	ret = settings_load_subtree(P2P_JOIN_SUBTREE);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_load_subtree", ret);
		return ret;
	}

	m_sf = (uint8_t)sf_from_cfg();

	ret = radio_configure(true);
	if (ret) {
		return ret;
	}

	m_session_id = sys_rand32_get();

	k_work_init_delayable(&m_join_work, join_work_handler);
	k_work_init_delayable(&m_ctrl_work, ctrl_work_handler);
#if defined(CONFIG_SHELL)
	k_work_init(&m_rx_work, rx_work_handler);
	k_work_init(&m_debug_compose_work, debug_compose_work_handler);
#endif

	/* The radio work queue's liveness heartbeat and the M-2 watchdog (#182). */
	app_radio_heartbeat_start();

	app_compose_reset();

	return 0;
}

void app_radio_p2p_start(void)
{
	/* No usable root key -- see app_key_is_set() above for why this refuses
	 * outright instead of trying.
	 *
	 * Deliberately checked BEFORE the PAIRED branch. The reset tiers that drop
	 * the network session (factory_reset, vendor_reset, lrw_reset) clear the
	 * pairing through app_radio_reset_link(), but a pairing restored by
	 * join_settings_set() survives device_reset and a config write that zeroes
	 * radio_appkey. Such a node would take the PAIRED shortcut and resume
	 * transmitting under a session it can never re-derive. */
	if (!app_key_is_set()) {
		LOG_ERR("P2P not started: radio_appkey is all-zero (device unprovisioned). "
			"Set radio-appkey over NFC or shell, then reboot.");
		m_disabled = true;
		return;
	}

	/* Also before the PAIRED branch: the address is in every frame header, so
	 * a session is no use without it either (plan §6.1). An all-zero DevEUI
	 * (unprovisioned) lands here too. */
	if (!addr_is_valid()) {
		LOG_ERR("P2P not started: radio_deveui gives address %08x, which is reserved "
			"(device unprovisioned?). Set radio-deveui over NFC or shell, then "
			"reboot.",
			node_addr());
		m_disabled = true;
		return;
	}

	if (!phy_is_supported()) {
		LOG_ERR("P2P not started: p2p-modulation fsk is not supported yet. Set "
			"p2p-modulation lora, then reboot.");
		m_disabled = true;
		return;
	}

	if (m_link_state == P2P_LINK_PAIRED) {
		/* Persisted pairing from a prior boot: no re-join needed (§7 --
		 * a session survives normal power cycles). */
		mark_ready();
		return;
	}

	/* Unpaired: kick off the boot-window join handshake (§5.2).
	 * app_radio_p2p_is_ready() only goes true once JoinAccept lands
	 * (mark_ready(), called from join_work_handler()). */
	start_join_episode(false);
}

bool app_radio_p2p_is_ready(void)
{
	/* m_started alone stayed true through a self-heal / RejoinReq join, so
	 * the node reported HEALTHY and kept composing telemetry into a session
	 * that was being replaced (parity review 2026-09-26). */
	return m_started && m_link_state == P2P_LINK_PAIRED;
}

enum app_radio_state app_radio_p2p_get_state(void)
{
	if (m_disabled) {
		return APP_RADIO_STATE_DISABLED;
	}

	switch (m_link_state) {
	case P2P_LINK_PAIRED:
		if (!m_started) {
			return APP_RADIO_STATE_IDLE;
		}
		return APP_RADIO_STATE_HEALTHY; /* app_radio adds WARNING */
	case P2P_LINK_JOINING:
		/* The slow policy runs for a self-heal / RejoinReq episode and
		 * after an unanswered boot window: a reconnect with backoff. */
		return m_join_slow ? APP_RADIO_STATE_RECONNECT : APP_RADIO_STATE_JOINING;
	case P2P_LINK_UNPAIRED:
	default:
		return APP_RADIO_STATE_IDLE;
	}
}

uint8_t app_radio_p2p_get_max_payload(void)
{
	return P2P_MAX_BODY;
}

void app_radio_p2p_register_ready_cb(void (*cb)(void))
{
	m_ready_cb = cb;
}

void app_radio_p2p_suspend(void)
{
	/* Nothing queued survives a poweroff -- the work queue itself (including
	 * any pending m_join_work retry) is torn down with the reboot that follows
	 * deep-sleep entry, same as app_radio_lrw_suspend() relies on for its own
	 * timers. No-op today; kept as an explicit facade hook. */
}

void app_radio_p2p_get_info(struct app_radio_p2p_info *info)
{
	info->link_state = m_link_state;
	info->addr = node_addr();
	info->net_id = m_net_id;
	info->rx_delay_s = m_rx_delay_s;
	info->sf = m_sf;
	info->tx_power_assigned = m_session_tx_power_assigned;
	info->tx_power_dbm = m_session_tx_power_assigned ? m_session_tx_power_dbm
							 : (int8_t)g_app_config.p2p_tx_power;
	info->fcnt = m_fcnt;
	info->dev_nonce = m_dev_nonce;
	info->gw_last = m_gw_last;
	info->ack_retry_pending = app_radio_ack_pending() ? 1 : 0;
	info->last_ack_rssi = m_last_ack_rssi;
	info->last_ack_valid = m_last_ack_valid;
	info->lc_valid = m_lc.valid;
	info->lc_rssi = m_lc.rssi;
	info->lc_snr = m_lc.snr;
	info->lc_margin = m_lc.margin;
	info->lc_gw_count = m_lc.gw_count;
	info->app_key_set = app_key_is_set();
}

/* ======================================================================== */
/* Debug / bench helpers (ats radio ..., #118, CONFIG_SHELL)                */
/* ======================================================================== */

void app_radio_p2p_forget_pairing(void)
{
	(void)pairing_clear();
}

void app_radio_p2p_rejoin(void)
{
	/* Same gates as app_radio_p2p_start() -- the shell must not be a way
	 * around them (a JoinRequest sealed under an all-zero app_key is
	 * forgeable by anyone; see app_key_is_set()). */
	if (!app_key_is_set()) {
		LOG_ERR("P2P rejoin refused: radio_appkey is all-zero (device unprovisioned)");
		return;
	}
	if (!addr_is_valid()) {
		LOG_ERR("P2P rejoin refused: radio_deveui gives no valid address");
		return;
	}
	if (!phy_is_supported()) {
		LOG_ERR("P2P rejoin refused: p2p-modulation fsk is not supported yet");
		return;
	}

	/* Explicit operator-forced fresh join: boot-window policy, not self-heal. */
	start_join_episode(false);
}

#if defined(CONFIG_SHELL)

int app_radio_p2p_unjoin(void)
{
	int ret = pairing_clear();

	if (ret) {
		return ret;
	}
	LOG_INF("P2P pairing cleared; reboot required");
	return 0;
}

void app_radio_p2p_debug_drop_acks(uint32_t count)
{
	m_debug_drop_acks = count;
	LOG_WRN("Debug: forcing next %u ACK(s) to appear dropped", count);
}

/* Runs on the radio work queue, same as a real send (app_compose.c's "solely on
 * the radio work queue" invariant -- see app_ats.c's `ats radio compose` for the
 * LoRaWAN side of the same rule). Previews the frame under the CURRENT fcnt
 * WITHOUT advancing it, so a dry-run can never desync the real data-plane
 * sequence with the peer. */
static void debug_compose_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct p2p_compose_result *res = &m_debug_compose_result;

	if (m_link_state != P2P_LINK_PAIRED) {
		res->ret = -ENOTCONN;
		return;
	}

	uint8_t pt[TWR_PAYLOAD_MAX];
	size_t body_len = 0;

	res->ret = app_compose_budget(&pt[P2P_ENV_LEN], P2P_MAX_BODY, &body_len, &res->more,
				      P2P_MAX_BODY);
	if (res->ret) {
		return;
	}
	if (body_len == 0) {
		/* Nothing to report -- mirror app_radio's own `len == 0` skip, so
		 * the preview never shows a frame the real send path would not
		 * actually transmit. */
		res->frame_len = 0;
		return;
	}

	const struct twr_hdr h = {
		.type = TWR_TYPE_DATA,
		.flags = TWR_FLAG_CONFIRMED,
		.src = node_addr(),
		.dest = m_net_id,
		.counter = MAX(m_fcnt, 1U), /* the counter the next frame takes */
	};

	pt[0] = P2P_ENV_DATA;
	pt[1] = P2P_PORT_TELEMETRY;
	res->ret = twr_seal(m_session_key, &h, pt, P2P_ENV_LEN + body_len, res->frame,
			    sizeof(res->frame), &res->frame_len);
}

int app_radio_p2p_debug_compose(uint8_t *out, size_t out_size, size_t *out_len, bool *more)
{
	struct p2p_compose_result *res = &m_debug_compose_result;

	int ret = k_work_submit_to_queue(app_radio_work_q(), &m_debug_compose_work);

	if (ret < 0) {
		return ret;
	}

	struct k_work_sync sync;

	k_work_flush(&m_debug_compose_work, &sync);

	if (res->ret) {
		return res->ret;
	}
	if (res->frame_len > out_size) {
		return -ENOMEM;
	}

	memcpy(out, res->frame, res->frame_len);
	*out_len = res->frame_len;
	*more = res->more;
	return 0;
}

#endif /* defined(CONFIG_SHELL) */
