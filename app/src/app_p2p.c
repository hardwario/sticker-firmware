/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_ccm.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_log.h"
#include "app_p2p.h"
#include "app_wdog.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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
 * Phase 1 (doc/p2p.md §13): net_id/dev_addr are the fixed pre-join value 0
 * (§5.3's own JoinRequest convention) -- join-assigned allocation is phase 2.
 * The body is AES-CCM'd under `join_key`, derived on demand from `secret_key`
 * (§4), never a manual config secret.
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

/* Phase 1: no join yet, so net_id/dev_addr are the fixed pre-join value (§5.3). */
#define P2P_NET_ID   0
#define P2P_DEV_ADDR 0

/* join_key = AES128-ECB(secret_key, "HIO-P2P-JOIN" || serial_number(4 BE)),
 * doc/p2p.md §4 -- a one-block PRF over the hardware AES already in flash. Used
 * directly as the phase-1 frame key (no session established yet); phase 2's
 * real JoinRequest/JoinAccept reuse the identical derivation. */
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

static const struct device *const m_lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));

/* Shallower than app_lrw.c's own m_work_q (4096 B, sized for LoRaMac's deep call
 * stacks): this queue's handlers only do raw lora_send()/lora_config(), AES-CCM
 * (app_ccm) and app_compose_budget() (whose Telemetry frame is a module static,
 * not stack-allocated), so 2048 B (the pre-#265 LoRaWAN default) is ample. */
static K_THREAD_STACK_DEFINE(m_work_stack, 2048);
static struct k_work_q m_work_q;
static struct k_work m_send_work;         /* compose + send telemetry */
static struct k_work_delayable m_tx_work; /* drain response/alarm queue, retries on -EAGAIN */
#if defined(CONFIG_SHELL)
static struct k_work m_rx_work; /* drain received frames (listen) */
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

/* ======================================================================== */
/* Frame TX                                                                 */
/* ======================================================================== */

static void build_nonce(uint8_t nonce[P2P_NONCE_LEN], uint32_t counter, uint8_t frame_type,
			uint8_t dir)
{
	memset(nonce, 0, P2P_NONCE_LEN);
	sys_put_be32(counter, &nonce[0]);
	sys_put_be16(P2P_DEV_ADDR, &nonce[4]);
	nonce[6] = frame_type;
	nonce[7] = dir;
}

/* Frame + encrypt + transmit one body. Returns 0, -EAGAIN (duty cycle), or errno. */
static int tx_frame(uint8_t frame_type, const uint8_t *body, size_t body_len)
{
	if (m_listening) {
		LOG_WRN("TX skipped: radio in listen mode");
		return -EBUSY;
	}
	if (body_len > P2P_MAX_BODY) {
		LOG_ERR("Body %zu B over P2P budget %d B", body_len, P2P_MAX_BODY);
		return -EMSGSIZE;
	}

	int64_t now = k_uptime_get();

	if (now < m_dc_blocked_until) {
		LOG_WRN("TX duty-cycle blocked for %lld ms", m_dc_blocked_until - now);
		return -EAGAIN;
	}

	uint8_t frame[P2P_FRAME_MAX];
	uint32_t counter = fcnt_next();

	sys_put_be32(P2P_NET_ID, &frame[0]);
	sys_put_be16(P2P_DEV_ADDR, &frame[4]);
	frame[6] = frame_type;
	sys_put_be32(counter, &frame[7]);

	uint8_t nonce[P2P_NONCE_LEN];

	build_nonce(nonce, counter, frame_type, P2P_DIR_TX);

	uint8_t key[P2P_KEY_LEN];

	derive_join_key(key);

	int ret = app_ccm_encrypt_and_tag(key, nonce, P2P_NONCE_LEN, /* AAD */ frame, P2P_HDR_LEN,
					  body, body_len, &frame[P2P_HDR_LEN],
					  &frame[P2P_HDR_LEN + body_len], P2P_TAG_LEN);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("app_ccm_encrypt_and_tag", ret);
		return ret;
	}

	size_t wire_len = P2P_HDR_LEN + body_len + P2P_TAG_LEN;

	ret = lora_send(m_lora_dev, frame, wire_len);
	if (ret) {
		LOG_ERR_CALL_FAILED_INT("lora_send", ret);
		return ret;
	}

	/* Charge the air-time against the 1% budget. */
	uint32_t air = frame_toa_ms((uint8_t)wire_len);

	m_dc_blocked_until = now + (int64_t)air * (1000 / P2P_DUTY_CYCLE_PERMILLE - 1);

	LOG_INF("TX type %u, %zu B (counter %u, %u ms air)", frame_type, wire_len, counter, air);
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
		if (tx_frame(APP_P2P_FRAME_TELEMETRY, buf, len) != 0) {
			return; /* duty cycle / radio error — drop the rest of the snapshot */
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
		int ret = tx_frame(msg.type, msg.buf, msg.len);

		if (ret == -EAGAIN) {
			/* Stash and retry this exact frame once the duty-cycle window
			 * clears, instead of dropping it — see m_tx_deferred's comment.
			 * Stop draining the rest of the queue until this one is sent,
			 * so frames stay in order. */
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

		if (net_id != P2P_NET_ID) {
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
		LOG_INF("P2P listen: ON (net_id=%u)", P2P_NET_ID);
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

	ret = radio_configure(true);
	if (ret) {
		return ret;
	}

	k_work_queue_init(&m_work_q);
	k_work_queue_start(&m_work_q, m_work_stack, K_THREAD_STACK_SIZEOF(m_work_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);

	k_work_init(&m_send_work, send_work_handler);
	k_work_init_delayable(&m_tx_work, tx_work_handler);
#if defined(CONFIG_SHELL)
	k_work_init(&m_rx_work, rx_work_handler);
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
	m_started = true;
	if (m_ready_cb) {
		m_ready_cb();
	}
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
	/* Nothing queued survives a poweroff; the work queue itself is torn down
	 * with the reboot that follows deep-sleep entry, same as app_lrw_suspend()
	 * relies on for its own timers. No-op today; kept as an explicit facade
	 * hook so a future phase 2 heartbeat/timer has a defined shutdown point. */
}
