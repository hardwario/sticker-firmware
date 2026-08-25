/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_ccm.h"
#include "app_cmd.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_log.h"
#include "app_p2p.h"
#include "app_version.h"
#include "app_wdog.h"

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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_p2p, LOG_LEVEL_INF);

/*
 * Wire frame (raw LoRa has no addressing/MIC/encryption of its own, doc/p2p.md §3):
 *
 *   [ net_id(4 BE) | dev_addr(2 BE) | frame_type(1) | counter(4 BE) ]  11 B header
 *   [ AES-CCM ciphertext (= plaintext length) ]
 *   [ AES-CCM tag (4 B) ]
 *
 * The header is cleartext (a receiver filters foreign traffic by net_id and
 * routes by dev_addr/frame_type before spending a decrypt) and is fed as AAD so
 * it is authenticated by the tag. The body is the exact app_compose payload
 * (version byte + protobuf Telemetry) LoRaWAN would put on fPort 2.
 *
 * net_id/dev_addr are the fixed pre-join value 0 (§5.3's own JoinRequest
 * convention) until a successful join assigns real ones (#118 phase 2,
 * doc/p2p.md §5.3) -- see m_net_id/m_dev_addr below. The body is AES-CCM'd
 * under `join_key` before pairing and the join handshake itself; the derived
 * `session_key` once paired (§4), never a manual config secret.
 *
 * The AES-CCM nonce is counter(4 BE) || dev_addr(2 BE) || frame_type(1) ||
 * direction(1) || zeros(5) = 13 B. `counter` is a strictly increasing 32-bit
 * frame counter persisted with a reservation window (fcnt_*) so a reboot never
 * reuses a (key, nonce) pair. The direction byte separates TX from RX keystream.
 */
#define P2P_HDR_LEN   11
#define P2P_TAG_LEN   4
#define P2P_NONCE_LEN 13
#define P2P_KEY_LEN   16
#define P2P_DIR_TX    0x00
#define P2P_DIR_RX    0x01
#define P2P_LORA_MTU  255
#define P2P_MAX_BODY  (P2P_LORA_MTU - P2P_HDR_LEN - P2P_TAG_LEN) /* 240 */
#define P2P_FRAME_MAX (P2P_HDR_LEN + P2P_MAX_BODY + P2P_TAG_LEN)

/* Pre-join fixed value (§5.3): both header fields are 0 until JoinAccept
 * allocates real ones. m_net_id/m_dev_addr (below) hold the CURRENT value --
 * 0 while UNPAIRED/JOINING, the assigned value once PAIRED. */
#define P2P_PREJOIN_NET_ID   0
#define P2P_PREJOIN_DEV_ADDR 0

/* join_key = AES128-CMAC(secret_key, "HIO-P2P-JOIN" || serial_number(4 BE)),
 * doc/p2p.md §4 -- keys the join handshake itself (JoinRequest/JoinAccept,
 * §5.3) always; the data plane (telemetry/alarm/response/command) uses it
 * only pre-pairing (phase 1 behavior) and switches to the derived
 * session_key once PAIRED (#118 phase 2). */
#define P2P_JOIN_KEY_LABEL "HIO-P2P-JOIN"

/* EU868 1% duty cycle, enforced app-side (raw LoRa bypasses LoRaMac). After a
 * frame of air-time A we must stay off the air for A*(100/1 - 1) = A*99 ms. */
#define P2P_DUTY_CYCLE_PERMILLE 10

#define P2P_FCNT_SUBTREE "p2pfc"
#define P2P_FCNT_KEY     "p2pfc/base"
#define P2P_FCNT_RESERVE 256u

#define P2P_TX_BUF_SIZE    64
#define P2P_TX_QUEUE_DEPTH 2
#define P2P_RX_QUEUE_DEPTH 1

/* Small margin added on top of the exact remaining duty-cycle block when
 * rescheduling a deferred response/alarm frame (#118) -- avoids retrying a
 * few ms too early and getting -EAGAIN again right back. */
#define P2P_TX_RETRY_MARGIN_MS 50

#if defined(CONFIG_WATCHDOG)
#define P2P_HEARTBEAT_PERIOD_SEC 5
#define P2P_HEARTBEAT_TIMEOUT_MS 30000
#endif

/* ---- Join/session persistence (#118 phase 2, doc/p2p.md §5.3) ---- */

#define P2P_JOIN_SUBTREE    "p2pjoin"
#define P2P_JOIN_DNONCE_KEY "p2pjoin/dnonce"
#define P2P_JOIN_STATE_KEY  "p2pjoin/state"
/* net_id(4 BE) | dev_addr(2 BE) | session_key(16) | rx1_delay_s(1) */
#define P2P_JOIN_STATE_LEN  (4 + 2 + P2P_KEY_LEN + 1)

/* JoinRequest body (§5.3): product_type(1) | proto_version(1) |
 * serial_number(4 BE) | fw_version(4). product_type has no existing
 * registry in this codebase yet (single-product today) -- 1 = STICKER, a
 * placeholder pending the central's actual product-type schema (#118
 * follow-up; doc/p2p.md §5.3 cites claiming_process.md §11's identity
 * envelope for the intended generalization). */
#define P2P_PRODUCT_TYPE_STICKER 1
#define P2P_JOIN_REQ_BODY_LEN    10

/* JoinAccept body (§5.3): net_id(4 BE) | dev_addr(2 BE) | central_nonce(4 BE)
 * | rx1_delay_s(1) | reserved(4) -- reserved is the v2 data-channel
 * assignment hook (§11), unused/ignored today. */
#define P2P_JOIN_ACCEPT_BODY_LEN 15

/* Boot-trigger join window (§5.2): an unpaired device retries for at most
 * this long after boot, then goes idle until the next boot or an NFC
 * `p2p_join` (not yet wired) -- caps the worst-case radio-retry drain for a
 * device that never finds a gateway. */
#define P2P_JOIN_BOOT_WINDOW_MS (120 * 1000)

/* Retry cadence for an unanswered JoinRequest. doc/p2p.md §5.3 specifies
 * "jittered, duty-cycle-aware backoff" without exact numbers: retry as soon
 * as the duty cycle clears (the dominant wait at SF10 -- tens of seconds),
 * plus this jitter so devices booting together don't collide on retry. */
#define P2P_JOIN_RETRY_JITTER_MS 2000

/* RX1 window (§6, reused for JoinAccept per §5.3): opened this many ms
 * before the nominal rx1_delay deadline to absorb node-side timing error
 * (crystal drift, work-queue scheduling jitter), sized generously against
 * the gateway-side ±10 ms design ceiling (proximos-v2#20). All
 * debug-shell-overridable (not yet wired) for bench sweeping without a full
 * re-join, same idiom as `ats radio lc`.
 *
 * HW finding (#118 phase 2 HIL): this driver's lora_recv() has NO hardware
 * symbol-timeout -- SetRxConfig always runs continuous RX
 * (sx12xx_lora_recv(), loramac-node/sx12xx_common.c) and the "timeout" is a
 * pure k_poll() software deadline that ABORTS an in-flight reception unless
 * RxDone is already firing. So the window can't just be sized to catch a
 * preamble (that's how a HW-symbol-timeout Class-A RX1 works, which is NOT
 * this driver) -- it must stay open for the *whole* expected frame's
 * time-on-air, or a real JoinAccept/Ack that starts right on time still gets
 * killed mid-reception. P2P_RX1_WINDOW_SYMBOLS is now only the
 * preamble-catch/open-timing-slop budget; frame_toa_ms() of the EXPECTED
 * frame's length is added on top (p2p_rx1_timeout_ms()).
 *
 * HW finding #2 (#118 phase 2 HIL, §6 Ack testing): tx_end_ms is captured
 * AFTER lora_send() returns, not at the actual TxDone instant -- lora_send()
 * is a blocking call, but there is a small driver-return latency between
 * the real end of the on-air transmission and our code resuming. That
 * latency shows up as OUR window opening slightly later than it should
 * relative to the sender's actual TxDone, which can marginally clip the
 * start of an on-time Ack (observed as an intermittent miss on an
 * on-time-per-the-sender's-own-radio-trace Ack). Bumped from 8 to 25 ms as
 * a quick app-level absorption of that latency; the precise fix (anchor
 * tx_end_ms on a TxDone IRQ/callback instead of the blocking call's return)
 * would need driver-level changes, tracked as a follow-up, not done here. */
#define P2P_RX1_OPEN_MARGIN_MS     25
#define P2P_RX1_WINDOW_SYMBOLS     12
#define P2P_RX1_TRAILING_MARGIN_MS 40
#define P2P_RX1_DELAY_DEFAULT_S    1

/* Confirmed uplink (§6): the Ack (0xFA) body is a single flags byte (bit 0:
 * downlink pending, read-and-logged only -- 0x56 dispatch stays out of scope,
 * same decision as §5.3). */
#define P2P_ACK_BODY_LEN 1

/* Unacknowledged uplinks retransmit the SAME counter (byte-identical frame)
 * up to this many times (§6) -- interpreted as retries AFTER the first send
 * (so up to 1 + P2P_ACK_MAX_RETRIES total transmissions), matching doc/p2p.md
 * §6's "retransmit ... up to 3 times". */
#define P2P_ACK_MAX_RETRIES 3

/* Jitter added to a retry's wait, on top of any remaining duty-cycle block. */
#define P2P_ACK_RETRY_JITTER_MS 1000

/* HW-informed finding (#118 phase 2 HIL): a single MAX-size telemetry
 * frame's duty-cycle block can be ~227 s (240 B body, SF10/BW125 --
 * frame_toa_ms(255) ~=2296 ms, block = air*99); a REAL SF10 send's block is
 * routinely ~39-45 s even for smaller frames (measured on the bench). An
 * earlier design capped how long a retry would wait for duty-cycle
 * clearance and gave up past the cap -- but any workable cap short enough to
 * be safe on a shared work queue is *always* shorter than a real SF10 duty-
 * cycle block, making "retry up to 3 times" silently never retry in
 * practice (found via HIL, not reasoning -- the whole point of testing on
 * real silicon). Fixed by making the wait itself ASYNCHRONOUS instead of
 * capped: schedule_ack_retry() reschedules a dedicated work item
 * (m_ack_retry_work) for whenever the duty cycle actually clears, however
 * long that is, rather than blocking m_work_q with a k_sleep(). No cap
 * needed because nothing blocks while waiting -- see send_confirmed(). */

static const struct device *const m_lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));

/* Shallower than app_lrw.c's own m_work_q (4096 B, sized for LoRaMac's deep call
 * stacks): this queue's handlers only do raw lora_send()/lora_config(), AES-CCM
 * (app_ccm) and app_compose_budget() (whose Telemetry frame is a module static,
 * not stack-allocated), so 2048 B (the pre-#265 LoRaWAN default) is ample. */
static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
static struct k_work_q m_work_q;
static struct k_work m_send_work;           /* compose + send telemetry */
static struct k_work_delayable m_tx_work;   /* drain response/alarm queue, retries on -EAGAIN */
static struct k_work_delayable m_join_work; /* JoinRequest attempt + retry (#118 phase 2) */
#if defined(CONFIG_SHELL)
static struct k_work m_rx_work; /* drain received frames (listen) */

/* ats radio ... debug/bench helpers (#118) */
static uint32_t m_debug_drop_acks; /* ats radio ack_drop: remaining forced Ack drops */

struct p2p_compose_result {
	uint8_t frame[P2P_FRAME_MAX];
	size_t frame_len;
	bool more;
	int ret; /* build_frame()/app_compose_budget() error, 0 on success */
};

static struct p2p_compose_result m_debug_compose_result; /* ats radio compose dry-run */
static struct k_work m_debug_compose_work;
static void debug_compose_work_handler(struct k_work *work); /* defined near EOF */
#endif

#if defined(CONFIG_WATCHDOG)
static int m_wdog_channel = -1;
static struct k_work_delayable m_heartbeat_work;
#endif

static bool m_started;
static bool m_listening;
static int64_t m_dc_blocked_until; /* uptime ms; no TX before this */
static void (*m_ready_cb)(void);

/* --- Persistent frame counter (nonce uniqueness across reboots) --- */
static uint32_t m_fcnt;          /* next counter value to use */
static uint32_t m_fcnt_reserved; /* persisted high-water; m_fcnt < this is durable */

/* --- Join/session state (#118 phase 2, doc/p2p.md §5.3) --- */
/* enum p2p_link_state is public (app_p2p.h) so app_p2p_get_info() can report it. */
static enum p2p_link_state m_link_state = P2P_LINK_UNPAIRED;
static uint32_t m_net_id;   /* 0 (pre-join) until PAIRED */
static uint16_t m_dev_addr; /* 0 (pre-join) until PAIRED */
static uint8_t m_session_key[P2P_KEY_LEN];
static uint8_t m_rx1_delay_s = P2P_RX1_DELAY_DEFAULT_S;
static uint32_t m_dev_nonce;      /* next JoinRequest counter; persisted, device lifetime */
static int64_t m_join_started_at; /* uptime ms; start of the current boot join window */

struct p2p_tx_msg {
	uint8_t type;
	uint16_t len;
	uint8_t buf[P2P_TX_BUF_SIZE];
};

K_MSGQ_DEFINE(m_tx_msgq, sizeof(struct p2p_tx_msg), P2P_TX_QUEUE_DEPTH, 4);

/* A frame that tx_frame() bounced with -EAGAIN (duty-cycle blocked): already
 * dequeued from m_tx_msgq, so it must be retried explicitly instead of
 * dropped, or the response/alarm frame is lost outright (#118 -- confirmed
 * on real HW: the default alarm-limit batch window is shorter than a single
 * SF10 frame's duty-cycle block, so every alarm detail frame was silently
 * dropped). Single slot: tx_work_handler() only ever defers the one message
 * it was mid-send on, then stops draining until that retry succeeds. */
static struct p2p_tx_msg m_tx_deferred;
static bool m_tx_deferred_valid;

/* §6 Ack-retry state: a frame that was TRANSMITTED but not yet Acked,
 * awaiting an asynchronous retry once the duty cycle clears (see
 * schedule_ack_retry()). Distinct from m_tx_deferred above -- that one is a
 * frame that never even got a first TX attempt (duty-cycle blocked before
 * sending); this one already went out at least once and is waiting on a
 * confirmation retry.
 *
 * A QUEUE, not a single slot (#118 phase 2 HIL finding): telemetry (its own
 * send_work_handler loop) and the alarm/response queue (tx_work_handler) are
 * independent call paths that can each have one frame awaiting an Ack retry
 * at the same time (e.g. an alarm firing during the same window a telemetry
 * chunk went unacked) -- a single slot would silently drop whichever one
 * schedule_ack_retry() overwrote. Depth 3 covers telemetry + alarm +
 * response each having one in flight; the shared duty-cycle gate still only
 * lets one physical TX happen at a time, so ack_retry_work_handler() drains
 * this FIFO one entry per fire, re-queueing (to the tail, so multiple
 * pending frames get serviced round-robin, not starved) whichever one still
 * needs another attempt. */
#define P2P_ACK_RETRY_QUEUE_DEPTH 3

struct p2p_ack_retry_state {
	uint8_t frame_type;
	uint8_t body[P2P_MAX_BODY];
	uint16_t body_len;
	uint32_t counter;
	int attempt; /* retries already sent; 0 on the first scheduled retry */
};

static struct k_work_delayable m_ack_retry_work;

K_MSGQ_DEFINE(m_ack_retry_msgq, sizeof(struct p2p_ack_retry_state), P2P_ACK_RETRY_QUEUE_DEPTH, 4);

#if defined(CONFIG_SHELL)
struct p2p_rx_msg {
	uint16_t len;
	int16_t rssi;
	int8_t snr;
	uint8_t buf[P2P_FRAME_MAX];
};

K_MSGQ_DEFINE(m_rx_msgq, sizeof(struct p2p_rx_msg), P2P_RX_QUEUE_DEPTH, 4);
#endif

/* ======================================================================== */
/* Key derivation                                                            */
/* ======================================================================== */

/* join_key = AES128-CMAC(secret_key, "HIO-P2P-JOIN" || serial_number(4 BE))
 * (doc/p2p.md §4, NIST SP 800-38B / RFC 4493) -- a proper PRF with
 * domain-separated subkeys, not the bare one-block AES-ECB PRF phase 1
 * shipped (a crypto review flagged the plain ECB construction as
 * under-specified; CMAC needs only the same AES-ECB forward primitive
 * underneath, so this costs no new backend, #118 phase 2). Breaking change
 * vs. phase 1's join_key, accepted pre-ship (phase 1 was bench-only, no
 * central to migrate). Derived fresh on demand (cheap) rather than cached --
 * secret_key can change (#299) without this module needing to know. */
static void derive_join_key(uint8_t out[P2P_KEY_LEN])
{
	uint8_t block[16] = {0};
	/* "HIO-P2P-JOIN" (12 B) + serial_number (4 B) = 16 B, exactly one block. */
	size_t label_len = strlen(P2P_JOIN_KEY_LABEL);

	memcpy(block, P2P_JOIN_KEY_LABEL, label_len);
	sys_put_be32(g_app_config.serial_number, &block[label_len]);

	(void)app_ccm_cmac(g_app_config.secret_key, block, sizeof(block), out);
}

/* session_key = AES128-CMAC(join_key, 0x01 || dev_nonce(4 BE) ||
 * central_nonce(4 BE) || serial_number(4 BE) || zeros(3)) (doc/p2p.md §4) --
 * derived once per successful join, keyed under join_key so a session_key
 * leak never exposes join_key. Rotates every re-join (fresh dev_nonce and
 * central_nonce each time), which is also why resetting the frame counter to
 * 0 on every new pairing is safe: CCM's nonce-uniqueness requirement is on
 * the (key, nonce) pair, not the nonce alone (confirmed w/ #118 phase 2
 * review). */
static void derive_session_key(uint32_t dev_nonce, uint32_t central_nonce, uint8_t out[P2P_KEY_LEN])
{
	uint8_t join_key[P2P_KEY_LEN];

	derive_join_key(join_key);

	uint8_t block[16] = {0};

	block[0] = 0x01;
	sys_put_be32(dev_nonce, &block[1]);
	sys_put_be32(central_nonce, &block[5]);
	sys_put_be32(g_app_config.serial_number, &block[9]);
	/* block[13..15] = zeros(3), already zero-initialized. */

	(void)app_ccm_cmac(join_key, block, sizeof(block), out);
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

static void fcnt_reserve(uint32_t high_water)
{
	m_fcnt_reserved = high_water;
	int ret = settings_save_one(P2P_FCNT_KEY, &high_water, sizeof(high_water));

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save_one(p2pfc)", ret);
	}
}

static uint32_t fcnt_next(void)
{
	uint32_t c = m_fcnt++;

	if (m_fcnt >= m_fcnt_reserved) {
		fcnt_reserve(m_fcnt + P2P_FCNT_RESERVE);
	}
	return c;
}

/* ======================================================================== */
/* Join/session persistence (#118 phase 2, doc/p2p.md §5.3)                 */
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
			m_net_id = sys_get_be32(&buf[0]);
			m_dev_addr = sys_get_be16(&buf[4]);
			memcpy(m_session_key, &buf[6], P2P_KEY_LEN);
			m_rx1_delay_s = buf[6 + P2P_KEY_LEN];
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
 * cheap enough; no need for p2pfc's reservation-window trick (confirmed
 * #118 phase 2 review). Never resets across pairings -- it is the central's
 * JoinRequest replay-protection handle (§5.3), so re-joining must never
 * present a dev_nonce the central could have already seen. */
static void dnonce_persist(uint32_t v)
{
	m_dev_nonce = v;
	int ret = settings_save_one(P2P_JOIN_DNONCE_KEY, &v, sizeof(v));

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save_one(p2pjoin/dnonce)", ret);
	}
}

/* Persist a successful JoinAccept's pairing state and switch the module to
 * PAIRED. Resets the data-plane frame counter to 0 -- safe because
 * session_key is fresh (see derive_session_key()'s comment) and keeps the
 * on-air counter values small. */
static void pairing_persist(uint32_t net_id, uint16_t dev_addr,
			    const uint8_t session_key[P2P_KEY_LEN], uint8_t rx1_delay_s)
{
	uint8_t buf[P2P_JOIN_STATE_LEN];

	sys_put_be32(net_id, &buf[0]);
	sys_put_be16(dev_addr, &buf[4]);
	memcpy(&buf[6], session_key, P2P_KEY_LEN);
	buf[6 + P2P_KEY_LEN] = rx1_delay_s;

	int ret = settings_save_one(P2P_JOIN_STATE_KEY, buf, sizeof(buf));

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_save_one(p2pjoin/state)", ret);
		return;
	}

	m_net_id = net_id;
	m_dev_addr = dev_addr;
	memcpy(m_session_key, session_key, P2P_KEY_LEN);
	m_rx1_delay_s = rx1_delay_s;
	m_link_state = P2P_LINK_PAIRED;

	m_fcnt = 0;
	fcnt_reserve(P2P_FCNT_RESERVE);
}

/* ======================================================================== */
/* Radio configuration                                                      */
/* ======================================================================== */

/* Fixed in phase 1 (kept out of config to fit the dual-stack flash budget):
 * the common P2P defaults of 125 kHz bandwidth and 4/5 coding rate. The
 * receiver must match these. Frequency / SF / TX power stay configurable. */
#define P2P_BANDWIDTH    BW_125_KHZ
#define P2P_BANDWIDTH_HZ 125000u
#define P2P_CODING_RATE  CR_4_5
#define P2P_CR_DENOM     1 /* CR_4_5 contributes (CR_DENOM + 4) symbols in ToA */

static int sf_from_cfg(void)
{
	return CLAMP(g_app_config.p2p_spreading_factor, SF_6, SF_12);
}

static void build_modem_config(struct lora_modem_config *c, bool tx)
{
	memset(c, 0, sizeof(*c));
	c->frequency = g_app_config.p2p_frequency;
	c->bandwidth = P2P_BANDWIDTH;
	c->datarate = (enum lora_datarate)sf_from_cfg();
	c->coding_rate = P2P_CODING_RATE;
	c->preamble_len = 8;
	c->tx_power = (int8_t)g_app_config.p2p_tx_power;
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

/* LoRa time-on-air in ms (Semtech AN1200.13), integer-only to avoid pulling in
 * the soft-float/libm code on this Cortex-M4-no-FPU part. Used for app-side
 * duty-cycle accounting (preamble_len fixed at 8, explicit header). */
static uint32_t frame_toa_ms(uint8_t payload_len)
{
	int sf = sf_from_cfg();
	uint32_t bw = P2P_BANDWIDTH_HZ;
	int cr = P2P_CR_DENOM;
	int de = (sf >= 11 && bw == 125000) ? 1 : 0;

	/* Symbol period in microseconds: Tsym = 2^SF / BW. */
	uint64_t tsym_us = ((uint64_t)(1u << sf) * 1000000ULL) / bw;

	/* Payload symbol count: 8 + max(ceil((8*PL - 4*SF + 28 + 16) / (4*(SF-2*DE)))
	 * * (CR+4), 0). H = 0 (explicit header). */
	int32_t num = 8 * (int32_t)payload_len - 4 * sf + 28 + 16;
	int32_t den = 4 * (sf - 2 * de);
	int32_t extra = 0;

	if (num > 0) {
		extra = ((num + den - 1) / den) * (cr + 4); /* ceil division */
	}
	uint32_t n_sym = 8 + (uint32_t)(extra > 0 ? extra : 0);

	/* Preamble = (8 + 4.25) symbols = 49/4 symbols. */
	uint64_t t_preamble_us = tsym_us * 49 / 4;
	uint64_t t_payload_us = tsym_us * n_sym;

	return (uint32_t)((t_preamble_us + t_payload_us + 500) / 1000);
}

/* Preamble-catch / open-timing-slop budget in ms, P2P_RX1_WINDOW_SYMBOLS
 * symbols at the live SF/BW -- only ONE component of the real lora_recv()
 * timeout (see p2p_rx1_timeout_ms() and the #define comment above: this
 * driver has no HW symbol-timeout, so this alone is NOT a valid window). */
static uint32_t rx1_preamble_catch_ms(void)
{
	int sf = sf_from_cfg();
	uint64_t tsym_us = ((uint64_t)(1u << sf) * 1000000ULL) / P2P_BANDWIDTH_HZ;

	return (uint32_t)((tsym_us * P2P_RX1_WINDOW_SYMBOLS + 500) / 1000);
}

/* Full lora_recv() timeout for an RX1 wait expecting a frame of
 * `expected_frame_len` bytes: preamble-catch budget + that frame's whole
 * time-on-air + a trailing margin (#118 phase 2 HW finding -- this driver's
 * "timeout" aborts an in-flight reception, so it must outlast the entire
 * expected frame, not just its preamble). */
static uint32_t p2p_rx1_timeout_ms(uint8_t expected_frame_len)
{
	return rx1_preamble_catch_ms() + frame_toa_ms(expected_frame_len) +
	       P2P_RX1_TRAILING_MARGIN_MS;
}

/* Open a bounded RX window `rx1_delay_s - open_margin_ms` after `tx_end_ms`
 * (uptime ms), sleeping until it opens then blocking on lora_recv() for
 * p2p_rx1_timeout_ms(expected_frame_len) -- shared by JoinAccept and §6's
 * data-plane Ack (each passes its own expected frame length). Runs on
 * m_work_q like everything else here; the whole call blocks that queue for
 * up to ~rx1_delay_s (dominant) + the frame's ToA (#118 phase 2 HW finding,
 * see p2p_rx1_timeout_ms()) -- an explicit watchdog feed covers this (and
 * any caller's own backoff sleep) since the periodic heartbeat_work_handler
 * can't run until this returns (single-threaded m_work_q). Restores TX radio
 * config before returning either way. Returns the received length (>=0) or a
 * negative errno (notably a timeout if nothing arrived within the window). */
static int p2p_rx_window(int64_t tx_end_ms, uint8_t rx1_delay_s, uint8_t expected_frame_len,
			 uint8_t *buf, size_t buf_size, int16_t *rssi, int8_t *snr)
{
	int64_t open_at = tx_end_ms + (int64_t)rx1_delay_s * 1000 - P2P_RX1_OPEN_MARGIN_MS;
	int64_t sleep_ms = open_at - k_uptime_get();

#if defined(CONFIG_WATCHDOG)
	app_wdog_ping(m_wdog_channel);
#endif

	if (sleep_ms > 0) {
		k_sleep(K_MSEC(sleep_ms));
	}

	int ret = radio_configure(false);

	if (ret) {
		return ret;
	}

	ret = lora_recv(m_lora_dev, buf, (uint8_t)MIN(buf_size, 255),
			K_MSEC(p2p_rx1_timeout_ms(expected_frame_len)), rssi, snr);

	(void)radio_configure(true);

	return ret;
}

/* ======================================================================== */
/* Frame TX                                                                 */
/* ======================================================================== */

static void build_nonce(uint8_t nonce[P2P_NONCE_LEN], uint32_t counter, uint16_t dev_addr,
			uint8_t frame_type, uint8_t dir)
{
	memset(nonce, 0, P2P_NONCE_LEN);
	sys_put_be32(counter, &nonce[0]);
	sys_put_be16(dev_addr, &nonce[4]);
	nonce[6] = frame_type;
	nonce[7] = dir;
}

static bool duty_cycle_blocked(void)
{
	return k_uptime_get() < m_dc_blocked_until;
}

/* Build header+encrypt one frame into `frame` (>= P2P_HDR_LEN + body_len +
 * P2P_TAG_LEN bytes). Pure -- no radio/queue/counter side effects -- shared
 * by the real TX path (tx_frame_at) and the `ats radio compose` dry-run
 * (debug_compose_work_handler). */
static int build_frame(uint8_t frame_type, const uint8_t *body, size_t body_len, uint32_t counter,
		       uint8_t *frame)
{
	sys_put_be32(m_net_id, &frame[0]);
	sys_put_be16(m_dev_addr, &frame[4]);
	frame[6] = frame_type;
	sys_put_be32(counter, &frame[7]);

	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, counter, m_dev_addr, frame_type, P2P_DIR_TX);

	int ret = app_ccm_encrypt_and_tag(m_session_key, nonce, P2P_NONCE_LEN, /* AAD */ frame,
					  P2P_HDR_LEN, body, body_len, &frame[P2P_HDR_LEN],
					  &frame[P2P_HDR_LEN + body_len], P2P_TAG_LEN);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_ccm_encrypt_and_tag", ret);
	}
	return ret;
}

/* Frame + encrypt + transmit one body under an EXPLICIT counter (no
 * fcnt_next() call) -- shared by the first send (tx_frame(), below, picks a
 * fresh counter) and a §6 Ack retry (send_confirmed(), which must resend a
 * byte-identical frame under the SAME counter: CCM under a fixed (key,
 * nonce, plaintext) is deterministic, so reusing the counter alone
 * reproduces the exact same ciphertext, no cached buffer needed). Caller
 * must have already checked !duty_cycle_blocked(). Returns 0 or errno; on
 * success reports the send-completion time via `tx_end_ms` (uptime ms, for
 * the caller's RX1/Ack wait) and charges the duty-cycle budget. */
static int tx_frame_at(uint8_t frame_type, const uint8_t *body, size_t body_len, uint32_t counter,
		       int64_t *tx_end_ms)
{
	if (m_listening) {
		LOG_WRN("TX skipped: radio in listen mode");
		return -EBUSY;
	}
	if (body_len > P2P_MAX_BODY) {
		LOG_ERR("Body %zu B over P2P budget %d B", body_len, P2P_MAX_BODY);
		return -EMSGSIZE;
	}
	if (m_link_state != P2P_LINK_PAIRED) {
		/* Callers gate on app_p2p_is_ready(), so this should never happen --
		 * defensive only (no session_key to encrypt the data plane under
		 * yet). */
		LOG_ERR("TX skipped: not paired (#118 phase 2)");
		return -ENOTCONN;
	}

	uint8_t frame[P2P_FRAME_MAX];

	int ret = build_frame(frame_type, body, body_len, counter, frame);

	if (ret) {
		return ret;
	}

	size_t wire_len = P2P_HDR_LEN + body_len + P2P_TAG_LEN;

	ret = lora_send(m_lora_dev, frame, wire_len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lora_send", ret);
		return ret;
	}

	int64_t end = k_uptime_get();
	uint32_t air = frame_toa_ms((uint8_t)wire_len);

	/* Charge the air-time against the 1% budget. */
	m_dc_blocked_until = end + (int64_t)air * (1000 / P2P_DUTY_CYCLE_PERMILLE - 1);

	LOG_INF("TX type %u, %zu B (counter %u, %u ms air)", frame_type, wire_len, counter, air);

	*tx_end_ms = end;
	return 0;
}

/* Frame + encrypt + transmit one body under a FRESH counter. Returns 0,
 * -EAGAIN (duty cycle), or errno; on success reports the counter used and
 * the send-completion time via the out-params (see tx_frame_at()). */
static int tx_frame(uint8_t frame_type, const uint8_t *body, size_t body_len, uint32_t *counter_out,
		    int64_t *tx_end_ms)
{
	if (duty_cycle_blocked()) {
		LOG_WRN("TX duty-cycle blocked for %lld ms", m_dc_blocked_until - k_uptime_get());
		return -EAGAIN;
	}

	uint32_t counter = fcnt_next();
	int ret = tx_frame_at(frame_type, body, body_len, counter, tx_end_ms);

	if (ret == 0) {
		*counter_out = counter;
	}
	return ret;
}

/* Wait for and validate an Ack (0xFA) for `counter` in the RX1 window after
 * `tx_end_ms` (doc/p2p.md §6): header must match (net_id/dev_addr/frame_type/
 * counter echo), then AES-CCM decrypt under session_key, direction=RX (it is
 * a downlink). The 1-byte flags body's bit 0 (downlink pending) is read and
 * logged only -- 0x56 dispatch stays out of scope, same decision as §5.3.
 * Returns true iff a valid, matching Ack was received. */
static bool recv_ack(uint32_t counter, int64_t tx_end_ms)
{
#if defined(CONFIG_SHELL)
	if (m_debug_drop_acks > 0) {
		m_debug_drop_acks--;
		LOG_WRN("Debug: dropping Ack (counter %u), %u drop(s) left", counter,
			m_debug_drop_acks);
		return false;
	}
#endif /* defined(CONFIG_SHELL) */

	uint8_t buf[P2P_FRAME_MAX];
	int16_t rssi;
	int8_t snr;
	size_t want = P2P_HDR_LEN + P2P_ACK_BODY_LEN + P2P_TAG_LEN;

	int len = p2p_rx_window(tx_end_ms, m_rx1_delay_s, (uint8_t)want, buf, sizeof(buf), &rssi,
				&snr);

	if (len < 0 || (size_t)len != want) {
		return false;
	}

	uint32_t net_id_hdr = sys_get_be32(&buf[0]);
	uint16_t dev_addr_hdr = sys_get_be16(&buf[4]);
	uint8_t frame_type = buf[6];
	uint32_t ctr = sys_get_be32(&buf[7]);

	if (net_id_hdr != m_net_id || dev_addr_hdr != m_dev_addr ||
	    frame_type != APP_P2P_FRAME_ACK || ctr != counter) {
		return false;
	}

	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, ctr, m_dev_addr, frame_type, P2P_DIR_RX);

	uint8_t flags;
	int ret = app_ccm_auth_decrypt(m_session_key, nonce, P2P_NONCE_LEN, buf, P2P_HDR_LEN,
				       &buf[P2P_HDR_LEN], P2P_ACK_BODY_LEN,
				       &buf[P2P_HDR_LEN + P2P_ACK_BODY_LEN], P2P_TAG_LEN, &flags);
	if (ret) {
		LOG_WRN("Ack auth failed (counter %u)", counter);
		return false;
	}

	LOG_INF("Ack received (counter %u)%s", counter,
		(flags & 0x01) ? " [downlink pending]" : "");
	return true;
}

/* (Re)schedule m_ack_retry_work for whenever the duty cycle clears (plus
 * jitter), if the queue has anything pending -- a no-op otherwise. Called
 * after every enqueue/dequeue so the timer always reflects the current
 * queue state and duty-cycle estimate. */
static void reschedule_ack_retry_work(void)
{
	if (k_msgq_num_used_get(&m_ack_retry_msgq) == 0) {
		return;
	}

	int64_t wait_ms = m_dc_blocked_until - k_uptime_get();

	if (wait_ms < 0) {
		wait_ms = 0;
	}

	uint32_t jitter = sys_rand32_get() % P2P_ACK_RETRY_JITTER_MS;

	k_work_reschedule_for_queue(&m_work_q, &m_ack_retry_work, K_MSEC(wait_ms + jitter));
}

/* Queue an asynchronous Ack retry for `counter` (doc/p2p.md §6) -- NOT a
 * blocking wait, so no cap is needed (see the P2P_ACK_RETRY_JITTER_MS
 * #define comment for why an earlier capped-sleep design was wrong).
 * `attempt` is how many retries have already been sent (0 for the first). */
static void schedule_ack_retry(uint8_t frame_type, const uint8_t *body, size_t body_len,
			       uint32_t counter, int attempt)
{
	struct p2p_ack_retry_state st = {
		.frame_type = frame_type,
		.body_len = (uint16_t)body_len,
		.counter = counter,
		.attempt = attempt,
	};

	memcpy(st.body, body, body_len);

	if (k_msgq_put(&m_ack_retry_msgq, &st, K_NO_WAIT) != 0) {
		LOG_WRN("Ack retry queue full; giving up on counter %u", counter);
		return;
	}

	reschedule_ack_retry_work();
}

static void ack_retry_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct p2p_ack_retry_state st;

	if (k_msgq_peek(&m_ack_retry_msgq, &st) != 0) {
		return; /* queue empty */
	}

	if (duty_cycle_blocked()) {
		reschedule_ack_retry_work();
		return;
	}

	/* Committed to sending st now -- actually dequeue it (peek() above only
	 * looked, so a still-blocked duty cycle above leaves it in place). */
	(void)k_msgq_get(&m_ack_retry_msgq, &st, K_NO_WAIT);

	int64_t tx_end;
	int ret = tx_frame_at(st.frame_type, st.body, st.body_len, st.counter, &tx_end);

	if (ret) {
		LOG_WRN("Ack retry (counter %u) send failed: %d", st.counter, ret);
	} else {
		LOG_INF("Uplink retry %d/%d sent (counter %u)", st.attempt + 1, P2P_ACK_MAX_RETRIES,
			st.counter);

		if (!recv_ack(st.counter, tx_end)) {
			if (st.attempt + 1 < P2P_ACK_MAX_RETRIES) {
				st.attempt++;
				/* Re-queue at the TAIL (not retried in place): with more
				 * than one frame pending, this services them round-robin
				 * instead of one starving the others. */
				if (k_msgq_put(&m_ack_retry_msgq, &st, K_NO_WAIT) != 0) {
					LOG_WRN("Ack retry queue full re-queueing counter %u; "
						"giving up",
						st.counter);
				}
			} else {
				LOG_WRN("Uplink counter %u unacked after %d retries; giving "
					"up",
					st.counter, P2P_ACK_MAX_RETRIES);
			}
		}
	}

	/* Whether this entry is done, gave up, or got re-queued, other frames
	 * may still be waiting -- keep the timer aligned with the queue. */
	reschedule_ack_retry_work();
}

/* Confirmed uplink (§6): send one frame and wait once for its Ack. If
 * unacknowledged, hand off to schedule_ack_retry() instead of blocking here
 * -- returns 0 either way (the frame WAS transmitted; confirmation, if a
 * retry is needed, continues asynchronously on m_ack_retry_work). Callers
 * (send_work_handler/tx_work_handler) move on immediately rather than
 * waiting for the eventual outcome. Returns -EAGAIN only if the FIRST send
 * itself was duty-cycle blocked (unchanged pre-existing semantics, same as
 * tx_frame()), or a hard errno from that first send. */
static int send_confirmed(uint8_t frame_type, const uint8_t *body, size_t body_len)
{
	uint32_t counter;
	int64_t tx_end;

	int ret = tx_frame(frame_type, body, body_len, &counter, &tx_end);

	if (ret) {
		return ret;
	}

	if (!recv_ack(counter, tx_end)) {
		schedule_ack_retry(frame_type, body, body_len, counter, 0);
	}

	return 0;
}

static void send_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (g_app_config.calibration) {
		return;
	}

	uint8_t buf[P2P_MAX_BODY];
	size_t len = 0;
	bool more = false;

	do {
		int ret = app_compose_budget(buf, sizeof(buf), &len, &more, P2P_MAX_BODY);

		if (ret == -EAGAIN) {
			LOG_DBG("Telemetry budget unavailable, skipping TX");
			return;
		}
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("app_compose_budget", ret);
			return;
		}
		if (len == 0) {
			return; /* nothing to report */
		}
		if (send_confirmed(APP_P2P_FRAME_TELEMETRY, buf, len) != 0) {
			return; /* duty cycle / unacked / radio error — drop the rest of the
				 * snapshot */
		}
	} while (more);
}

static void tx_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct p2p_tx_msg msg;

	if (m_tx_deferred_valid) {
		msg = m_tx_deferred;
		m_tx_deferred_valid = false;
	} else if (k_msgq_get(&m_tx_msgq, &msg, K_NO_WAIT) != 0) {
		return;
	}

	for (;;) {
		int ret = send_confirmed(msg.type, msg.buf, msg.len);

		if (ret == -EAGAIN) {
			/* Only the FIRST send attempt returns -EAGAIN (send_confirmed()'s
			 * own Ack-retry loop never re-raises it, see its cap). Stash and
			 * retry this exact frame once the duty-cycle window clears,
			 * instead of dropping it — see m_tx_deferred's comment. Stop
			 * draining the rest of the queue until this one is sent, so
			 * frames stay in order. */
			m_tx_deferred = msg;
			m_tx_deferred_valid = true;

			int64_t delay_ms = m_dc_blocked_until - k_uptime_get();

			if (delay_ms < 0) {
				delay_ms = 0;
			}
			k_work_reschedule_for_queue(&m_work_q, dwork,
						    K_MSEC(delay_ms + P2P_TX_RETRY_MARGIN_MS));
			return;
		}

		if (k_msgq_get(&m_tx_msgq, &msg, K_NO_WAIT) != 0) {
			return;
		}
	}
}

/* ======================================================================== */
/* Frame RX (reference receiver / diagnostics, CONFIG_SHELL)                */
/* ======================================================================== */

#if defined(CONFIG_SHELL)
static void rx_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct p2p_rx_msg msg;

	while (k_msgq_get(&m_rx_msgq, &msg, K_NO_WAIT) == 0) {
		if (msg.len < P2P_HDR_LEN + P2P_TAG_LEN) {
			LOG_WRN("RX runt frame (%u B)", msg.len);
			continue;
		}

		uint32_t net_id = sys_get_be32(&msg.buf[0]);
		uint16_t dev_addr = sys_get_be16(&msg.buf[4]);
		uint8_t frame_type = msg.buf[6];
		uint32_t counter = sys_get_be32(&msg.buf[7]);

		if (net_id != m_net_id) {
			LOG_DBG("RX foreign net_id %u; ignored", net_id);
			continue;
		}

		size_t ct_len = msg.len - P2P_HDR_LEN - P2P_TAG_LEN;
		uint8_t pt[P2P_MAX_BODY];
		uint8_t nonce[P2P_NONCE_LEN];

		/* Sender's frames are TX-direction; a receiver decrypts with the
		 * sender's nonce, so use the addr/type that came in the header. */
		memset(nonce, 0, sizeof(nonce));
		sys_put_be32(counter, &nonce[0]);
		sys_put_be16(dev_addr, &nonce[4]);
		nonce[6] = frame_type;
		nonce[7] = P2P_DIR_TX;

		/* Tries join_key only -- a PAIRED device's own data-plane traffic is
		 * under session_key instead and will correctly fail here (#118
		 * phase 2; this listen mode is diagnostic/bench-only and was never
		 * extended to try both keys, out of the reviewed A-D join-handshake
		 * scope). */
		uint8_t key[P2P_KEY_LEN];

		derive_join_key(key);

		int ret = app_ccm_auth_decrypt(key, nonce, P2P_NONCE_LEN, /* AAD */ msg.buf,
					       P2P_HDR_LEN, &msg.buf[P2P_HDR_LEN], ct_len,
					       &msg.buf[P2P_HDR_LEN + ct_len], P2P_TAG_LEN, pt);
		if (ret) {
			LOG_WRN("RX auth failed (addr %u, type %u)", dev_addr, frame_type);
			continue;
		}

		LOG_INF("RX type %u from addr %u (RSSI %d dBm, SNR %d dB, %zu B)", frame_type,
			dev_addr, msg.rssi, msg.snr, ct_len);
		LOG_HEXDUMP_INF(pt, ct_len, "P2P body:");

		/* #118 phase 1: COMMAND frames are logged only, not dispatched to
		 * app_cmd -- that arrives with phase 2's real, anti-replay-protected
		 * (session-keyed) channel; dispatching against a bare join_key-only
		 * decrypt here would have no replay protection at all (no per-session
		 * counter high-water yet, just the monotonic TX-side fcnt). */
		if (frame_type == APP_P2P_FRAME_COMMAND) {
			LOG_INF("COMMAND frame received (not dispatched, phase 1)");
		}
	}
}

static void p2p_recv_cb(const struct device *dev, uint8_t *data, uint16_t size, int16_t rssi,
			int8_t snr, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	struct p2p_rx_msg msg;

	msg.len = MIN(size, (uint16_t)sizeof(msg.buf));
	msg.rssi = rssi;
	msg.snr = snr;
	memcpy(msg.buf, data, msg.len);

	if (k_msgq_put(&m_rx_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("RX queue full; dropping frame");
		return;
	}
	k_work_submit_to_queue(&m_work_q, &m_rx_work);
}

int app_p2p_listen(bool enable)
{
	if (enable == m_listening) {
		return 0;
	}

	if (enable) {
		int ret = radio_configure(false);

		if (ret) {
			return ret;
		}
		ret = lora_recv_async(m_lora_dev, p2p_recv_cb, NULL);
		if (ret) {
			LOG_ERR_CALL_FAILED_INT("lora_recv_async", ret);
			return ret;
		}
		m_listening = true;
		LOG_INF("P2P listen: ON (net_id=%u)", m_net_id);
	} else {
		(void)lora_recv_async(m_lora_dev, NULL, NULL);
		m_listening = false;
		(void)radio_configure(true);
		LOG_INF("P2P listen: OFF");
	}
	return 0;
}
#endif /* defined(CONFIG_SHELL) */

/* ======================================================================== */
/* Join handshake (#118 phase 2, doc/p2p.md §5.3)                           */
/* ======================================================================== */

static void mark_ready(void)
{
	m_started = true;
	if (m_ready_cb) {
		m_ready_cb();
	}
}

/* Send one JoinRequest (doc/p2p.md §5.3): header net_id=0/dev_addr=0,
 * counter=dev_nonce; body product_type|proto_version|serial_be32|fw_version
 * sent as AAD with an EMPTY ciphertext (the body is authenticated, not
 * encrypted -- there is nothing secret in it, it is the central's lookup
 * key). Persists the advanced dev_nonce BEFORE sending: once a JoinRequest
 * *could* have reached the central, that nonce value must never be reused,
 * even if the TX or the round-trip afterward fails. Returns 0 (with
 * `*used_nonce`/`*tx_end_ms` set) or -EAGAIN (duty-cycle blocked) or an
 * errno. */
static int send_join_request(uint32_t *used_nonce, int64_t *tx_end_ms)
{
	int64_t now = k_uptime_get();

	if (now < m_dc_blocked_until) {
		return -EAGAIN;
	}

	uint32_t nonce_val = m_dev_nonce;

	dnonce_persist(nonce_val + 1);

	uint8_t frame[P2P_HDR_LEN + P2P_JOIN_REQ_BODY_LEN + P2P_TAG_LEN];

	sys_put_be32(P2P_PREJOIN_NET_ID, &frame[0]);
	sys_put_be16(P2P_PREJOIN_DEV_ADDR, &frame[4]);
	frame[6] = APP_P2P_FRAME_JOIN_REQUEST;
	sys_put_be32(nonce_val, &frame[7]);

	uint8_t *body = &frame[P2P_HDR_LEN];

	body[0] = P2P_PRODUCT_TYPE_STICKER;
	body[1] = APP_PROTO_VERSION;
	sys_put_be32(g_app_config.serial_number, &body[2]);
	body[6] = APP_VERSION_MAJOR;
	body[7] = APP_VERSION_MINOR;
	body[8] = APP_VERSION_PATCH;
	body[9] = 0; /* reserved */

	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, nonce_val, P2P_PREJOIN_DEV_ADDR, APP_P2P_FRAME_JOIN_REQUEST, P2P_DIR_TX);

	uint8_t key[P2P_KEY_LEN];

	derive_join_key(key);

	/* Whole body is AAD; ciphertext is empty (doc/p2p.md §5.3). */
	int ret = app_ccm_encrypt_and_tag(key, nonce, P2P_NONCE_LEN, frame,
					  sizeof(frame) - P2P_TAG_LEN, NULL, 0, NULL,
					  &frame[sizeof(frame) - P2P_TAG_LEN], P2P_TAG_LEN);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_ccm_encrypt_and_tag", ret);
		return ret;
	}

	ret = lora_send(m_lora_dev, frame, sizeof(frame));
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lora_send", ret);
		return ret;
	}

	int64_t end = k_uptime_get();
	uint32_t air = frame_toa_ms(sizeof(frame));

	m_dc_blocked_until = end + (int64_t)air * (1000 / P2P_DUTY_CYCLE_PERMILLE - 1);

	LOG_INF("JoinRequest sent (dev_nonce %u, %u ms air)", nonce_val, air);

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
	int16_t rssi;
	int8_t snr;
	size_t want = P2P_HDR_LEN + P2P_JOIN_ACCEPT_BODY_LEN + P2P_TAG_LEN;

	int len = p2p_rx_window(tx_end_ms, P2P_RX1_DELAY_DEFAULT_S, (uint8_t)want, buf, sizeof(buf),
				&rssi, &snr);

	if (len < 0) {
		return len;
	}

	if ((size_t)len != want) {
		LOG_WRN("JoinAccept: unexpected length %d (want %zu)", len, want);
		return -EBADMSG;
	}

	uint32_t net_id_hdr = sys_get_be32(&buf[0]);
	uint16_t dev_addr_hdr = sys_get_be16(&buf[4]);
	uint8_t frame_type = buf[6];
	uint32_t counter = sys_get_be32(&buf[7]);

	if (net_id_hdr != P2P_PREJOIN_NET_ID || dev_addr_hdr != P2P_PREJOIN_DEV_ADDR ||
	    frame_type != APP_P2P_FRAME_JOIN_ACCEPT || counter != dev_nonce) {
		LOG_WRN("JoinAccept: header mismatch (type %u, ctr %u, want ctr %u)", frame_type,
			counter, dev_nonce);
		return -EBADMSG;
	}

	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, counter, P2P_PREJOIN_DEV_ADDR, frame_type, P2P_DIR_RX);

	uint8_t key[P2P_KEY_LEN];

	derive_join_key(key);

	uint8_t body[P2P_JOIN_ACCEPT_BODY_LEN];
	int ret = app_ccm_auth_decrypt(key, nonce, P2P_NONCE_LEN, buf, P2P_HDR_LEN,
				       &buf[P2P_HDR_LEN], P2P_JOIN_ACCEPT_BODY_LEN,
				       &buf[P2P_HDR_LEN + P2P_JOIN_ACCEPT_BODY_LEN], P2P_TAG_LEN,
				       body);
	if (ret) {
		LOG_WRN("JoinAccept: auth failed");
		return ret;
	}

	uint32_t net_id = sys_get_be32(&body[0]);
	uint16_t dev_addr = sys_get_be16(&body[4]);
	uint32_t central_nonce = sys_get_be32(&body[6]);
	uint8_t rx1_delay_s = body[10];
	/* body[11..14] = reserved (v2 data-channel assignment hook, §11), unused. */

	uint8_t session_key[P2P_KEY_LEN];

	derive_session_key(dev_nonce, central_nonce, session_key);
	pairing_persist(net_id, dev_addr, session_key, rx1_delay_s);

	LOG_INF("Joined: net_id=%u dev_addr=%u rx1_delay=%us (RSSI %d dBm, SNR %d dB)", net_id,
		dev_addr, rx1_delay_s, rssi, snr);
	return 0;
}

static void join_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	if (m_link_state != P2P_LINK_JOINING) {
		return; /* paired (or reverted) while a retry was already in flight */
	}

	if (k_uptime_get() - m_join_started_at >= P2P_JOIN_BOOT_WINDOW_MS) {
		LOG_WRN("P2P join: boot window (%d s) expired without a JoinAccept; giving up "
			"until next boot/trigger",
			P2P_JOIN_BOOT_WINDOW_MS / 1000);
		m_link_state = P2P_LINK_UNPAIRED;
		return;
	}

	uint32_t used_nonce;
	int64_t tx_end;
	int ret = send_join_request(&used_nonce, &tx_end);

	if (ret == 0) {
		ret = recv_join_accept(used_nonce, tx_end);
		if (ret == 0) {
			mark_ready();
			return; /* paired; no more retries */
		}
		LOG_INF("JoinAccept not received/invalid (%d); retrying", ret);
	} else if (ret != -EAGAIN) {
		LOG_ERR_CALL_FAILED_INT("send_join_request", ret);
	}

	int64_t wait_ms = (ret == -EAGAIN) ? (m_dc_blocked_until - k_uptime_get()) : 0;

	if (wait_ms < 0) {
		wait_ms = 0;
	}

	uint32_t jitter = sys_rand32_get() % P2P_JOIN_RETRY_JITTER_MS;

	k_work_reschedule_for_queue(&m_work_q, dwork, K_MSEC(wait_ms + jitter));
}

/* ======================================================================== */
/* Watchdog heartbeat (mirrors app_lrw.c's m_work_q liveness pattern)        */
/* ======================================================================== */

#if defined(CONFIG_WATCHDOG)
static void heartbeat_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	app_wdog_ping(m_wdog_channel);
	k_work_schedule_for_queue(&m_work_q, &m_heartbeat_work,
				  K_SECONDS(P2P_HEARTBEAT_PERIOD_SEC));
}
#endif /* defined(CONFIG_WATCHDOG) */

/* ======================================================================== */
/* Public API                                                                */
/* ======================================================================== */

int app_p2p_init(void)
{
	if (!device_is_ready(m_lora_dev)) {
		LOG_ERR("LoRa device not ready");
		return -ENODEV;
	}

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

	ret = radio_configure(true);
	if (ret) {
		return ret;
	}

	k_work_queue_init(&m_work_q);
	k_work_queue_start(&m_work_q, m_work_stack, K_THREAD_STACK_SIZEOF(m_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_work_init(&m_send_work, send_work_handler);
	k_work_init_delayable(&m_tx_work, tx_work_handler);
	k_work_init_delayable(&m_join_work, join_work_handler);
	k_work_init_delayable(&m_ack_retry_work, ack_retry_work_handler);
#if defined(CONFIG_SHELL)
	k_work_init(&m_rx_work, rx_work_handler);
	k_work_init(&m_debug_compose_work, debug_compose_work_handler);
#endif

#if defined(CONFIG_WATCHDOG)
	m_wdog_channel = app_wdog_register(P2P_HEARTBEAT_TIMEOUT_MS);
	if (m_wdog_channel < 0) {
		LOG_ERR_CALL_FAILED_INT("app_wdog_register", m_wdog_channel);
	}
	k_work_init_delayable(&m_heartbeat_work, heartbeat_work_handler);
	k_work_schedule_for_queue(&m_work_q, &m_heartbeat_work, K_NO_WAIT);
#endif /* defined(CONFIG_WATCHDOG) */

	app_compose_reset();

	return 0;
}

void app_p2p_start(void)
{
	if (m_link_state == P2P_LINK_PAIRED) {
		/* Persisted pairing from a prior boot: no re-join needed (§7 --
		 * a session survives normal power cycles). */
		mark_ready();
		return;
	}

	/* Unpaired: kick off the boot-window join handshake (#118 phase 2,
	 * §5.2). app_p2p_is_ready() only goes true once JoinAccept lands
	 * (mark_ready(), called from join_work_handler()). */
	m_link_state = P2P_LINK_JOINING;
	m_join_started_at = k_uptime_get();
	k_work_schedule_for_queue(&m_work_q, &m_join_work, K_NO_WAIT);
}

bool app_p2p_is_ready(void)
{
	return m_started;
}

uint8_t app_p2p_get_max_payload(void)
{
	return P2P_MAX_BODY;
}

void app_p2p_send_telemetry(void)
{
	k_work_submit_to_queue(&m_work_q, &m_send_work);
}

static int queue_frame(uint8_t type, const uint8_t *buf, size_t len)
{
	if (!buf || len == 0) {
		return -EINVAL;
	}
	if (len > P2P_TX_BUF_SIZE) {
		LOG_ERR("Frame %zu B over queue slot %d B", len, P2P_TX_BUF_SIZE);
		return -EMSGSIZE;
	}

	struct p2p_tx_msg msg = {.type = type, .len = (uint16_t)len};

	memcpy(msg.buf, buf, len);
	if (k_msgq_put(&m_tx_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("TX queue full (type %u); dropping", type);
		return -ENOMEM;
	}
	/* If a retry is already scheduled (deferred frame waiting out a duty-cycle
	 * block), this is a no-op — the queue drains in order once that retry
	 * fires, same as an immediate submit would have. */
	k_work_schedule_for_queue(&m_work_q, &m_tx_work, K_NO_WAIT);
	return 0;
}

int app_p2p_queue_response(uint8_t port, const uint8_t *buf, size_t len)
{
	ARG_UNUSED(port); /* P2P has no fPort; frame_type carries the equivalent */
	return queue_frame(APP_P2P_FRAME_RESPONSE, buf, len);
}

int app_p2p_send_alarm(const uint8_t *buf, size_t len)
{
	return queue_frame(APP_P2P_FRAME_ALARM, buf, len);
}

void app_p2p_register_ready_cb(void (*cb)(void))
{
	m_ready_cb = cb;
}

void app_p2p_suspend(void)
{
	/* Nothing queued survives a poweroff -- the work queue itself (including
	 * any pending m_join_work retry, #118 phase 2) is torn down with the
	 * reboot that follows deep-sleep entry, same as app_lrw_suspend() relies
	 * on for its own timers. No-op today; kept as an explicit facade hook. */
}

void app_p2p_get_info(struct app_p2p_info *info)
{
	info->link_state = m_link_state;
	info->net_id = m_net_id;
	info->dev_addr = m_dev_addr;
	info->rx1_delay_s = m_rx1_delay_s;
	info->fcnt = m_fcnt;
	info->dev_nonce = m_dev_nonce;
	info->ack_retry_pending = k_msgq_num_used_get(&m_ack_retry_msgq);
}

/* ======================================================================== */
/* Debug / bench helpers (ats radio ..., #118, CONFIG_SHELL)                */
/* ======================================================================== */

#if defined(CONFIG_SHELL)

void app_p2p_rejoin(void)
{
	m_link_state = P2P_LINK_JOINING;
	m_join_started_at = k_uptime_get();
	k_work_schedule_for_queue(&m_work_q, &m_join_work, K_NO_WAIT);
}

int app_p2p_unjoin(void)
{
	int ret = settings_delete(P2P_JOIN_STATE_KEY);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("settings_delete(p2pjoin/state)", ret);
		return ret;
	}
	LOG_INF("P2P pairing cleared; reboot required");
	return 0;
}

void app_p2p_debug_set_rx1_delay(uint8_t rx1_delay_s)
{
	m_rx1_delay_s = rx1_delay_s;
	LOG_WRN("Debug: rx1_delay override -> %u s (not persisted)", rx1_delay_s);
}

void app_p2p_debug_drop_acks(uint32_t count)
{
	m_debug_drop_acks = count;
	LOG_WRN("Debug: forcing next %u Ack(s) to appear dropped", count);
}

/* Runs on m_work_q, same as a real send (app_compose.c's "solely on
 * m_work_q" invariant -- see app_ats.c's `ats radio compose` for the
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

	uint8_t body[P2P_MAX_BODY];
	size_t body_len = 0;

	res->ret = app_compose_budget(body, sizeof(body), &body_len, &res->more, P2P_MAX_BODY);
	if (res->ret) {
		return;
	}
	if (body_len == 0) {
		/* Nothing to report -- mirror send_work_handler()'s own `len == 0`
		 * skip, so the preview never shows a frame the real send path
		 * would not actually transmit. */
		res->frame_len = 0;
		return;
	}

	res->ret = build_frame(APP_P2P_FRAME_TELEMETRY, body, body_len, m_fcnt, res->frame);
	res->frame_len = P2P_HDR_LEN + body_len + P2P_TAG_LEN;
}

int app_p2p_debug_compose(uint8_t *out, size_t out_size, size_t *out_len, bool *more)
{
	struct p2p_compose_result *res = &m_debug_compose_result;

	int ret = k_work_submit_to_queue(&m_work_q, &m_debug_compose_work);

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
