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
#include "app_radio_lrw.h"
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
 * Wire frame (raw LoRa has no addressing/MIC/encryption of its own, doc/p2p.md §3):
 *
 * Data plane (telemetry/alarm/response/ack, always PAIRED -- see
 * tx_frame_at()'s P2P_LINK_PAIRED gate):
 *
 *   [ net_id(4 BE) | dev_addr(2 BE) | frame_type(1) | FCtrl(1) | counter(4 BE) ]  12 B
 *   header (FCtrl since decision #22: P2P_FCTRL_* in app_radio_p2p.h)
 *   [ AES-CCM ciphertext (= plaintext length) ]
 *   [ AES-CCM tag (4 B) ]
 *
 * The header is cleartext (a receiver filters foreign traffic by net_id and
 * routes by dev_addr/frame_type before spending a decrypt) and is fed as AAD so
 * it is authenticated by the tag. The body is the exact app_compose payload
 * (version byte + protobuf Telemetry) LoRaWAN would put on fPort 2. Keyed
 * under `session_key` (§4) once a successful join assigns it -- never a
 * manual config secret.
 *
 * The AES-CCM nonce is counter(4 BE) || dev_addr(2 BE) || frame_type(1) ||
 * direction(1) || zeros(5) = 13 B. `counter` is a strictly increasing 32-bit
 * frame counter persisted with a reservation window (fcnt_*) so a reboot never
 * reuses a (key, nonce) pair. The direction byte separates TX from RX keystream.
 *
 * Join handshake (JoinRequest/JoinAccept, §5.3; net_id=dev_addr=0 always,
 * see P2P_PREJOIN_NET_ID/P2P_PREJOIN_DEV_ADDR below): the SAME 12 B header, FCtrl 0,
 * but the body is CLEARTEXT (not AES-CCM'd -- neither frame carries an
 * actual secret: JoinRequest is the device's own public identity,
 * JoinAccept is the assigned net_id/dev_addr/central_nonce; only `app_key`
 * is secret and it is never transmitted) followed by a full 16 B plain
 * AES-CMAC tag (P2P_JOIN_TAG_LEN, NOT a truncated 4 B CCM tag) over a
 * domain-separation label || header || body -- see
 * send_join_request()/recv_join_accept() and the
 * P2P_JOIN_TAG_LABEL/P2P_JOINACCEPT_TAG_LABEL comment below. Deliberately a
 * simpler, different construction than the data plane's AES-CCM above
 * (#118 phase 2 revision, proximos-v2 MR!7 §7) precisely because there is
 * nothing to encrypt in either handshake frame, only to authenticate.
 */
/* P2P_HDR_LEN / P2P_TAG_LEN / P2P_NONCE_LEN / P2P_KEY_LEN / P2P_DIR_TX /
 * P2P_DIR_RX / P2P_LORA_MTU / P2P_MAX_BODY / P2P_FRAME_MAX are in app_radio_p2p.h
 * (shared with tests/p2p_logic). */

/* Pre-join fixed value (§5.3): both header fields are 0 until JoinAccept
 * allocates real ones. m_net_id/m_dev_addr (below) hold the CURRENT value --
 * 0 while UNPAIRED/JOINING, the assigned value once PAIRED. */
#define P2P_PREJOIN_NET_ID   0
#define P2P_PREJOIN_DEV_ADDR 0

/* The STICKER's existing LoRaWAN OTAA AppKey (g_app_config.lrw_appkey) is the
 * ONLY root secret for the whole P2P transport -- there is no separate
 * join_key (#118 phase 2 revision, per proximos-v2 MR!7 §7). The central
 * already knows app_key from the device's OTAA/claim flow, so nothing new
 * needs provisioning; app_config's `secret_key` (the NFC command channel's
 * key, #299) is NOT used in P2P at all any more.
 *
 * JoinRequest/JoinAccept authenticate with a full 16 B plain AES-CMAC tag
 * (P2P_JOIN_TAG_LEN) over label || header || (cleartext) body -- see
 * send_join_request()/recv_join_accept(). Each frame type gets its own
 * domain-separation label so a tag computed for one can never be replayed
 * as the other's. This exact construction (label || header || body, plain
 * CMAC, no CCM/nonce) is a NEW proposal introduced with this revision --
 * there is no pre-existing spec for JoinAccept's authentication to match
 * (kept symmetric with JoinRequest's for the same "nothing secret in the
 * body" reason). See doc/p2p.md §4/§5.3. */
#define P2P_JOIN_TAG_LABEL       "HIO-P2P-JOIN" /* 12 B -- JoinRequest tag */
#define P2P_JOINACCEPT_TAG_LABEL "HIO-P2P-ACC"  /* 11 B -- JoinAccept tag */
/* P2P_JOIN_TAG_LEN (the full CMAC output; NOT P2P_TAG_LEN), and the join body
 * and frame lengths, live in app_radio_p2p.h -- tests/p2p_logic shares them. */

/* session_key = AES128-CMAC(app_key, "HIO-P2P-SES" || 0x01 || dev_nonce(4 BE)
 * || central_nonce(4 BE) || dev_eui(8 B, MSB-first) || zero-pad to 32 B),
 * doc/p2p.md §4 -- keys the data plane (telemetry/alarm/response/ack) once
 * PAIRED, derived directly from app_key (see above), never from a bare
 * config secret. Label(11 B) + 0x01(1 B) + 2*4 B nonces + 8 B dev_eui = 28 B,
 * zero-padded to 32 B (two full CMAC blocks) -- app_ccm_cmac() already
 * handles multi-block messages (its RFC4493 Mlen-40/64 KAT vectors in
 * tests/ccm), so no new primitive is needed, just the wider buffer.
 *
 * The last field was serial_number(4 BE) until #417 / GitLab #73 made the
 * DevEUI the node's identity on the air. Both ends must agree byte for byte:
 * if they do not, the join still succeeds and every data frame after it fails
 * to decrypt with nothing in the log to explain it (the #118 failure class).
 * Pinned against the central and the JS decoder by the shared fixture
 * tests/ccm/p2p_join_kat.json. */
#define P2P_SESSION_KEY_LABEL "HIO-P2P-SES"

/*
 * EU868 1% duty cycle, enforced app-side (raw LoRa bypasses LoRaMac's own duty
 * enforcement) with an exact sliding-hour ledger (B2, decision D1).
 *
 * Two models preceded it. The first blocked the radio for air*99 ms after
 * EVERY frame, so an alarm queued behind a long telemetry frame waited
 * minutes. The second (PR #408) was a token bucket refilling at 1% of wall
 * time and capped at the full hourly allowance: that fixed the latency and
 * held the long-run average at 1%, but a node idle for an hour could then
 * burst the whole 36 s of air at once, which means a worst-case SLIDING hour
 * of ~2%. Amortised compliance, not compliance.
 *
 * The ledger records (end time, air-time) per transmission and admits a frame
 * only if the air already inside the trailing hour plus this frame fits the
 * allowance -- so every sliding hour sums to <= 1%, with no burst hole to
 * argue about in a certification review. It keeps the bucket's latency
 * behaviour: a frame goes the moment there is room, rather than serving a
 * fixed post-frame penalty.
 *
 * Cost is 384 B of RAM (P2P_DUTY_LEDGER_ENTRIES entries) and a bounded frame
 * count per hour -- see the header, and doc/p2p.md §6/§11 for both.
 */
/* P2P_DUTY_WINDOW_MS / P2P_DUTY_BUDGET_MS / P2P_DUTY_LEDGER_ENTRIES are in
 * app_radio_p2p.h (shared with tests). */

#define P2P_FCNT_SUBTREE "p2pfc"
#define P2P_FCNT_KEY     "p2pfc/base"
#define P2P_FCNT_RESERVE 256u

#define P2P_RX_QUEUE_DEPTH 1

/* Small margin added on top of the exact remaining duty-cycle block when a
 * frame waits out the duty cycle (#118) -- avoids retrying a few ms too early
 * and getting -EAGAIN again right back. */
#define P2P_TX_RETRY_MARGIN_MS 50

/* ---- Join/session persistence (#118 phase 2, doc/p2p.md §5.3) ---- */

#define P2P_JOIN_SUBTREE    "p2pjoin"
#define P2P_JOIN_DNONCE_KEY "p2pjoin/dnonce"
#define P2P_JOIN_STATE_KEY  "p2pjoin/state"
/* net_id(4 BE) | dev_addr(2 BE) | session_key(16) | rx1_delay_s(1) |
 * tx_power_dbm(1, 0 = none)
 *
 * The trailing tx_power byte is new in this release (D3). join_settings_set()
 * accepts only records of exactly this length, so a node upgraded across the
 * change reads its old 23 B record as invalid, boots UNPAIRED and re-joins
 * once -- deliberate, and harmless pre-deployment: the re-join is what fetches
 * the assignment the record now has room for. doc/p2p.md §7. */
#define P2P_JOIN_STATE_LEN  (4 + 2 + P2P_KEY_LEN + 1 + 1)

/* JoinRequest body (§5.3): product_type(1) | proto_version(1) |
 * dev_eui(8, MSB-first) | fw_version(4) -- 14 B, so 42 B on the air.
 *
 * The identity field was serial_number(4 BE) until #417 / GitLab #73: the
 * serial is off the air entirely now, and stays only as the number printed on
 * the device. The DevEUI is written MSB-first, exactly as `config lrw-deveui`
 * prints it and as the hex string reads -- deliberately NOT LoRaWAN's LSB-first
 * on-air order, which LoRaMac applies internally for OTAA joins. Reusing that
 * serialization here would be the #118 failure class in its purest form
 * (decision D1).
 *
 * product_type has no existing registry in this codebase yet (single-product
 * today) -- 1 = STICKER, a placeholder pending the central's actual
 * product-type schema (#118 follow-up; doc/p2p.md §5.3 cites
 * claiming_process.md §11's identity envelope for the intended
 * generalization). */
#define P2P_PRODUCT_TYPE_STICKER 1

/* JoinAccept body (§5.3, in app_radio_p2p.h): net_id(4 BE) | dev_addr(2 BE) |
 * central_nonce(4 BE) | rx1_delay_s(1) | reserved(4) -- reserved is the v2
 * data-channel assignment hook (§11), unused/ignored today. Unchanged by
 * #417: the JoinAccept carries no identity field at all. */

/* P2P_JOIN_BOOT_WINDOW_MS (§5.2, the unpaired retry deadline) and
 * P2P_JOIN_RETRY_JITTER_MS (§5.3, so devices booting together don't collide on
 * retry) live in app_radio_p2p.h -- tests/p2p_logic checks the wait against them. */

/* SF range the join sweep tries when the configured SF finds no Hub (B-2). The
 * SF is network-wide and the Hub owns it, so a Hub that changed it leaves every
 * node deaf until the node re-discovers it. SF6 is configurable but excluded:
 * it needs the implicit-header mode this PHY does not use. */
#define P2P_JOIN_LAST_RESORT_MS (24LL * 60 * 60 * 1000)
#define P2P_JOIN_SWEEP_SF_MIN   7
#define P2P_JOIN_SWEEP_SF_MAX   12

/* Link supervision (decision #22 §3.4) is app_radio's, one machine for both
 * radios (doc/plan/460 F2). A failed link check is a CONFIRMED frame with no
 * Ack (or 0x56) after its P2P_ACK_MAX_RETRIES retries; any authenticated
 * downlink is a success. WARNING's rung here is the TX power: each failed
 * check steps it up towards p2p-tx-power (warning_tx_power_step()). A rejoin
 * is a self-healing re-join on the configured SF which, unlike the
 * never-paired boot join (§5.2), skips the 120 s fast phase and starts
 * straight on the common exponential backoff (app_radio_rejoin_backoff_ms()),
 * to keep the duty budget and battery sane over a long outage. */
#define P2P_WARNING_TX_POWER_STEP_DB 2

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
 * would need driver-level changes, tracked as a follow-up, not done here.
 *
 * HW finding #3 (F-P2P-2, ProXimos Hub bench 2026-09-26): the window must also
 * absorb a FIXED lateness of the central, which the SF-scaled terms do not
 * cover at low SF. Measured against a Northbridge central: its Ack starts
 * 65..78 ms after the nominal RX1 instant (aim margin + TX-start latency +
 * RxDone stamp latency), and the window's timeout itself only starts ~22 ms
 * after the formula (radio wake from sleep, 5 ms TCXO start-up, two
 * SetRxConfig calls). With a 40 ms trailing margin the end slack was +15 ms
 * at SF9, +5 ms at SF8 and -12 ms at SF7 -- every SF7 Ack was cut off
 * mid-reception ("Receive timeout" + retries), and a mid-packet abort can
 * also wedge the radio (F-P2P-3). A fixed 120 ms trailing margin keeps SF7
 * working for a central up to ~100 ms late, independent of its aim, for about
 * 80 ms more receiver-on per confirmed uplink (a few mA for 0.08 s). */
#define P2P_RX1_OPEN_MARGIN_MS     25
#define P2P_RX1_WINDOW_SYMBOLS     12
#define P2P_RX1_TRAILING_MARGIN_MS 120
#define P2P_RX1_DELAY_DEFAULT_S    1
/* A JoinAccept's rx1_delay_s outside 1..P2P_RX1_DELAY_MAX_S is refused (review
 * of #400): p2p_rx_window() sleeps it on the radio work queue, and one near the 30 s
 * watchdog would reset the node on every uplink -- persisted, a boot loop. */
#define P2P_RX1_DELAY_MAX_S        15

/* Confirmed uplink (§6): the Ack (0xFA) body (B1/B5, PR #408, matches the
 * central in proximos-v2 MR!30). Base body is flags(1) | rssi(i8) | snr(i8) --
 * the RSSI/SNR the central measured on the uplink being acknowledged (a free
 * link-quality sample, TOWER's pattern). An optional 4-byte big-endian Unix
 * time tail rides when flags bit 1 is set (B5 clock sync). The frame is
 * self-describing: the node derives the body length from the received frame
 * length, so no wire version is needed (P2P is pre-deployment). */
/* P2P_ACK_FLAG_PENDING / P2P_ACK_FLAG_TIME / P2P_ACK_BODY_BASE_LEN /
 * P2P_ACK_TIME_LEN / P2P_ACK_BODY_MAX_LEN are in app_radio_p2p.h (shared with the
 * pure p2p_parse_ack_body() helper and tests/p2p_logic). */

/* Unacknowledged uplinks retransmit the SAME counter (byte-identical frame)
 * up to this many times (§6) -- interpreted as retries AFTER the first send
 * (so up to 1 + P2P_ACK_MAX_RETRIES total transmissions), matching doc/p2p.md
 * §6's "retransmit ... up to 3 times". */
#define P2P_ACK_MAX_RETRIES 3

/* RX1 window size when no longer downlink is announced (decision #22 §3.3, O7
 * interim): a 0x56 of up to this many bytes on the air may arrive in the RX1 of
 * any uplink. Covers the fully-extended Ack (24 B) too. */
#define P2P_RX1_CMD_INTERIM_LEN 64

/* Retry backoff (decision #22 §3.2, Hynek 2026-09-27): retry n (1-based) waits
 * a random 1..2^n s on top of any remaining duty-cycle block, like LoRaWAN's
 * ACK_TIMEOUT (2 +/- 1 s). The fixed ~2.3 s rhythm it replaces (1 s gap + RX1 +
 * 0..1 s) locked two nodes rebooted together ~1 s apart, each transmitting into
 * the other's RX1 until both gave up (F-P2P-4). */
#define P2P_ACK_RETRY_BACKOFF_MIN_MS 1000

/* HW-informed finding (#118 phase 2 HIL): under the OLD "block for air*99 ms
 * after every frame" model a single MAX-size telemetry frame blocked the
 * radio for ~227 s (240 B body, SF10/BW125 -- frame_toa_ms(255) ~=2296 ms);
 * a real SF10 send blocked ~39-45 s even for smaller frames. The token-bucket
 * governor (B2) replaces that fixed post-frame block, so a retry now waits
 * only until enough budget has re-accrued for ITS frame -- but the wait can
 * still be long once the bucket is drained, so the async design below still
 * matters. An earlier design capped how long a retry would wait for duty-cycle
 * clearance and gave up past the cap -- but any workable cap short enough to
 * be safe on a shared work queue is *always* shorter than a real SF10 duty-
 * cycle block, making "retry up to 3 times" silently never retry in
 * practice (found via HIL, not reasoning -- the whole point of testing on
 * real silicon). Fixed by making the wait itself ASYNCHRONOUS instead of
 * capped: schedule_ack_retry() reschedules a dedicated work item
 * (m_ack_retry_work) for whenever the duty cycle actually clears, however
 * long that is, rather than blocking the radio work queue with a k_sleep(). No cap
 * needed because nothing blocks while waiting -- see send_confirmed(). */

static const struct device *const m_lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));

/* All work runs on the radio work queue, app_radio_work_q() (doc/plan/439 T2a). Its
 * 4096 B stack is sized for this module's deepest path, a 0x56 command:
 * recv_ack() -> app_radio_downlink() -> app_cmd_handle() and nanopb (app_radio.c). */
static struct k_work_delayable m_join_work; /* JoinRequest attempt + retry (#118 phase 2) */

/* F-P1-1: the central keeps a strict counter high-water, so frames must leave
 * in counter order. Two rules keep them there, as on LoRaWAN (a confirmed
 * uplink's retransmissions finish before the next uplink):
 *  - one confirmed uplink in flight: no fresh-counter frame while an Ack retry
 *    is pending (a newer counter would turn the retry into a "replay");
 *  - P2P_TX_GAP_MS between an Ack window and the next TX, so the central is
 *    back in RX after sending its Ack (a back-to-back frame went unheard). */
#define P2P_TX_GAP_MS 1000
static int64_t m_link_idle_at; /* uptime before which no TX starts */
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

static bool m_started;
/* app_radio_p2p_start() refused to run: lrw_appkey or lrw_deveui is all-zero.
 * Reported as APP_RADIO_STATE_DISABLED. */
static bool m_disabled;
static bool m_listening;
static struct p2p_duty m_duty; /* exact sliding-hour duty ledger (B2/D1) */
static void (*m_ready_cb)(void);

/* --- Persistent frame counter (nonce uniqueness across reboots) --- */
static uint32_t m_fcnt;          /* next counter value to use */
static uint32_t m_fcnt_reserved; /* persisted high-water; m_fcnt < this is durable */

/* --- Join/session state (#118 phase 2, doc/p2p.md §5.3) --- */
/* enum p2p_link_state is public (app_radio_p2p.h) so app_radio_p2p_get_info() can report it. */
static enum p2p_link_state m_link_state = P2P_LINK_UNPAIRED;
static uint32_t m_net_id;   /* 0 (pre-join) until PAIRED */
static uint16_t m_dev_addr; /* 0 (pre-join) until PAIRED */
static uint8_t m_session_key[P2P_KEY_LEN];
static uint8_t m_rx1_delay_s = P2P_RX1_DELAY_DEFAULT_S;
/* D3: TX power the central assigned for this session in JoinAccept
 * reserved[2]. Falls back to g_app_config.p2p_tx_power when unassigned. */
static bool m_session_tx_power_assigned;
static int8_t m_session_tx_power_dbm;
static uint32_t m_dev_nonce;      /* next JoinRequest counter; persisted, device lifetime */
static int64_t m_join_started_at; /* uptime ms; start of the current boot join window */

/* Live spreading factor the radio is tuned to. Seeded from the config at init
 * and at the start of every join episode; the join sweep re-tunes it between
 * attempts, so every radio path (modem config, time-on-air, RX1 window sizing)
 * must read THIS rather than the config, or the two drift apart mid-join.
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

/* Join SF sweep state (B-2). An episode walks passes; a pass is
 * P2P_JOIN_SF_ATTEMPTS sent JoinRequests at p2p_join_sweep_sf(cfg, 0) -- the
 * configured SF -- then one at each further step until the order is exhausted.
 * m_join_episode_fresh makes join_work_handler, not start_join_episode(), do
 * the per-episode seeding: the shell `join` command calls start_join_episode()
 * from its own thread, and every m_sf write has to happen on the radio work queue. */
static uint8_t m_join_sweep_step;  /* sweep step the next JoinRequest uses */
static int64_t m_join_sweep_epoch; /* uptime ms since which no last-resort sweep ran */
static uint8_t m_join_sf_attempts; /* SENT attempts already made at that step */
static bool m_join_episode_fresh;  /* the handler has not opened this episode yet */

/* RadioState (#446): every change of the backoff step is pushed to app_radio,
 * which is where readers take it from. */
static void set_rejoin_attempt(uint32_t n)
{
	m_rejoin_attempt = (uint8_t)MIN(n, UINT8_MAX);
	app_radio_set_join_attempts(m_rejoin_attempt);
}

/* After a transmission: the parameters it went out with (the same precedence
 * as build_modem_config()) and, when paired, the session. */
static void publish_link(void)
{
	app_radio_set_params(m_sf, -1,
			     m_session_tx_power_assigned ? m_session_tx_power_dbm
							 : (int8_t)g_app_config.p2p_tx_power);
	if (m_link_state == P2P_LINK_PAIRED) {
		app_radio_set_session(m_dev_addr, m_fcnt);
	}
}

/* B1: RSSI/SNR the central reported in the last Ack (its measurement of our
 * uplink). m_last_ack_valid is false until the first Ack of this session. */
/* clock_sync (app_radio_p2p_clock_sync()): answer with an Info carrying this
 * seq once the next Ack (and its time tail) has been processed. Set from a
 * command thread, consumed on the radio work queue. */
static atomic_t m_clock_sync_pending;
static atomic_t m_clock_sync_seq;
/* Reports sent CONFIRMED for the pending clock_sync so far (PF-2): at most
 * P2P_CLOCK_SYNC_REPORTS_MAX, so a central that sends no time tail cannot keep
 * every report confirmed; the request then waits for the next link check. */
#define P2P_CLOCK_SYNC_REPORTS_MAX 3
static atomic_t m_clock_sync_reports;
static int8_t m_last_ack_rssi;
static int8_t m_last_ack_snr;
static bool m_last_ack_valid;

/* B4: the last Ack's "downlink pending" flag -- when set, the central will
 * deliver a 0x56 COMMAND in the RX1 window of the NEXT uplink (replacing that
 * uplink's Ack), so that window must be sized for a command frame. */
static bool m_downlink_pending;

/* D2: the on-air length that Ack announced for the pending 0x56, so the next
 * RX1 window is sized exactly instead of for a 255 B worst case. 0 means "not
 * announced" -- either nothing is pending, or the central is still emitting
 * the pre-D2 3/7-byte Ack body; both fall back to P2P_FRAME_MAX. */
static uint8_t m_pending_frame_len;

/* §6 Ack-retry state: a frame that was TRANSMITTED but not yet Acked,
 * awaiting an asynchronous retry once the duty cycle clears (see
 * schedule_ack_retry()). Distinct from a frame app_radio keeps while the duty
 * cycle blocks its first TX attempt; this one already went out at least once
 * and is waiting on a confirmation retry.
 *
 * A QUEUE, not a single slot (#118 phase 2 HIL finding): telemetry and the
 * alarm/response frames, and the history replay, can each have one frame
 * awaiting an Ack retry
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

/* The whole transport is rooted in app_key (see the P2P_JOIN_TAG_LABEL comment
 * above), so an all-zero lrw_appkey is not merely "unset" -- it is a PUBLICLY
 * KNOWN root key. Anything derived under it (both handshake tags and every
 * session_key) is forgeable by anyone in radio range: a forged JoinAccept
 * would pair the node into a hostile network and hand the attacker the same
 * session_key the node computes.
 *
 * It is also a genuinely reachable state, not a theoretical one. All-zero is
 * the config default, so any device set to `radio-mode p2p` before its
 * lrw_appkey was provisioned lands here -- the ordinary case on a bench or a
 * P2P-only build. It is also what a factory_reset leaves behind (lrw_appkey
 * is `persistent: [device_reset]` only and is absent from
 * app_config_factory_reset()'s preserve list, unlike secret_key, which the
 * earlier join_key-rooted design could always fall back on).
 *
 * So the radio must not come up at all until the device is provisioned --
 * see app_radio_p2p_start()/app_radio_p2p_rejoin(). Refusing loudly rather than silently
 * idling follows radio_disabled()'s rule in app_radio_lrw.c (#271/#98): a
 * provisioning problem should surface, not masquerade as a radio that is
 * merely off. */
static bool app_key_is_set(void)
{
	for (size_t i = 0; i < sizeof(g_app_config.lrw_appkey); i++) {
		if (g_app_config.lrw_appkey[i] != 0) {
			return true;
		}
	}
	return false;
}

/* The same rule for lrw_deveui, which #417 / GitLab #73 made load-bearing:
 * the DevEUI is now the central's lookup key on the air AND an input to the
 * session-key KDF, so an all-zero one is not a cosmetic gap.
 *
 * All-zero is a legitimate state today -- it is what an unprovisioned device
 * has, and app_radio_lrw.c treats it as a reason to stay radio-silent rather than an
 * error. Without this guard a P2P node would happily transmit JoinRequests
 * carrying eight zero bytes, which no central can have registered, and the
 * only symptom would be a node that joins forever. Refuse for the same reason
 * and in the same shape as app_key_is_set() above. */
static bool dev_eui_is_set(void)
{
	for (size_t i = 0; i < sizeof(g_app_config.lrw_deveui); i++) {
		if (g_app_config.lrw_deveui[i] != 0) {
			return true;
		}
	}
	return false;
}

/* Constant-time 16 B tag compare (mirrors app_ccm_auth_decrypt()'s own
 * accumulate-the-XOR pattern in app_ccm.c) -- used to verify the plain
 * AES-CMAC tags on JoinRequest/JoinAccept below. A short-circuiting memcmp()
 * would leak how many leading bytes matched via timing; that is exactly the
 * kind of side channel a MAC verification must not have. */
static bool p2p_tag_eq(const uint8_t a[P2P_JOIN_TAG_LEN], const uint8_t b[P2P_JOIN_TAG_LEN])
{
	uint8_t diff = 0;

	for (size_t i = 0; i < P2P_JOIN_TAG_LEN; i++) {
		diff |= a[i] ^ b[i];
	}
	return diff == 0;
}

/* See P2P_SESSION_KEY_LABEL above for the formula and rationale. Label (11 B)
 * + 0x01 (1 B) + dev_nonce/central_nonce (4 B each) + dev_eui (8 B) = 28 B,
 * zero-padded to 32 B (two full CMAC blocks) -- app_ccm_cmac() already
 * handles multi-block messages (see its RFC4493 Mlen-40/64 KAT vectors in
 * tests/ccm), so this needs no new primitive, just the wider buffer.
 * Rotates every re-join (fresh dev_nonce and central_nonce each time), which
 * is also why resetting the frame counter to 0 on every new pairing is
 * safe: CCM's nonce-uniqueness requirement is on the (key, nonce) pair, not
 * the nonce alone (confirmed w/ #118 phase 2 review). */
static void derive_session_key(uint32_t dev_nonce, uint32_t central_nonce, uint8_t out[P2P_KEY_LEN])
{
	uint8_t block[32] = {0};
	size_t label_len = strlen(P2P_SESSION_KEY_LABEL);

	memcpy(block, P2P_SESSION_KEY_LABEL, label_len);
	block[label_len] = 0x01;
	sys_put_be32(dev_nonce, &block[label_len + 1]);
	sys_put_be32(central_nonce, &block[label_len + 5]);
	/* MSB-first, straight out of the config array: lrw_deveui is already
	 * stored in the order the hex string reads (LoRaMac does the LoRaWAN
	 * LSB reversal internally in lorawan_join), so no byte swap here. */
	memcpy(&block[label_len + 9], g_app_config.lrw_deveui, sizeof(g_app_config.lrw_deveui));
	/* block[label_len+17 .. 31] = zero padding, already zero-initialized. */

	(void)app_ccm_cmac(g_app_config.lrw_appkey, block, sizeof(block), out);
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
			uint8_t tx_power = buf[7 + P2P_KEY_LEN];

			m_net_id = sys_get_be32(&buf[0]);
			m_dev_addr = sys_get_be16(&buf[4]);
			memcpy(m_session_key, &buf[6], P2P_KEY_LEN);
			m_rx1_delay_s = CLAMP(buf[6 + P2P_KEY_LEN], 1, P2P_RX1_DELAY_MAX_S);
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
 * cheap enough; no need for p2pfc's reservation-window trick (confirmed
 * #118 phase 2 review). Never resets across pairings -- it is the central's
 * JoinRequest replay-protection handle (§5.3), so re-joining must never
 * present a dev_nonce the central could have already seen. */
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

/* Persist a successful JoinAccept's pairing state and switch the module to
 * PAIRED. Resets the data-plane frame counter to 0 -- safe because
 * session_key is fresh (see derive_session_key()'s comment) and keeps the
 * on-air counter values small. */
/* A new session_key restarts the counter at 0 (pairing_persist()), so an Ack
 * retry left over from the old session must never go out: its old counter
 * under the new key would reuse a (key, nonce) pair once m_fcnt reaches it, and
 * a central that accepted it would jump its high-water ahead (review of #400,
 * H1). Answers and alarms go back to the TX queue for a fresh counter under
 * the new session; a telemetry or history frame is dropped (the next report /
 * the replay's own retry covers it). Radio work queue only. */
static void ack_retry_drop_old_session(void)
{
	struct p2p_ack_retry_state st;

	(void)k_work_cancel_delayable(&m_ack_retry_work);
	while (k_msgq_get(&m_ack_retry_msgq, &st, K_NO_WAIT) == 0) {
		bool alarm = st.frame_type == APP_RADIO_P2P_FRAME_ALARM;
		bool keep = (alarm || st.frame_type == APP_RADIO_P2P_FRAME_RESPONSE) &&
			    st.body_len <= APP_RADIO_TX_SLOT_SIZE;

		if (keep &&
		    app_radio_tx_queue(alarm ? APP_RADIO_FRAME_ALARM : APP_RADIO_FRAME_ANSWER,
				       APP_RADIO_TAG_OTHER, 0, st.body, st.body_len) == 0) {
			LOG_INF("Old-session uplink (type %u, counter %u) re-queued for the new "
				"session",
				st.frame_type, st.counter);
		} else {
			LOG_WRN("Old-session uplink (type %u, counter %u) dropped at re-join",
				st.frame_type, st.counter);
		}
	}
}

static int pairing_persist(uint32_t net_id, uint16_t dev_addr,
			   const uint8_t session_key[P2P_KEY_LEN], uint8_t rx1_delay_s,
			   const struct p2p_radio_assign *assign)
{
	uint8_t buf[P2P_JOIN_STATE_LEN];

	sys_put_be32(net_id, &buf[0]);
	sys_put_be16(dev_addr, &buf[4]);
	memcpy(&buf[6], session_key, P2P_KEY_LEN);
	buf[6 + P2P_KEY_LEN] = rx1_delay_s;
	buf[7 + P2P_KEY_LEN] = assign->tx_power_assigned ? (uint8_t)assign->tx_power_dbm : 0;

	int ret = settings_save_one(P2P_JOIN_STATE_KEY, buf, sizeof(buf));

	if (ret) {
		/* The join is retried: a session only in RAM would leave the node
		 * JOINING with nothing scheduled (review of #400). */
		LOG_ERR_CALL_FAILED_INT("settings_save_one(p2pjoin/state)", ret);
		return ret;
	}

	m_net_id = net_id;
	m_dev_addr = dev_addr;
	memcpy(m_session_key, session_key, P2P_KEY_LEN);
	m_rx1_delay_s = rx1_delay_s;
	m_session_tx_power_assigned = assign->tx_power_assigned;
	m_session_tx_power_dbm = assign->tx_power_dbm;
	m_link_state = P2P_LINK_PAIRED;

	/* Fresh pairing: counter restarts at 0 under the just-rotated session_key,
	 * so reuse vs. the old session is impossible. Best-effort reserve here --
	 * if it fails, the old (higher) persisted watermark conservatively still
	 * covers these low counters, and fcnt_next() re-reserves fail-closed once
	 * m_fcnt catches up to it. */
	m_fcnt = 0;
	(void)fcnt_reserve(P2P_FCNT_RESERVE);
	ack_retry_drop_old_session();
	return 0;
}

/* Tear the pairing down: drop the persisted session and return the module to
 * UNPAIRED, live, without a reboot. The inverse of pairing_persist().
 *
 * Shared by the `ats radio unjoin` shell path (which reboots afterwards
 * anyway) and the Detach downlink (§5.4), which must take effect immediately
 * -- the central has already dropped the session, so every further uplink
 * would be shouting at a network that is no longer listening. Dropping to
 * UNPAIRED (and clearing m_started) is what stops the report cadence:
 * app_report.c::run_report gates the uplink on app_radio_is_ready() ->
 * app_radio_p2p_is_ready() (paired and started), so the cadence timer keeps
 * running harmlessly while nothing is transmitted.
 *
 * The Ack retries are purged because their frames are encrypted under a
 * session_key that no longer has a peer. The answers and alarms app_radio
 * still queues are plaintext and wait for the next session, as over LoRaWAN
 * across a rejoin.
 *
 * NEVER touches m_dev_nonce (see dnonce_persist()) or m_fcnt: the nonce is
 * the central's JoinRequest replay handle and must keep advancing across
 * pairings, and the counter is reset by the NEXT pairing_persist() under a
 * freshly derived key.
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
	m_last_ack_valid = false;
	m_downlink_pending = false;
	m_pending_frame_len = 0;
	k_msgq_purge(&m_ack_retry_msgq);

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

/* LoRa time-on-air in ms (Semtech AN1200.13) for spreading factor `sf`,
 * integer-only to avoid pulling in the soft-float/libm code on this
 * Cortex-M4-no-FPU part. Fixed PHY: BW 125 kHz, CR 4/5, preamble 8 symbols,
 * explicit header (doc/p2p.md §3.3). Pure -- exposed to tests/p2p_logic. */
P2P_TESTABLE uint32_t p2p_toa_ms(int sf, uint8_t payload_len)
{
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

/* Time-on-air at the SF the radio is currently tuned to. */
static uint32_t frame_toa_ms(uint8_t payload_len)
{
	return p2p_toa_ms(m_sf, payload_len);
}

/* Preamble-catch / open-timing-slop budget in ms, P2P_RX1_WINDOW_SYMBOLS
 * symbols at `sf`/BW -- only ONE component of the real lora_recv() timeout
 * (see p2p_rx1_timeout_ms() and the #define comment above: this driver has no
 * HW symbol-timeout, so this alone is NOT a valid window). Pure -- exposed to
 * tests/p2p_logic. */
P2P_TESTABLE uint32_t rx1_preamble_catch_ms(int sf)
{
	uint64_t tsym_us = ((uint64_t)(1u << sf) * 1000000ULL) / P2P_BANDWIDTH_HZ;

	return (uint32_t)((tsym_us * P2P_RX1_WINDOW_SYMBOLS + 500) / 1000);
}

/* Full lora_recv() timeout for an RX1 wait at `sf` expecting a frame of
 * `expected_frame_len` bytes: preamble-catch budget + that frame's whole
 * time-on-air + a trailing margin (#118 phase 2 HW finding -- this driver's
 * "timeout" aborts an in-flight reception, so it must outlast the entire
 * expected frame, not just its preamble). SF is explicit because a join sweep
 * tries SFs other than the configured one. Pure -- exposed to tests/p2p_logic. */
P2P_TESTABLE uint32_t p2p_rx1_timeout_ms(int sf, uint8_t expected_frame_len)
{
	return rx1_preamble_catch_ms(sf) + p2p_toa_ms(sf, expected_frame_len) +
	       P2P_RX1_TRAILING_MARGIN_MS;
}

/* Open a bounded RX window `rx1_delay_s - open_margin_ms` after `tx_end_ms`
 * (uptime ms), sleeping until it opens then blocking on lora_recv() for
 * p2p_rx1_timeout_ms(expected_frame_len) -- shared by JoinAccept and §6's
 * data-plane Ack (each passes its own expected frame length). Runs on
 * the radio work queue like everything else here; the whole call blocks that queue for
 * up to ~rx1_delay_s (dominant) + the frame's ToA (#118 phase 2 HW finding,
 * see p2p_rx1_timeout_ms()) -- an explicit watchdog feed covers this (and
 * any caller's own backoff sleep) since app_radio's periodic heartbeat
 * can't run until this returns (the radio work queue is single-threaded). Restores TX radio
 * config before returning either way. Returns the received length (>=0) or a
 * negative errno (notably a timeout if nothing arrived within the window). */
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
	app_radio_air_end(); /* the exchange tx_frame_at() began */

	return ret;
}

/* ======================================================================== */
/* Frame TX                                                                 */
/* ======================================================================== */

/* CCM nonce layout (doc/p2p.md §3): counter(4 BE) | dev_addr(2 BE) |
 * frame_type(1) | direction(1) | zero-pad. The direction byte separates the
 * TX and RX keystreams under the same (key, counter). Pure -- exposed to
 * tests/p2p_logic. */
P2P_TESTABLE void build_nonce(uint8_t nonce[P2P_NONCE_LEN], uint32_t counter, uint16_t dev_addr,
			      uint8_t frame_type, uint8_t dir)
{
	memset(nonce, 0, P2P_NONCE_LEN);
	sys_put_be32(counter, &nonce[0]);
	sys_put_be16(dev_addr, &nonce[4]);
	nonce[6] = frame_type;
	nonce[7] = dir;
}

/* ---- Exact sliding-hour duty ledger (B2/D1, see the header comment) ------ */

/* Index of the i-th oldest entry. */
static inline uint8_t duty_slot(const struct p2p_duty *d, uint8_t i)
{
	return (uint8_t)((d->head + i) % P2P_DUTY_LEDGER_ENTRIES);
}

/* Drop every entry that has fallen out of the trailing window.
 *
 * `now` and `end_ms` are uptime truncated to 32 bits and compared as an
 * unsigned difference, which stays correct across the ~49.7-day wrap: an
 * entry only ever lives P2P_DUTY_WINDOW_MS, four orders of magnitude short of
 * the wrap distance, so `now - end_ms` can never alias. */
static void duty_expire(struct p2p_duty *d, uint32_t now)
{
	while (d->count > 0 && (now - d->entries[d->head].end_ms) >= P2P_DUTY_WINDOW_MS) {
		d->head = duty_slot(d, 1);
		d->count--;
	}
}

/* Make room for one more entry by folding the two oldest into one: the
 * younger keeps its end time and takes the older's air, so the pair leaves the
 * window when the younger would have. Only ever over-counts air (the older
 * half is held a little longer), so the 1 % bound holds (F-P2P-1). The sum
 * fits: the ledger never holds more than P2P_DUTY_BUDGET_MS of air. */
static void duty_fold_oldest(struct p2p_duty *d)
{
	struct p2p_duty_entry *oldest = &d->entries[d->head];
	struct p2p_duty_entry *next = &d->entries[duty_slot(d, 1)];

	next->air_ms = (uint16_t)MIN((uint32_t)next->air_ms + oldest->air_ms, UINT16_MAX);
	d->head = duty_slot(d, 1);
	d->count--;
}

/* Air-time recorded inside the current window. Caller must have expired
 * first. Cannot overflow: ENTRIES * UINT16_MAX is ~3.1e6, and the ledger
 * never admits a sum past P2P_DUTY_BUDGET_MS anyway. */
static uint32_t duty_used_ms(const struct p2p_duty *d)
{
	uint32_t used = 0;

	for (uint8_t i = 0; i < d->count; i++) {
		used += d->entries[duty_slot(d, i)].air_ms;
	}
	return used;
}

/* Empty ledger: boot is never blocked, exactly as the full token bucket was
 * not. A reboot therefore forgets the hour just transmitted -- the same hole
 * the bucket had (it restarted full), and accepted for the same reason: the
 * ledger is RAM-only, and persisting it would cost an NVS write per frame.
 * doc/p2p.md §6 records it. */
P2P_TESTABLE void p2p_duty_init(struct p2p_duty *d)
{
	d->head = 0;
	d->count = 0;
}

/* Record `air_ms` of air that finished at `now_ms`. */
P2P_TESTABLE void p2p_duty_charge(struct p2p_duty *d, int64_t now_ms, uint32_t air_ms)
{
	uint32_t now = (uint32_t)now_ms;

	duty_expire(d, now);

	if (d->count >= P2P_DUTY_LEDGER_ENTRIES) {
		/* p2p_duty_wait_ms() already folds before admitting, so the real
		 * call paths never get here with a full ring; fold anyway rather
		 * than drop a charge, the one outcome that could breach 1 %. */
		duty_fold_oldest(d);
	}

	d->entries[duty_slot(d, d->count)] = (struct p2p_duty_entry){
		.end_ms = now,
		.air_ms = (uint16_t)MIN(air_ms, (uint32_t)UINT16_MAX),
	};
	d->count++;
}

/* How many ms to wait before `air_ms` of air may be transmitted -- 0 if now.
 *
 * The guarantee is exact rather than amortised: a frame is admitted only when
 * the air already recorded in the trailing hour plus this frame fits inside
 * P2P_DUTY_BUDGET_MS, so EVERY sliding one-hour window sums to <= 1%.
 *
 * When blocked, the answer is the time until the OLDEST entry leaves the
 * window. That is a lower bound, not necessarily enough on its own -- freeing
 * one entry may still leave the sum too high -- but every caller re-checks
 * and reschedules (app_radio's TX scheduler, reschedule_ack_retry_work,
 * join_work_handler), so the wait converges instead of needing an exact
 * answer here. Returning the true wait would mean solving for the smallest
 * prefix of expiries that frees enough budget, for no behavioural gain. */
P2P_TESTABLE int64_t p2p_duty_wait_ms(struct p2p_duty *d, int64_t now_ms, uint32_t air_ms)
{
	uint32_t now = (uint32_t)now_ms;

	duty_expire(d, now);

	/* F-P2P-1: a full ring folds its two oldest entries rather than making
	 * the frame wait for a slot, so only the air-time budget can refuse it. */
	if (d->count >= P2P_DUTY_LEDGER_ENTRIES) {
		duty_fold_oldest(d);
	}

	if (duty_used_ms(d) + air_ms <= P2P_DUTY_BUDGET_MS) {
		return 0;
	}

	if (d->count == 0) {
		/* Nothing to wait for. Only reachable if one frame's own air
		 * exceeded the whole hourly allowance, which no supported
		 * PHY setting can produce (worst case ~9.2 s at SF12 vs a
		 * 36 s budget) -- refusing forever would be worse than
		 * sending it. */
		return 0;
	}

	/* duty_expire() guarantees the oldest entry is still inside the
	 * window, so this is in (0, P2P_DUTY_WINDOW_MS]. */
	return (int64_t)(P2P_DUTY_WINDOW_MS - (now - d->entries[d->head].end_ms));
}

/* The SF to try on join sweep step `step` (0-based) when the device is
 * configured for `cfg_sf`. Step 0 is always the configured SF -- it is the
 * likeliest answer and the only one a Hub that never moved will ever accept.
 * After that the sweep walks [P2P_JOIN_SWEEP_SF_MIN, P2P_JOIN_SWEEP_SF_MAX]
 * nearest-first, higher SF first on a tie (an SF change is almost always
 * upward, for range), skipping the configured SF. Returns -1 once the pass is
 * exhausted. A configured SF outside the sweep range still gets step 0, then
 * the whole range follows. Pure -- exposed to tests/p2p_logic. */
P2P_TESTABLE int p2p_join_sweep_sf(int cfg_sf, uint8_t step)
{
	if (step == 0) {
		return cfg_sf;
	}

	int max_dist = MAX(P2P_JOIN_SWEEP_SF_MAX - cfg_sf, cfg_sf - P2P_JOIN_SWEEP_SF_MIN);
	uint8_t seen = 0;

	for (int dist = 1; dist <= max_dist; dist++) {
		const int candidates[] = {cfg_sf + dist, cfg_sf - dist}; /* higher first */

		for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
			int sf = candidates[i];

			if (sf < P2P_JOIN_SWEEP_SF_MIN || sf > P2P_JOIN_SWEEP_SF_MAX) {
				continue;
			}
			if (++seen == step) {
				return sf;
			}
		}
	}
	return -1;
}

/* How long to wait before the next JoinRequest, or < 0 for "the boot window is
 * over" -- which hands the episode to the slow policy rather than ending it
 * (join_window_expired). Pure -- exposed to tests/p2p_logic. The caller adds
 * jitter.
 *
 * The slow policy (§7, selected by a self-heal) has no window: a paired device
 * recovers for its whole life, so it always gets its exponential backoff -- or
 * the duty wait, whichever is longer. Taking only the backoff meant a round the
 * duty ledger had refused (the JoinRequest never reached the air) woke into the
 * same refusal having spent a backoff step on nothing; once the 48-entry ring
 * is full the ledger's wait runs to the better part of an hour, well past the
 * 60 s first backoff.
 *
 * The fast policy (a boot join, §5.2) has a 120 s deadline, and that deadline
 * has to bound the wait as well as the retrying. `duty_wait_ms` is whatever
 * p2p_duty_wait_ms returned, which is "time until the oldest ledger entry
 * leaves the sliding hour" -- up to P2P_DUTY_WINDOW_MS, a full hour, once the
 * 48-entry ring is full. 48 JoinRequests at 494 ms fill that ring well inside
 * 120 s, so the unclamped wait routinely landed hours past the deadline: the
 * episode's own deadline check ran, but not until long after
 * the window had closed. Measured on the bench 2026-09-10 (§9): still
 * `state: JOINING` 7 m 38 s into a 120 s window, with a reconstructed duty
 * wait of ~1296 s, and the window-expiry line never reached. Capping at the
 * remaining window makes the next wake-up the one that reports it -- the
 * caller's jitter lands it just past the edge, which is exactly when the
 * hand-over to the slow policy is due. */
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

/* Parse a decrypted Ack body (app_radio_p2p.h): flags|rssi|snr, optionally followed
 * by the pending 0x56's on-air length (D2) and/or a big-endian Unix time tail.
 *
 * The LENGTH decides the shape and the flags only refine it, never the other
 * way round. That asymmetry is deliberate and pre-dates the length byte: a
 * flag claiming a field the frame is too short to hold is ignored rather than
 * trusted, so a central that over-claims cannot walk this parser off the end
 * of the body. It is also what makes the length byte adoptable without a wire
 * version -- a central still emitting the old 3/7-byte body with bit 0 set is
 * read as "pending, length unknown", and the caller falls back to the 255 B
 * worst-case window until the byte appears.
 *
 *   3 B  base                          (bit 1 without a tail is ignored)
 *   4 B  base + pending_frame_len      requires bit 0
 *   7 B  base + time                   bit 0 without the length byte = legacy
 *   8 B  base + pending_frame_len + time
 *
 * Pure -- exposed to tests/p2p_logic. Returns false on any other length. */
P2P_TESTABLE bool p2p_parse_ack_body(const uint8_t *body, size_t body_len, struct p2p_ack_info *out)
{
	bool has_len;
	size_t time_off;

	switch (body_len) {
	case P2P_ACK_BODY_BASE_LEN: /* 3 */
		has_len = false;
		time_off = 0; /* no room for a tail */
		break;
	case P2P_ACK_BODY_BASE_LEN + P2P_ACK_PENDING_LEN_LEN: /* 4 */
		if (!(body[0] & P2P_ACK_FLAG_PENDING)) {
			return false; /* a length byte with nothing pending */
		}
		has_len = true;
		time_off = 0;
		break;
	case P2P_ACK_BODY_BASE_LEN + P2P_ACK_TIME_LEN: /* 7 */
		has_len = false;
		time_off = P2P_ACK_BODY_BASE_LEN;
		break;
	case P2P_ACK_BODY_MAX_LEN: /* 8 */
		if (!(body[0] & P2P_ACK_FLAG_PENDING)) {
			return false;
		}
		has_len = true;
		time_off = P2P_ACK_BODY_BASE_LEN + P2P_ACK_PENDING_LEN_LEN;
		break;
	default:
		return false;
	}

	out->flags = body[0];
	out->rssi = (int8_t)body[1];
	out->snr = (int8_t)body[2];
	out->pending_len_present = has_len;
	out->pending_frame_len = has_len ? body[P2P_ACK_BODY_BASE_LEN] : 0;
	out->time_present = (time_off != 0) && ((out->flags & P2P_ACK_FLAG_TIME) != 0);
	out->unix_time = out->time_present ? sys_get_be32(&body[time_off]) : 0;
	return true;
}

/* Parse JoinAccept's reserved(4) radio assignment (D3, app_radio_p2p.h):
 * channel_idx | sf | tx_power | flags. Every unsupported or out-of-range field
 * is warned about and ignored rather than refused -- a JoinAccept is otherwise
 * valid and authenticated, and refusing to pair over a byte this release
 * cannot honour would strand the node.
 *
 * `sf` is recorded rather than judged here: only the caller knows the
 * configured SF to compare against, and this stays a pure function so
 * tests/p2p_logic can drive it without a config. Pure -- exposed to
 * tests/p2p_logic. */
P2P_TESTABLE void p2p_parse_join_accept_reserved(const uint8_t reserved[4],
						 struct p2p_radio_assign *out)
{
	uint8_t channel_idx = reserved[0];
	uint8_t tx_power = reserved[2];
	uint8_t flags = reserved[3];

	out->tx_power_assigned = false;
	out->tx_power_dbm = 0;
	out->sf_hint = reserved[1];

	if (channel_idx != 0) {
		LOG_WRN("JoinAccept assigns channel %u: not supported (single channel)",
			channel_idx);
	}

	if (tx_power != 0) {
		if (tx_power >= P2P_TX_POWER_MIN_DBM && tx_power <= P2P_TX_POWER_MAX_DBM) {
			out->tx_power_assigned = true;
			out->tx_power_dbm = (int8_t)tx_power;
		} else {
			LOG_WRN("JoinAccept assigns %u dBm TX power: outside %d..%d, ignoring",
				tx_power, P2P_TX_POWER_MIN_DBM, P2P_TX_POWER_MAX_DBM);
		}
	}

	if (flags != 0) {
		LOG_WRN("JoinAccept sets reserved flags 0x%02x: unknown, ignoring", flags);
	}
}

/* Duty-cycle budget (ms) still needed before a `wire_len`-byte frame can be
 * sent at the current SF -- 0 if it can go now. */
static int64_t duty_wait_ms_for(size_t wire_len)
{
	int64_t wait = p2p_duty_wait_ms(&m_duty, k_uptime_get(), frame_toa_ms((uint8_t)wire_len));

	if (wait > 0) {
		app_radio_set_duty_held(true);
	}
	return wait;
}

/* Charge `air_ms` of just-transmitted air-time against the budget; the send
 * went out, so the duty cycle no longer holds anything. */
static void duty_charge(uint32_t air_ms)
{
	int64_t now = k_uptime_get();

	p2p_duty_charge(&m_duty, now, air_ms);
	duty_expire(&m_duty, (uint32_t)now);
	app_radio_set_duty_held(false);
	app_radio_set_airtime(duty_used_ms(&m_duty));
}

/* Retune the radio to `sf` for the next JoinRequest. Radio work queue ONLY: it writes
 * m_sf, which every radio path reads, and reconfigures the modem -- doing that
 * from another thread while the radio work queue is inside lora_recv() would leave the two
 * disagreeing about what the radio is tuned to. */
static void join_set_sf(int sf)
{
	m_sf = (uint8_t)sf;
	(void)radio_configure(true);
}

/* Open a join episode on the radio work queue: back to sweep step 0 (the configured SF)
 * with a clean attempt count. Separate from start_join_episode() because that
 * one may run on the shell thread -- see join_set_sf(). */
static void join_episode_begin(void)
{
	m_join_episode_fresh = false;
	m_join_sweep_step = 0;
	m_join_sf_attempts = 0;
	m_join_sweep_epoch = k_uptime_get();
	join_set_sf(p2p_join_sweep_sf(sf_from_cfg(), 0));
}

/* Decision #22 §3.1: a join / rejoin stays on the configured SF -- one network
 * default on both ends (7), moved only on purpose. The sweep over SF7..12 is a
 * last resort: a node without a JoinAccept for P2P_JOIN_LAST_RESORT_MS makes
 * one pass, then returns to the configured SF for another such period. It
 * saves a truck roll after a network SF change or a mis-provisioned unit; it
 * is not the normal join path. */
static bool join_sweep_allowed(void)
{
	return k_uptime_get() - m_join_sweep_epoch >= P2P_JOIN_LAST_RESORT_MS;
}

/* Account for one JoinRequest that actually reached the air and, when the step
 * has had its attempts, move to the next SF in the order. Returns true when the
 * pass is exhausted -- the caller owns what happens between passes, because
 * that is where the two retry policies differ.
 *
 * Only SENT attempts get here: a duty bounce tried no SF at all, and advancing
 * on one would let a blocked node walk the whole order without transmitting
 * once. */
static bool join_sweep_advance(void)
{
	uint8_t limit = (m_join_sweep_step == 0) ? P2P_JOIN_SF_ATTEMPTS : 1;

	if (++m_join_sf_attempts < limit) {
		return false;
	}

	m_join_sf_attempts = 0;

	int next = (m_join_sweep_step == 0 && !join_sweep_allowed())
			   ? -1
			   : p2p_join_sweep_sf(sf_from_cfg(), m_join_sweep_step + 1);

	if (next < 0) {
		/* Pass exhausted: back to the configured SF, which is both step 0
		 * of the next pass and the SF the data plane would use if a join
		 * landed some other way. A last-resort sweep that ran restarts the
		 * wait for the next one. */
		if (m_join_sweep_step > 0) {
			m_join_sweep_epoch = k_uptime_get();
		}
		m_join_sweep_step = 0;
		join_set_sf(sf_from_cfg());
		return true;
	}

	m_join_sweep_step++;
	join_set_sf(next);
	return false;
}

/* Persist the SF a JoinAccept actually arrived on, when it is not the one the
 * config names. app_radio_p2p_start()'s PAIRED shortcut never joins, so without this
 * a node that swept its way onto the network would come up on the stale
 * configured SF after a reboot with nothing left to re-discover it; persisting
 * an UNCHANGED SF, on the other hand, is a pointless flash write on every
 * ordinary join. Returns 0, or the errno the save failed with (the session
 * keeps running at m_sf either way). Pure enough to expose to tests/p2p_logic,
 * which stubs the save. */
P2P_TESTABLE int p2p_join_adopt_sf(uint8_t joined_sf)
{
	int cfg_sf = sf_from_cfg();

	if ((int)joined_sf == cfg_sf) {
		return 0;
	}

	int ret = app_settings_save_p2p_spreading_factor(joined_sf);

	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_settings_save_p2p_spreading_factor", ret);
		return ret;
	}

	LOG_INF("P2P join: network SF changed %d -> %d, persisted", cfg_sf, joined_sf);
	return 0;
}

/* Start a JOINING episode and schedule the first JoinRequest. `slow` selects
 * which retry policy join_work_handler() opens with: the fast one runs with
 * tight jitter until the 120 s boot window closes and then hands over to the
 * slow policy (§5.2); the slow one starts there directly, on exponential
 * backoff (§7). Neither gives up. Shared by app_radio_p2p_start(), app_radio_p2p_rejoin()
 * (shell), and the self-heal trigger below. */
static void start_join_episode(bool slow)
{
	/* The sweep state and m_sf are seeded by join_work_handler instead, on
	 * the radio work queue: app_radio_p2p_rejoin() reaches here from the shell thread, which
	 * may be running while the radio work queue sits blocked in lora_recv(), and a
	 * retune from under it would leave the radio and m_sf disagreeing. */
	m_join_episode_fresh = true;
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

/* A link check succeeded: an Ack or any other authenticated downlink. */
static void note_uplink_acked(void)
{
	app_radio_link_result(true);
}

/* WARNING's rung (struct app_radio_backend.warning_step): with a fixed SF the
 * P2P counterpart of LoRaWAN's DR / TX ladder is the TX power -- a session the
 * central assigned a lower power steps back up towards the node's own
 * p2p-tx-power, per failed check. Runtime only; the next JoinAccept assigns
 * afresh. Returns false once at the node's own power. */
static bool warning_tx_power_step(void)
{
	int8_t cap = (int8_t)g_app_config.p2p_tx_power;

	if (!m_session_tx_power_assigned || m_session_tx_power_dbm >= cap) {
		return false; /* already at the node's own power */
	}

	int8_t from = m_session_tx_power_dbm;

	m_session_tx_power_dbm = (int8_t)MIN(from + P2P_WARNING_TX_POWER_STEP_DB, cap);
	(void)radio_configure(true);
	publish_link();
	LOG_WRN("P2P WARNING: TX power %d -> %d dBm", from, m_session_tx_power_dbm);
	return true;
}

/* A link check failed: a confirmed frame got no Ack after all its retries.
 * app_radio counts it only while the session is up, so once a self-heal is
 * under way further give-ups do not re-trigger it. */
static void note_uplink_cycle_failed(void)
{
	app_radio_link_result(false);
}

/* struct app_radio_backend.rejoin: the self-healing re-join (§7) on the slow
 * policy. Refused while unprovisioned: no JoinRequest can succeed under an
 * all-zero app_key (§4) or DevEUI (#417). */
static int p2p_tx_rejoin(bool forced)
{
	ARG_UNUSED(forced);

	if (!app_key_is_set()) {
		LOG_ERR("P2P self-heal refused: lrw_appkey is all-zero (unprovisioned)");
		return -ENOTSUP;
	}
	if (!dev_eui_is_set()) {
		LOG_ERR("P2P self-heal refused: lrw_deveui is all-zero (unprovisioned)");
		return -ENOTSUP;
	}
	LOG_WRN("P2P: self-healing re-join (§7)");
	start_join_episode(true);
	return 0;
}

/* The 12 B header (app_radio_p2p.h, decision #22), written and read in one
 * place so no path can disagree on where FCtrl sits. */
P2P_TESTABLE void p2p_hdr_put(uint8_t *frame, const struct p2p_hdr *h)
{
	sys_put_be32(h->net_id, &frame[P2P_HDR_OFF_NET_ID]);
	sys_put_be16(h->dev_addr, &frame[P2P_HDR_OFF_DEV_ADDR]);
	frame[P2P_HDR_OFF_TYPE] = h->frame_type;
	frame[P2P_HDR_OFF_FCTRL] = h->fctrl;
	sys_put_be32(h->counter, &frame[P2P_HDR_OFF_COUNTER]);
}

P2P_TESTABLE void p2p_hdr_get(const uint8_t *frame, struct p2p_hdr *h)
{
	h->net_id = sys_get_be32(&frame[P2P_HDR_OFF_NET_ID]);
	h->dev_addr = sys_get_be16(&frame[P2P_HDR_OFF_DEV_ADDR]);
	h->frame_type = frame[P2P_HDR_OFF_TYPE];
	h->fctrl = frame[P2P_HDR_OFF_FCTRL];
	h->counter = sys_get_be32(&frame[P2P_HDR_OFF_COUNTER]);
}

/* Build header+encrypt one frame into `frame` (>= P2P_HDR_LEN + body_len +
 * P2P_TAG_LEN bytes) under an explicit net_id/dev_addr/session_key. Pure --
 * no radio/queue/counter side effects. Exposed to tests/p2p_logic; the
 * firmware calls it through build_frame() with the live pairing state.
 * FCtrl is in the AAD, not in the nonce. */
P2P_TESTABLE int build_frame_keyed(uint32_t net_id, uint16_t dev_addr,
				   const uint8_t session_key[P2P_KEY_LEN], uint8_t frame_type,
				   uint8_t fctrl, const uint8_t *body, size_t body_len,
				   uint32_t counter, uint8_t *frame)
{
	const struct p2p_hdr h = {
		.net_id = net_id,
		.dev_addr = dev_addr,
		.frame_type = frame_type,
		.fctrl = fctrl,
		.counter = counter,
	};

	p2p_hdr_put(frame, &h);

	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, counter, dev_addr, frame_type, P2P_DIR_TX);

	return app_ccm_encrypt_and_tag(session_key, nonce, P2P_NONCE_LEN, /* AAD */ frame,
				       P2P_HDR_LEN, body, body_len, &frame[P2P_HDR_LEN],
				       &frame[P2P_HDR_LEN + body_len], P2P_TAG_LEN);
}

/* Build one frame with the live pairing state (m_net_id/m_dev_addr/
 * m_session_key). Shared by the real TX path (tx_frame_at) and the
 * `ats radio compose` dry-run (debug_compose_work_handler). */
static int build_frame(uint8_t frame_type, uint8_t fctrl, const uint8_t *body, size_t body_len,
		       uint32_t counter, uint8_t *frame)
{
	int ret = build_frame_keyed(m_net_id, m_dev_addr, m_session_key, frame_type, fctrl, body,
				    body_len, counter, frame);
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
 * must have already checked the duty-cycle budget (duty_wait_ms_for). Returns
 * 0 or errno; on success reports the send-completion time via `tx_end_ms`
 * (uptime ms, for the caller's RX1/Ack wait) and charges the duty budget. */
/* lora_send() failed. The driver reports a TX timeout as -EAGAIN and a busy
 * modem as -EBUSY, which this module reads as "duty cycle refused" and "uplink
 * in flight": a dead or wedged modem was then retried every few ms and never
 * advanced the join sweep (review of #400). A radio fault is -EIO here. The PA
 * may have been keyed, so its air is charged to the ledger (over-counting only
 * errs towards compliance). */
static int tx_send_failed(uint8_t wire_len)
{
	app_radio_count(APP_RADIO_CNT_TX_ERR);
	duty_charge(frame_toa_ms(wire_len));
	return -EIO;
}

static int tx_frame_at(uint8_t frame_type, uint8_t fctrl, const uint8_t *body, size_t body_len,
		       uint32_t counter, int64_t *tx_end_ms)
{
	if (m_listening) {
		LOG_WRN("TX skipped: radio in listen mode");
		return -EBUSY;
	}
	if (body_len > P2P_MAX_BODY) {
		LOG_ERR("Body %zu B over P2P budget %d B", body_len, P2P_MAX_BODY);
		return -EMSGSIZE;
	}
	int64_t gap_ms = m_link_idle_at - k_uptime_get();

	if (gap_ms > 0) {
		k_sleep(K_MSEC(gap_ms)); /* radio work queue; at most P2P_TX_GAP_MS */
	}
	if (m_link_state != P2P_LINK_PAIRED) {
		/* Callers gate on app_radio_p2p_is_ready(), so this should never happen --
		 * defensive only (no session_key to encrypt the data plane under
		 * yet). */
		LOG_ERR("TX skipped: not paired (#118 phase 2)");
		return -ENOTCONN;
	}

	uint8_t frame[P2P_FRAME_MAX];

	int ret = build_frame(frame_type, fctrl, body, body_len, counter, frame);

	if (ret) {
		return ret;
	}

	size_t wire_len = P2P_HDR_LEN + body_len + P2P_TAG_LEN;

	/* The exchange runs to the end of its RX1 window (p2p_rx_window()): no
	 * flash write may stall the CPU in between (app_radio_air_begin()). */
	app_radio_air_begin();
	ret = lora_send(m_lora_dev, frame, wire_len);
	if (ret) {
		app_radio_air_end();
		LOG_ERR_CALL_FAILED_INT("lora_send", ret);
		return tx_send_failed((uint8_t)wire_len);
	}

	int64_t end = k_uptime_get();
	uint32_t air = frame_toa_ms((uint8_t)wire_len);

	/* Charge the just-sent air-time against the 1% budget. */
	duty_charge(air);
	app_radio_count(APP_RADIO_CNT_TX);
	app_radio_note_send(true, false);
	publish_link();

	LOG_INF("TX type %u%s, %zu B (counter %u, %u ms air)", frame_type,
		(fctrl & P2P_FCTRL_CONFIRMED) ? " confirmed" : "", wire_len, counter, air);

	*tx_end_ms = end;
	return 0;
}

/* Frame + encrypt + transmit one body under a FRESH counter. Returns 0,
 * -EAGAIN (duty cycle), or errno; on success reports the counter used and
 * the send-completion time via the out-params (see tx_frame_at()). */
static int tx_frame(uint8_t frame_type, uint8_t fctrl, const uint8_t *body, size_t body_len,
		    uint32_t *counter_out, int64_t *tx_end_ms)
{
	if (k_msgq_num_used_get(&m_ack_retry_msgq) > 0) {
		return -EBUSY; /* one confirmed uplink in flight (F-P1-1) */
	}

	int64_t wait = duty_wait_ms_for(P2P_HDR_LEN + body_len + P2P_TAG_LEN);

	if (wait > 0) {
		LOG_WRN("TX duty-cycle blocked for %lld ms", wait);
		app_radio_note_send(false, true);
		return -EAGAIN;
	}

	uint32_t counter;
	int ret = fcnt_next(&counter);

	if (ret) {
		return ret; /* fail-closed: no durably-reserved counter available */
	}

	ret = tx_frame_at(frame_type, fctrl, body, body_len, counter, tx_end_ms);

	if (ret == 0) {
		*counter_out = counter;
	}
	return ret;
}

/* Report the node-measured quality of an authenticated downlink to app_radio
 * (RadioState, #446). */
static void note_downlink(int16_t rssi, int8_t snr)
{
	app_radio_note_downlink(rssi, snr);
}

/* Apply an authenticated, parsed Ack of this uplink's window: the central's
 * uplink RSSI/SNR, the pending-downlink flag and the time tail. Split out of
 * recv_ack() so tests/p2p_logic can drive it without CCM framing. */
P2P_TESTABLE void p2p_apply_ack(const struct p2p_ack_info *ack, uint32_t counter, int16_t rssi,
				int8_t snr)
{
	/* B1: the RSSI/SNR the central measured on this uplink. */
	m_last_ack_rssi = ack->rssi;
	m_last_ack_snr = ack->snr;
	m_last_ack_valid = true;
	app_radio_set_uplink_rssi(ack->rssi, ack->snr);
	note_downlink(rssi, snr);

	/* B4/D2: remember whether -- and how large -- to size the NEXT uplink's
	 * window. Clamp defensively: a corrupt-but-authentic byte below a bare
	 * header+tag or above the PHY limit would otherwise produce a window
	 * that cannot hold any frame at all. */
	m_downlink_pending = (ack->flags & P2P_ACK_FLAG_PENDING) != 0;
	m_pending_frame_len = ack->pending_len_present
				      ? (uint8_t)CLAMP(ack->pending_frame_len,
						       P2P_HDR_LEN + P2P_TAG_LEN, P2P_FRAME_MAX)
				      : 0;

	/* B5: apply the wall-clock time tail if present, with the same sanity
	 * bounds as the LoRaWAN DeviceTimeAns (L-5). */
	if (ack->time_present) {
		(void)app_clock_set_network_time(ack->unix_time);
	}
	/* A pending clock_sync is answered once the network time came (PF-2, as
	 * LoRaWAN waits for LORAWAN_TIME_UPDATED): an Ack without the tail, or a
	 * 0x56 in its place, leaves it pending for the next confirmed uplink. */
	if (ack->time_present && atomic_cas(&m_clock_sync_pending, 1, 0)) {
		(void)app_radio_send_info((uint32_t)atomic_get(&m_clock_sync_seq));
	}

	/* rssi/snr = the central's measurement of the uplink (Ack body, B1);
	 * dl_rssi/dl_snr = this node's measurement of the Ack itself. */
	if (m_pending_frame_len != 0) {
		LOG_INF("Ack (counter %u) rssi=%d snr=%d dl_rssi=%d dl_snr=%d [pending] "
			"pending_len=%u%s",
			counter, m_last_ack_rssi, m_last_ack_snr, rssi, snr, m_pending_frame_len,
			ack->time_present ? " [time]" : "");
	} else {
		LOG_INF("Ack (counter %u) rssi=%d snr=%d dl_rssi=%d dl_snr=%d%s%s", counter,
			m_last_ack_rssi, m_last_ack_snr, rssi, snr,
			m_downlink_pending ? " [pending]" : "", ack->time_present ? " [time]" : "");
	}
}

/* Wait for and validate the RX1 downlink for `counter` after `tx_end_ms`
 * (doc/p2p.md §6): header must match (net_id/dev_addr/counter echo), then
 * AES-CCM decrypt under session_key, direction=RX. Two frame types are
 * accepted, both confirming the uplink got through:
 *  - Ack (0xFA): body flags(1) | rssi_i8 | snr_i8 (B1) + optional 4-byte Unix
 *    time tail when flags bit 1 is set (B5); length derived from the frame
 *    length (self-describing). The pending flag (bit 0) sizes the NEXT uplink's
 *    window for a command (B4).
 *  - Command (0x56, B4): only when a pending downlink was announced -- decrypt,
 *    dispatch (app_radio_downlink), and treat as an implicit Ack.
 * Returns true iff a valid, matching downlink was received. */
static bool recv_ack(uint32_t counter, int64_t tx_end_ms)
{
#if defined(CONFIG_SHELL)
	if (m_debug_drop_acks > 0) {
		m_debug_drop_acks--;
		LOG_WRN("Debug: dropping Ack (counter %u), %u drop(s) left", counter,
			m_debug_drop_acks);
		app_radio_air_end(); /* no RX1 window this time */
		return false;
	}
#endif /* defined(CONFIG_SHELL) */

	uint8_t buf[P2P_FRAME_MAX];
	int16_t rssi;
	int8_t snr;
	/* Size the window for the largest downlink the central may send. With
	 * nothing pending that is the fully-extended Ack (23 B). Once a downlink
	 * has been announced (B4) a 0x56 arrives INSTEAD of the Ack, and the
	 * receiver must stay on for its whole time-on-air -- this driver's
	 * "timeout" aborts an in-flight reception (see p2p_rx1_timeout_ms), so a
	 * window sized short truncates a real command mid-frame.
	 *
	 * D2: the announcing Ack now carries that frame's exact length, so the
	 * window costs only what the command actually needs -- at SF10 a 2 B
	 * GetInfo drops the receiver-on from 2434 ms to 468 ms. Without the byte
	 * (central not yet upgraded) fall back to the 255 B worst case, which is
	 * the pre-D2 behaviour. */
	uint8_t want_max;

	/* Decision #22 §3.3 (O7 interim): the central may put a queued 0x56 of up
	 * to P2P_RX1_CMD_INTERIM_LEN into the RX1 of ANY uplink, confirmed or not,
	 * without announcing it first, so every window is sized for that; only a
	 * longer command is still announced (pending bit + length) and sizes the
	 * next window exactly. */
	if (!m_downlink_pending) {
		want_max = P2P_RX1_CMD_INTERIM_LEN;
	} else if (m_pending_frame_len != 0) {
		want_max = m_pending_frame_len;
	} else {
		want_max = P2P_FRAME_MAX;
	}

	int len = p2p_rx_window(tx_end_ms, m_rx1_delay_s, want_max, buf, sizeof(buf), &rssi, &snr);

	if (len < P2P_HDR_LEN + P2P_TAG_LEN) {
		return false; /* timeout or too short to hold a header + tag */
	}

	struct p2p_hdr hdr;

	p2p_hdr_get(buf, &hdr);

	uint8_t frame_type = hdr.frame_type;
	uint32_t ctr = hdr.counter;

	if (hdr.net_id != m_net_id || hdr.dev_addr != m_dev_addr || ctr != counter) {
		return false;
	}

	size_t body_len = (size_t)len - P2P_HDR_LEN - P2P_TAG_LEN;

	/* --- Link control (§5.4): Detach (0xFD) / RejoinRequest (0xFE) --- */
	if (frame_type == APP_RADIO_P2P_FRAME_DETACH ||
	    frame_type == APP_RADIO_P2P_FRAME_REJOIN_REQUEST) {
		if (body_len != 0) {
			return false; /* both are empty-bodied on the wire */
		}

		uint8_t nonce[P2P_NONCE_LEN];
		/* Zero-length plaintext, but a real object: app_ccm's failure
		 * path memset()s the output buffer, and memset(NULL, 0, 0) is
		 * undefined even though it copies nothing. */
		uint8_t empty[1];

		build_nonce(nonce, ctr, m_dev_addr, frame_type, P2P_DIR_RX);

		/* An empty message is a legitimate CCM input: the tag still
		 * covers the nonce and the 12 B header AAD, which is what
		 * authenticates this frame (app_ccm.c::params_ok constrains the
		 * nonce/AAD/tag lengths only, not the payload; RFC 3610 allows
		 * an empty message). Together with the counter echo checked
		 * above -- single-use per uplink, and this window closes right
		 * after -- neither frame can be forged or replayed without
		 * session_key. */
		int ret = app_ccm_auth_decrypt(m_session_key, nonce, P2P_NONCE_LEN, buf,
					       P2P_HDR_LEN, &buf[P2P_HDR_LEN], 0, &buf[P2P_HDR_LEN],
					       P2P_TAG_LEN, empty);
		if (ret) {
			LOG_WRN("Detach/RejoinRequest auth failed (counter %u)", counter);
			return false;
		}

		note_downlink(rssi, snr);

		if (frame_type == APP_RADIO_P2P_FRAME_DETACH) {
			LOG_WRN("Detach received (counter %u): pairing cleared, radio idle "
				"until reboot or `join`",
				counter);
			(void)pairing_clear();
		} else if (!app_key_is_set() || !dev_eui_is_set()) {
			/* The central can ask a paired node to rekey at any time,
			 * and the session key it authenticated this frame with
			 * outlives a config edit -- so the identity could have been
			 * cleared underneath us since the join. Re-joining then
			 * would put an all-zero app_key or DevEUI on the air, which
			 * is exactly what the guards on the other three
			 * start_join_episode() paths exist to prevent (#417). */
			LOG_ERR("RejoinRequest received (counter %u) but lrw_appkey or lrw_deveui "
				"is all-zero (device unprovisioned) -- not re-joining",
				counter);
		} else {
			LOG_WRN("RejoinRequest received (counter %u): re-joining", counter);
			/* Self-heal policy (§7), not the boot window: this is a
			 * paired node the central asked to rekey, and it must
			 * keep trying past the 120 s cap with backoff. */
			start_join_episode(true);
		}

		/* Either way the central proved it received this uplink, so the
		 * cycle is confirmed and no Ack retry is scheduled. */
		return true;
	}

	/* --- B4: a 0x56 COMMAND takes this window instead of the Ack --- */
	if (frame_type == APP_RADIO_P2P_FRAME_COMMAND) {
		uint8_t nonce[P2P_NONCE_LEN];
		uint8_t body[P2P_MAX_BODY];

		build_nonce(nonce, ctr, m_dev_addr, frame_type, P2P_DIR_RX);

		int ret = app_ccm_auth_decrypt(m_session_key, nonce, P2P_NONCE_LEN, buf,
					       P2P_HDR_LEN, &buf[P2P_HDR_LEN], body_len,
					       &buf[P2P_HDR_LEN + body_len], P2P_TAG_LEN, body);
		if (ret) {
			LOG_WRN("Command auth failed (counter %u)", counter);
			return false;
		}

		note_downlink(rssi, snr);
		LOG_INF("Command received (counter %u, %zu B) dl_rssi=%d dl_snr=%d", counter,
			body_len, rssi, snr);
		app_radio_downlink(body, body_len);

		/* The command replaced this uplink's Ack; receiving it confirms the
		 * uplink reached the central. The 0x55 response is now queued; the
		 * next Ack re-announces any further pending downlink. */
		m_downlink_pending = false;
		return true;
	}

	/* --- Ack (0xFA): rssi/snr + optional pending length and time tail --- */
	/* Bound the body before spending a decrypt; p2p_parse_ack_body() below
	 * does the exact 3/4/7/8 validation once the plaintext is in hand. */
	if (frame_type != APP_RADIO_P2P_FRAME_ACK || body_len < P2P_ACK_BODY_BASE_LEN ||
	    body_len > P2P_ACK_BODY_MAX_LEN) {
		return false;
	}

	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, ctr, m_dev_addr, frame_type, P2P_DIR_RX);

	uint8_t body[P2P_ACK_BODY_MAX_LEN];
	int ret = app_ccm_auth_decrypt(m_session_key, nonce, P2P_NONCE_LEN, buf, P2P_HDR_LEN,
				       &buf[P2P_HDR_LEN], body_len, &buf[P2P_HDR_LEN + body_len],
				       P2P_TAG_LEN, body);
	if (ret) {
		LOG_WRN("Ack auth failed (counter %u)", counter);
		return false;
	}

	struct p2p_ack_info ack;

	if (!p2p_parse_ack_body(body, body_len, &ack)) {
		return false;
	}

	p2p_apply_ack(&ack, counter, rssi, snr);
	return true;
}

/* (Re)schedule m_ack_retry_work for whenever the duty cycle clears (plus
 * jitter), if the queue has anything pending -- a no-op otherwise. Called
 * after every enqueue/dequeue so the timer always reflects the current
 * queue state and duty-cycle estimate. The wait is sized for the frame at the
 * head of the queue (peeked, not dequeued). */
/* The wait before the next retry of a frame that has had `attempt` retries (0
 * before the first): a random P2P_ACK_RETRY_BACKOFF_MIN_MS..2^(attempt+1) s, so
 * 1..2 s, 1..4 s, 1..8 s. Pure -- exposed to tests/p2p_logic. */
P2P_TESTABLE uint32_t p2p_ack_retry_backoff_ms(int attempt, uint32_t rand32)
{
	int n = CLAMP(attempt + 1, 1, P2P_ACK_MAX_RETRIES);
	uint32_t max_ms = (uint32_t)P2P_ACK_RETRY_BACKOFF_MIN_MS << n;

	return P2P_ACK_RETRY_BACKOFF_MIN_MS + rand32 % (max_ms - P2P_ACK_RETRY_BACKOFF_MIN_MS);
}

static void reschedule_ack_retry_work(void)
{
	struct p2p_ack_retry_state head;

	if (k_msgq_peek(&m_ack_retry_msgq, &head) != 0) {
		return; /* queue empty */
	}

	int64_t wait_ms = duty_wait_ms_for(P2P_HDR_LEN + head.body_len + P2P_TAG_LEN);
	uint32_t backoff = p2p_ack_retry_backoff_ms(head.attempt, sys_rand32_get());

	k_work_reschedule_for_queue(app_radio_work_q(), &m_ack_retry_work,
				    K_MSEC(wait_ms + backoff));
}

/* Queue an asynchronous Ack retry for `counter` (doc/p2p.md §6) -- NOT a
 * blocking wait, so no cap is needed (see the comment after
 * P2P_ACK_RETRY_BACKOFF_MIN_MS for why an earlier capped-sleep design was wrong).
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
		app_radio_count(APP_RADIO_CNT_FAIL);
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

	if (duty_wait_ms_for(P2P_HDR_LEN + st.body_len + P2P_TAG_LEN) > 0) {
		reschedule_ack_retry_work();
		return;
	}

	/* Committed to sending st now -- actually dequeue it (peek() above only
	 * looked, so a still-blocked duty cycle above leaves it in place). */
	(void)k_msgq_get(&m_ack_retry_msgq, &st, K_NO_WAIT);

	int64_t tx_end;
	int ret = tx_frame_at(st.frame_type, P2P_FCTRL_CONFIRMED, st.body, st.body_len, st.counter,
			      &tx_end);

	if (ret) {
		LOG_WRN("Ack retry (counter %u) send failed: %d", st.counter, ret);
	} else {
		LOG_INF("Uplink retry %d/%d sent (counter %u)", st.attempt + 1, P2P_ACK_MAX_RETRIES,
			st.counter);
		app_radio_count(APP_RADIO_CNT_RETRY);

		bool acked = recv_ack(st.counter, tx_end);

		m_link_idle_at = k_uptime_get() + P2P_TX_GAP_MS;
		if (acked) {
			note_uplink_acked();
		} else {
			if (st.attempt + 1 < P2P_ACK_MAX_RETRIES) {
				st.attempt++;
				/* Re-queue at the TAIL (not retried in place): with more
				 * than one frame pending, this services them round-robin
				 * instead of one starving the others. */
				if (k_msgq_put(&m_ack_retry_msgq, &st, K_NO_WAIT) != 0) {
					LOG_WRN("Ack retry queue full re-queueing counter %u; "
						"giving up",
						st.counter);
					note_uplink_cycle_failed();
				}
			} else {
				LOG_WRN("Uplink counter %u unacked after %d retries; giving "
					"up",
					st.counter, P2P_ACK_MAX_RETRIES);
				note_uplink_cycle_failed();
			}
		}
	}

	/* Whether this entry is done, gave up, or got re-queued, other frames
	 * may still be waiting -- keep the timer aligned with the queue. */
	reschedule_ack_retry_work();
	if (k_msgq_num_used_get(&m_ack_retry_msgq) == 0) {
		/* The in-flight uplink is done: the frames that bounced with -EBUSY
		 * (the TX path, the history replay) go now. */
		app_radio_tx_kick();
	}
}

/* Confirmed uplink (§6): send one frame and wait once for its Ack. If
 * unacknowledged, hand off to schedule_ack_retry() instead of blocking here
 * -- returns 0 either way (the frame WAS transmitted; confirmation, if a
 * retry is needed, continues asynchronously on m_ack_retry_work). Callers
 * (the TX backend, the history replay) move on immediately rather than
 * waiting for the eventual outcome. Returns -EAGAIN only if the FIRST send
 * itself was duty-cycle blocked (unchanged pre-existing semantics, same as
 * tx_frame()), or a hard errno from that first send. */
static int send_uplink(uint8_t frame_type, const uint8_t *body, size_t body_len, bool confirmed)
{
	uint32_t counter;
	int64_t tx_end;

	int ret = tx_frame(frame_type, confirmed ? P2P_FCTRL_CONFIRMED : 0, body, body_len,
			   &counter, &tx_end);

	if (ret) {
		return ret;
	}

	/* The RX1 opens after every uplink: an unconfirmed one gets no Ack, but a
	 * queued 0x56 (or a Detach / RejoinRequest) may still arrive in it. */
	bool heard = recv_ack(counter, tx_end);

	m_link_idle_at = k_uptime_get() + P2P_TX_GAP_MS;
	if (heard) {
		note_uplink_acked(); /* any authenticated downlink is a link success */
	} else if (confirmed) {
		schedule_ack_retry(frame_type, body, body_len, counter, 0);
	}
	/* An unconfirmed frame is sent once (decision #22 §3.2, NbTrans 1). */

	return 0;
}

static int send_confirmed(uint8_t frame_type, const uint8_t *body, size_t body_len)
{
	return send_uplink(frame_type, body, body_len, true);
}

/* Decision #22 §3.2: telemetry is unconfirmed and sent once, except a report
 * app_radio's cadence picked as a link check (`due`: the first after a link-up
 * and every radio-link-check-interval-th, every one in WARNING), which is
 * CONFIRMED -- alarms and answers are always confirmed and judge the link on
 * their own. Decided once per snapshot, kept for all its frames. */
static bool telemetry_report_confirmed(bool due)
{
	/* A pending clock_sync also rides a confirmed report: the time comes in the
	 * Ack's tail, as LoRaWAN's DeviceTimeReq rides the next uplink (PF-2). */
	if (atomic_get(&m_clock_sync_pending) &&
	    atomic_inc(&m_clock_sync_reports) < P2P_CLOCK_SYNC_REPORTS_MAX) {
		return true;
	}
	return due;
}

/* ---- TX backend: app_radio schedules, this sends one frame (doc/plan/460 F4) ---- */

/* struct app_radio_backend.send. Answers, alarms and history frames go out as
 * 0x55 RESPONSE / 0x57 ALARM, telemetry as 0x52. Radio work queue only. */
static int p2p_tx_send(const struct app_radio_frame *f, struct app_radio_tx_result *res)
{
	uint8_t type;

	switch (f->kind) {
	case APP_RADIO_FRAME_TELEMETRY:
		type = APP_RADIO_P2P_FRAME_TELEMETRY;
		break;
	case APP_RADIO_FRAME_ALARM:
		type = APP_RADIO_P2P_FRAME_ALARM;
		break;
	default:
		type = APP_RADIO_P2P_FRAME_RESPONSE;
		break;
	}
	if (f->len == 0) {
		return -EINVAL; /* no MAC to flush: the P2P budget is never 0 */
	}

	int ret = send_uplink(type, f->buf, f->len, (f->flags & APP_RADIO_FRAME_CONFIRMED) != 0);

	switch (ret) {
	case 0:
		break;
	case -EAGAIN:
		/* Duty cycle: again the moment the ledger has room for this frame. */
		res->wait_ms = (uint32_t)duty_wait_ms_for(P2P_HDR_LEN + f->len + P2P_TAG_LEN) +
			       P2P_TX_RETRY_MARGIN_MS;
		break;
	case -EMSGSIZE:
		res->budget = P2P_MAX_BODY;
		break;
	default:
		/* -EBUSY: a confirmed uplink is in flight or the radio listens
		 * (kicked when it ends); -ENOTCONN: no session (kicked at the next
		 * JoinAccept); other: a radio fault. */
		break;
	}
	return ret;
}

static uint8_t p2p_tx_budget(void)
{
	return P2P_MAX_BODY;
}

/* Once per report, at its first frame, kept for all its frames. */
static uint8_t p2p_tx_report_flags(bool due)
{
	return telemetry_report_confirmed(due) ? APP_RADIO_FRAME_CONFIRMED : 0;
}

/* struct app_radio_backend.in_flight: a transmitted confirmed frame still waits
 * for its Ack retries (m_ack_retry_msgq), so a post-command reboot waits too. */
static bool p2p_tx_in_flight(void)
{
	return k_msgq_num_used_get(&m_ack_retry_msgq) > 0;
}

const struct app_radio_backend app_radio_p2p_backend = {
	.send = p2p_tx_send,
	.budget = p2p_tx_budget,
	.tx_ready = app_radio_p2p_is_ready,
	.report_flags = p2p_tx_report_flags,
	.get_state = app_radio_p2p_get_state,
	.warning_step = warning_tx_power_step,
	.rejoin = p2p_tx_rejoin,
	.in_flight = p2p_tx_in_flight,
	/* §6: answers, alarms and history frames are confirmed; telemetry only
	 * the N-th report (report_flags). */
	.confirm_kinds = BIT(APP_RADIO_FRAME_ANSWER) | BIT(APP_RADIO_FRAME_ALARM) |
			 BIT(APP_RADIO_FRAME_HISTORY),
	.frame_gap_ms = 0, /* P2P_TX_GAP_MS after an Ack window is taken in tx_frame_at() */
	.cmd_transport = APP_CMD_TRANSPORT_P2P,
};

/* ======================================================================== */
/* Frame RX (reference receiver / diagnostics, CONFIG_SHELL)                */
/* ======================================================================== */

#if defined(CONFIG_SHELL)
static void rx_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct p2p_rx_msg msg;

	while (k_msgq_get(&m_rx_msgq, &msg, K_NO_WAIT) == 0) {
		if (msg.len < P2P_HDR_LEN) {
			LOG_WRN("RX runt frame (%u B)", msg.len);
			continue;
		}

		struct p2p_hdr hdr;

		p2p_hdr_get(msg.buf, &hdr);

		uint32_t net_id = hdr.net_id;
		uint16_t dev_addr = hdr.dev_addr;
		uint8_t frame_type = hdr.frame_type;
		uint32_t counter = hdr.counter;

		if (net_id != m_net_id) {
			LOG_DBG("RX foreign net_id %u; ignored", net_id);
			continue;
		}

		/* JoinRequest/JoinAccept carry a CLEARTEXT body + a 16 B plain
		 * AES-CMAC tag (P2P_JOIN_TAG_LEN, see the P2P_JOIN_TAG_LABEL comment
		 * above), not AES-CCM -- there is nothing secret to decrypt, so just
		 * log the body as-is (no tag verification here: this listen mode is
		 * diagnostic/bench-only eavesdropping, not a protocol participant).
		 * Any other frame_type is a data-plane frame under session_key,
		 * which tx_frame_at() only ever sends once PAIRED (#118 phase 2) --
		 * this listen mode has no peer's session_key to try, and there is no
		 * longer a join_key fallback either (#118 phase 2 revision), so such
		 * a frame can never be decoded here and is just noted. */
		if (frame_type == APP_RADIO_P2P_FRAME_JOIN_REQUEST ||
		    frame_type == APP_RADIO_P2P_FRAME_JOIN_ACCEPT) {
			if (msg.len < P2P_HDR_LEN + P2P_JOIN_TAG_LEN) {
				LOG_WRN("RX runt join frame (%u B)", msg.len);
				continue;
			}

			size_t body_len = msg.len - P2P_HDR_LEN - P2P_JOIN_TAG_LEN;

			LOG_INF("RX %s from addr %u, ctr %u (RSSI %d dBm, SNR %d dB, %zu B "
				"body)",
				frame_type == APP_RADIO_P2P_FRAME_JOIN_REQUEST ? "JoinRequest"
									       : "JoinAccept",
				dev_addr, counter, msg.rssi, msg.snr, body_len);
			LOG_HEXDUMP_INF(&msg.buf[P2P_HDR_LEN], body_len,
					"P2P join body (cleartext, tag not verified):");
			continue;
		}

		LOG_DBG("RX data-plane frame_type %u pre-pairing; no session_key to try, "
			"ignored",
			frame_type);
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
	k_work_submit_to_queue(app_radio_work_q(), &m_rx_work);
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
		app_radio_tx_kick(); /* frames the listen mode bounced with -EBUSY */
	}
	return 0;
}
#endif /* defined(CONFIG_SHELL) */

/* ======================================================================== */
/* Join handshake (#118 phase 2, doc/p2p.md §5.3)                           */
/* ======================================================================== */

static void mark_ready(void)
{
	/* Paired: back to the fast policy. */
	m_join_slow = false;
	set_rejoin_attempt(0);

	/* Fresh session: last Ack's link quality and any pending-downlink hint
	 * from the old session no longer apply. */
	m_last_ack_valid = false;
	m_downlink_pending = false;
	m_pending_frame_len = 0;

	m_started = true;
	/* Link supervision and the M-2 clock start afresh; the first report of
	 * the session is the link check (§3.2). */
	app_radio_link_up();
	/* The TX power assignment is NOT reset here: pairing_persist() has
	 * already installed this session's value (or cleared it), and
	 * mark_ready() also runs on the already-PAIRED boot shortcut, where the
	 * value restored from NVS is the one to keep. */

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

/* Build one JoinRequest into `frame` (P2P_JOIN_REQ_LEN bytes): the header, the
 * cleartext body and the full CMAC tag over both. Factored out of
 * send_join_request() so a ztest can pin the exact bytes against the shared
 * KAT fixture without a radio (#417) -- the on-air frame and the tested frame
 * are then the same code, not two spellings of it. */
static void join_request_build(uint32_t nonce_val, uint8_t frame[P2P_JOIN_REQ_LEN])
{
	const struct p2p_hdr h = {
		.net_id = P2P_PREJOIN_NET_ID,
		.dev_addr = P2P_PREJOIN_DEV_ADDR,
		.frame_type = APP_RADIO_P2P_FRAME_JOIN_REQUEST,
		.fctrl = 0, /* join frames carry FCtrl 0 (decision #22) */
		.counter = nonce_val,
	};

	p2p_hdr_put(frame, &h);

	uint8_t *body = &frame[P2P_HDR_LEN];

	body[0] = P2P_PRODUCT_TYPE_STICKER;
	body[1] = APP_PROTO_VERSION;
	/* MSB-first -- see P2P_JOIN_REQ_BODY_LEN in app_radio_p2p.h for why this is a
	 * plain memcpy and not LoRaMac's OTAA byte order. */
	memcpy(&body[2], g_app_config.lrw_deveui, sizeof(g_app_config.lrw_deveui));
	body[10] = APP_VERSION_MAJOR;
	body[11] = APP_VERSION_MINOR;
	body[12] = APP_VERSION_PATCH;
	body[13] = 0; /* reserved */

	/* tag = CMAC(app_key, label || header || body); header+body are already
	 * contiguous in frame[0 .. P2P_HDR_LEN+P2P_JOIN_REQ_BODY_LEN). */
	uint8_t tag_in[sizeof(P2P_JOIN_TAG_LABEL) - 1 + P2P_HDR_LEN + P2P_JOIN_REQ_BODY_LEN];

	memcpy(tag_in, P2P_JOIN_TAG_LABEL, sizeof(P2P_JOIN_TAG_LABEL) - 1);
	memcpy(&tag_in[sizeof(P2P_JOIN_TAG_LABEL) - 1], frame, P2P_HDR_LEN + P2P_JOIN_REQ_BODY_LEN);

	uint8_t tag[P2P_JOIN_TAG_LEN];

	(void)app_ccm_cmac(g_app_config.lrw_appkey, tag_in, sizeof(tag_in), tag);
	memcpy(&frame[P2P_HDR_LEN + P2P_JOIN_REQ_BODY_LEN], tag, P2P_JOIN_TAG_LEN);
}

#if defined(CONFIG_ZTEST)
/* Test hooks for the join identity (#417 / GitLab #73). They build the frame
 * and derive the key through exactly the code the radio path uses, so the KAT
 * vectors in tests/p2p_logic pin the shipped bytes rather than a re-spelling
 * of them. Neither transmits. */
void p2p_test_build_join_request(uint32_t dev_nonce, uint8_t out[P2P_JOIN_REQ_LEN])
{
	join_request_build(dev_nonce, out);
}

void p2p_test_derive_session_key(uint32_t dev_nonce, uint32_t central_nonce,
				 uint8_t out[P2P_KEY_LEN])
{
	derive_session_key(dev_nonce, central_nonce, out);
}
#endif /* defined(CONFIG_ZTEST) */

/* Send one JoinRequest (doc/p2p.md §5.3): header net_id=0/dev_addr=0,
 * counter=dev_nonce; CLEARTEXT body product_type|proto_version|dev_eui(8)|
 * fw_version (nothing secret in it -- it is the central's lookup key)
 * followed by a full 16 B plain AES-CMAC tag = CMAC(app_key,
 * P2P_JOIN_TAG_LABEL || header || body) -- see the P2P_JOIN_TAG_LABEL
 * comment above; deliberately NOT AES-CCM, there is no ciphertext and no
 * nonce involved at all (#118 phase 2 revision, proximos-v2 MR!7 §7).
 * Persists the advanced dev_nonce BEFORE sending: once a JoinRequest *could*
 * have reached the central, that nonce value must never be reused, even if
 * the TX or the round-trip afterward fails. Returns 0 (with
 * `*used_nonce`/`*tx_end_ms` set) or -EAGAIN (duty-cycle blocked) or an
 * errno. */
static int send_join_request(uint32_t *used_nonce, int64_t *tx_end_ms)
{
	if (duty_wait_ms_for(P2P_JOIN_REQ_LEN) > 0) {
		return -EAGAIN;
	}

	uint32_t nonce_val = m_dev_nonce;

	/* Fail closed (review of #400): a dev_nonce the flash does not hold would
	 * be presented again after a reboot, and a recorded JoinAccept for it
	 * replayed -- the old session key back with the counter at 0. */
	if (dnonce_persist(nonce_val + 1) != 0) {
		return -EIO;
	}

	uint8_t frame[P2P_JOIN_REQ_LEN]; /* 42 B */

	join_request_build(nonce_val, frame);

	app_radio_air_begin(); /* until the JoinAccept window closed */
	int ret = lora_send(m_lora_dev, frame, sizeof(frame));
	if (ret) {
		app_radio_air_end();
		LOG_ERR_CALL_FAILED_INT("lora_send", ret);
		return tx_send_failed(sizeof(frame));
	}

	int64_t end = k_uptime_get();
	uint32_t air = frame_toa_ms(sizeof(frame));

	duty_charge(air);
	app_radio_count(APP_RADIO_CNT_JOIN);
	publish_link();

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
	size_t want = P2P_JOIN_ACCEPT_LEN; /* 43 B (12 B header since decision #22) */

	int len = p2p_rx_window(tx_end_ms, P2P_RX1_DELAY_DEFAULT_S, (uint8_t)want, buf, sizeof(buf),
				&rssi, &snr);

	if (len < 0) {
		return len;
	}

	if ((size_t)len != want) {
		LOG_WRN("JoinAccept: unexpected length %d (want %zu)", len, want);
		return -EBADMSG;
	}

	struct p2p_hdr hdr;

	p2p_hdr_get(buf, &hdr);

	/* FCtrl is not checked: RFU bits are ignored on receipt, and the tag
	 * below covers the whole header anyway. */
	if (hdr.net_id != P2P_PREJOIN_NET_ID || hdr.dev_addr != P2P_PREJOIN_DEV_ADDR ||
	    hdr.frame_type != APP_RADIO_P2P_FRAME_JOIN_ACCEPT || hdr.counter != dev_nonce) {
		LOG_WRN("JoinAccept: header mismatch (type %u, ctr %u, want ctr %u)",
			hdr.frame_type, hdr.counter, dev_nonce);
		return -EBADMSG;
	}

	/* Body is CLEARTEXT (see the P2P_JOIN_TAG_LABEL comment above) -- verify
	 * the trailing 16 B plain AES-CMAC tag = CMAC(app_key,
	 * P2P_JOINACCEPT_TAG_LABEL || header || body) before trusting it. */
	uint8_t tag_in[sizeof(P2P_JOINACCEPT_TAG_LABEL) - 1 + P2P_HDR_LEN +
		       P2P_JOIN_ACCEPT_BODY_LEN];

	memcpy(tag_in, P2P_JOINACCEPT_TAG_LABEL, sizeof(P2P_JOINACCEPT_TAG_LABEL) - 1);
	memcpy(&tag_in[sizeof(P2P_JOINACCEPT_TAG_LABEL) - 1], buf,
	       P2P_HDR_LEN + P2P_JOIN_ACCEPT_BODY_LEN);

	uint8_t expected_tag[P2P_JOIN_TAG_LEN];

	(void)app_ccm_cmac(g_app_config.lrw_appkey, tag_in, sizeof(tag_in), expected_tag);

	if (!p2p_tag_eq(expected_tag, &buf[P2P_HDR_LEN + P2P_JOIN_ACCEPT_BODY_LEN])) {
		LOG_WRN("JoinAccept: auth failed");
		return -EBADMSG;
	}

	const uint8_t *body = &buf[P2P_HDR_LEN];
	uint32_t net_id = sys_get_be32(&body[0]);
	uint16_t dev_addr = sys_get_be16(&body[4]);
	uint32_t central_nonce = sys_get_be32(&body[6]);
	uint8_t rx1_delay_s = body[10];

	if (rx1_delay_s < 1 || rx1_delay_s > P2P_RX1_DELAY_MAX_S) {
		LOG_WRN("JoinAccept: rx1_delay %u s outside 1..%u, refused", rx1_delay_s,
			P2P_RX1_DELAY_MAX_S);
		return -EBADMSG;
	}

	/* body[11..14] = reserved(4): the central's radio assignment (D3). */
	struct p2p_radio_assign assign;

	p2p_parse_join_accept_reserved(&body[11], &assign);

	/* SF is network-wide: the NorthBridge has a single receiver, so a
	 * per-node SF would simply make this node unhearable. The byte stays a
	 * documented hook -- warn and keep ours. */
	if (assign.sf_hint != 0 && assign.sf_hint != m_sf) {
		LOG_WRN("JoinAccept assigns SF%u: SF is network-wide, keeping SF%u", assign.sf_hint,
			m_sf);
	}

	uint8_t session_key[P2P_KEY_LEN];

	derive_session_key(dev_nonce, central_nonce, session_key);

	int ret = pairing_persist(net_id, dev_addr, session_key, rx1_delay_s, &assign);

	if (ret) {
		return ret;
	}
	/* The sweep may have landed this join on an SF the config does not name;
	 * record it before anything else can reboot us into the stale one. */
	(void)p2p_join_adopt_sf(m_sf);

	LOG_INF("Joined: net_id=%u dev_addr=%u rx1_delay=%us (RSSI %d dBm, SNR %d dB)", net_id,
		dev_addr, rx1_delay_s, rssi, snr);
	if (assign.tx_power_assigned) {
		LOG_INF("Session TX power assigned: %d dBm (config %d dBm)", assign.tx_power_dbm,
			g_app_config.p2p_tx_power);
	}
	return 0;
}

/* §5.2: the never-paired boot join's 120 s window is over. It ends the FAST
 * retry policy, not the episode -- the node keeps looking, on the same
 * exponential curve a self-heal uses (§7), converging to one sweep pass an
 * hour. Going UNPAIRED and silent here is what stranded a node switched on
 * before its Hub, or after the Hub moved the network SF: nothing short of a
 * power cycle would ever have brought it back.
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

	if (m_join_episode_fresh) {
		join_episode_begin();
	}

	uint32_t used_nonce;
	int64_t tx_end;
	int ret = send_join_request(&used_nonce, &tx_end);
	/* Captured before recv_join_accept() overwrites `ret`: only the SEND can
	 * report the duty ledger's refusal, and that refusal is the one outcome
	 * that tried no SF at all. */
	bool duty_blocked = (ret == -EAGAIN);
	bool sent = (ret == 0);

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

	/* Only a JoinRequest that reached the air consumes an attempt at this SF.
	 * A duty bounce tried nothing and waits for the ledger; a hard radio fault
	 * tried nothing either, but must still END the round so the slow policy
	 * backs off -- advancing neither the sweep nor the round would retry a dead
	 * modem every jitter interval, with a dev_nonce flash write each time. */
	bool pass_end = sent ? join_sweep_advance() : !duty_blocked;

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
		/* No window on the slow policy (§7). Waits INSIDE a pass are short
		 * whatever the policy -- the sweep is the point of the pass, and
		 * spreading one over the backoff curve would mean an SF got tried
		 * once an hour. The curve is charged between passes instead, which
		 * is the only place the two policies differ. */
		if (pass_end) {
			base = app_radio_rejoin_backoff_ms(m_rejoin_attempt);
		}
		wait_ms = p2p_join_retry_delay_ms(true, 0, duty_wait_ms, base,
						  P2P_JOIN_RETRY_JITTER_MS);
	}

	if (m_join_slow && pass_end) {
		/* Exponential backoff between passes, +/-25% jitter. A duty-cycle-
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
	k_work_init_delayable(&m_ack_retry_work, ack_retry_work_handler);
	started = true;
}

/* Put the module into a fresh boot-policy JOINING episode configured for
 * `cfg_sf`, with the duty ledger empty and the episode already opened, so a
 * test can read the state the first JoinRequest will go out on before it
 * drives a single step. */
void p2p_test_join_setup(int cfg_sf)
{
	test_queue_start_once();
	g_app_config.p2p_spreading_factor = cfg_sf;
	p2p_duty_init(&m_duty);
	m_link_state = P2P_LINK_JOINING;
	m_join_slow = false;
	set_rejoin_attempt(0);
	m_join_started_at = k_uptime_get();
	m_join_episode_fresh = false;
	m_join_sweep_step = 0;
	m_join_sf_attempts = 0;
	m_join_sweep_epoch = k_uptime_get();
	join_set_sf(p2p_join_sweep_sf(cfg_sf, 0));
}

/* A pending clock_sync forces confirmed reports (PF-2). */
void p2p_test_link_reset(void)
{
	atomic_clear(&m_clock_sync_pending);
	atomic_clear(&m_clock_sync_reports);
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

/* Put the last-resort sweep in reach (a day without a JoinAccept). */
void p2p_test_allow_join_sweep(void)
{
	m_join_sweep_epoch = k_uptime_get() - P2P_JOIN_LAST_RESORT_MS;
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

/* Record a downlink as recv_ack() does after authenticating one. */
void p2p_test_note_downlink(int16_t rssi, int8_t snr)
{
	note_downlink(rssi, snr);
}

/* Put the link where a node that was paired under older firmware boots: PAIRED
 * from the persisted record, but not yet started. */
void p2p_test_set_paired(void)
{
	m_link_state = P2P_LINK_PAIRED;
	m_started = false;
}

void p2p_test_tx_reset(void)
{
	k_work_cancel_delayable(&m_ack_retry_work);
	k_msgq_purge(&m_ack_retry_msgq);
	m_link_idle_at = 0;
}

/* Stand in for a confirmed uplink whose Ack retry is still pending (F-P1-1). */
void p2p_test_put_ack_retry(uint32_t counter)
{
	struct p2p_ack_retry_state st = {.counter = counter};

	(void)k_msgq_put(&m_ack_retry_msgq, &st, K_NO_WAIT);
}

void p2p_test_put_ack_retry_frame(uint8_t type, const uint8_t *body, size_t len, uint32_t counter)
{
	struct p2p_ack_retry_state st = {
		.frame_type = type, .body_len = (uint16_t)len, .counter = counter};

	memcpy(st.body, body, len);
	(void)k_msgq_put(&m_ack_retry_msgq, &st, K_NO_WAIT);
}

uint32_t p2p_test_ack_retry_count(void)
{
	return k_msgq_num_used_get(&m_ack_retry_msgq);
}

/* What pairing_persist() does to the retry queue when a session is replaced. */
void p2p_test_drop_old_session(void)
{
	ack_retry_drop_old_session();
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

void p2p_test_get_join(uint8_t *sf, uint8_t *step, uint8_t *attempts, bool *slow, uint8_t *rejoin,
		       enum p2p_link_state *state)
{
	if (sf) {
		*sf = m_sf;
	}
	if (step) {
		*step = m_join_sweep_step;
	}
	if (attempts) {
		*attempts = m_join_sf_attempts;
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

/* The live duty ledger, so a test can fill it and make send_join_request()
 * return -EAGAIN for real rather than through a stub. */
struct p2p_duty *p2p_test_get_duty(void)
{
	return &m_duty;
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

	p2p_duty_init(&m_duty);

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

	/* The configured SF is also the discovered one: a join that swept onto a
	 * different SF persisted it here (p2p_join_adopt_sf), so a PAIRED boot --
	 * which app_radio_p2p_start() takes without joining -- comes up on the SF the
	 * network is actually using. If that persist had failed, this boots on the
	 * stale value, the uplinks go unacknowledged, and the self-heal episode's
	 * sweep re-discovers it after P2P_REJOIN_FAIL_THRESHOLD cycles: slow
	 * recovery, not a brick, which is why the failure is logged at ERR rather
	 * than being treated as fatal. */
	m_sf = (uint8_t)sf_from_cfg();

	ret = radio_configure(true);
	if (ret) {
		return ret;
	}

	k_work_init_delayable(&m_join_work, join_work_handler);
	k_work_init_delayable(&m_ack_retry_work, ack_retry_work_handler);
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
	 * lrw_appkey. Such a node would take the PAIRED shortcut and resume
	 * transmitting under a session it can never re-derive. */
	if (!app_key_is_set()) {
		LOG_ERR("P2P not started: lrw_appkey is all-zero (device unprovisioned). "
			"Set lrw-appkey over NFC or shell, then reboot.");
		m_disabled = true;
		return;
	}

	if (m_link_state == P2P_LINK_PAIRED) {
		/* Persisted pairing from a prior boot: no re-join needed (§7 --
		 * a session survives normal power cycles). */
		mark_ready();
		return;
	}

	/* Below the PAIRED shortcut on purpose: the DevEUI is a join identity
	 * (#417), not a session input -- derive_session_key() and
	 * join_request_build() are the only readers, and a session persisted
	 * before it was set is self-contained (P2P_JOIN_STATE_LEN is unchanged,
	 * so join_settings_set() still accepts the old 24 B record). Every path
	 * that would start a NEW join carries its own guard. And unlike
	 * lrw_appkey it survives factory_reset (app_config.yml: persistent
	 * [device_reset, factory_reset]), so the ordering argument above does
	 * not transfer to this gate. */
	if (!dev_eui_is_set()) {
		LOG_ERR("P2P not started: lrw_deveui is all-zero (device unprovisioned). "
			"Set lrw-deveui over NFC or shell, then reboot.");
		m_disabled = true;
		return;
	}

	/* Unpaired: kick off the boot-window join handshake (#118 phase 2,
	 * §5.2). app_radio_p2p_is_ready() only goes true once JoinAccept lands
	 * (mark_ready(), called from join_work_handler()). */
	start_join_episode(false);
}

bool app_radio_p2p_is_ready(void)
{
	/* m_started alone stayed true through a self-heal / RejoinRequest join, so
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
		/* The slow policy runs for a self-heal / RejoinRequest episode and
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
	 * any pending m_join_work retry, #118 phase 2) is torn down with the
	 * reboot that follows deep-sleep entry, same as app_radio_lrw_suspend() relies
	 * on for its own timers. No-op today; kept as an explicit facade hook. */
}

void app_radio_p2p_get_info(struct app_radio_p2p_info *info)
{
	info->link_state = m_link_state;
	info->net_id = m_net_id;
	info->dev_addr = m_dev_addr;
	info->rx1_delay_s = m_rx1_delay_s;
	info->sf = m_sf;
	info->tx_power_assigned = m_session_tx_power_assigned;
	info->tx_power_dbm = m_session_tx_power_assigned ? m_session_tx_power_dbm
							 : (int8_t)g_app_config.p2p_tx_power;
	info->fcnt = m_fcnt;
	info->dev_nonce = m_dev_nonce;
	info->ack_retry_pending = k_msgq_num_used_get(&m_ack_retry_msgq);
	info->last_ack_rssi = m_last_ack_rssi;
	info->last_ack_snr = m_last_ack_snr;
	info->last_ack_valid = m_last_ack_valid;
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
	/* Same gate as app_radio_p2p_start() -- the shell must not be a way around it
	 * (a JoinRequest tagged under an all-zero app_key is forgeable by
	 * anyone; see app_key_is_set()). */
	if (!app_key_is_set()) {
		LOG_ERR("P2P rejoin refused: lrw_appkey is all-zero (device unprovisioned)");
		return;
	}
	if (!dev_eui_is_set()) {
		LOG_ERR("P2P rejoin refused: lrw_deveui is all-zero (device unprovisioned)");
		return;
	}

	/* Explicit operator-forced fresh join: boot-window policy, not self-heal. */
	start_join_episode(false);
}

/* ---- clock_sync hook (app_radio, doc/plan/439 T3) -------------------------- */

void app_radio_p2p_clock_sync(uint32_t seq)
{
	/* seq before the flag, so the Ack path never pairs a stale seq. */
	atomic_set(&m_clock_sync_seq, (atomic_val_t)seq);
	atomic_clear(&m_clock_sync_reports);
	atomic_set(&m_clock_sync_pending, 1);
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

void app_radio_p2p_debug_set_rx1_delay(uint8_t rx1_delay_s)
{
	m_rx1_delay_s = rx1_delay_s;
	LOG_WRN("Debug: rx1_delay override -> %u s (not persisted)", rx1_delay_s);
}

void app_radio_p2p_debug_drop_acks(uint32_t count)
{
	m_debug_drop_acks = count;
	LOG_WRN("Debug: forcing next %u Ack(s) to appear dropped", count);
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

	uint8_t body[P2P_MAX_BODY];
	size_t body_len = 0;

	res->ret = app_compose_budget(body, sizeof(body), &body_len, &res->more, P2P_MAX_BODY);
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

	res->ret = build_frame(APP_RADIO_P2P_FRAME_TELEMETRY, P2P_FCTRL_CONFIRMED, body, body_len,
			       m_fcnt, res->frame);
	res->frame_len = P2P_HDR_LEN + body_len + P2P_TAG_LEN;
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
