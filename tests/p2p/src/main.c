/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal gw-sim bench firmware for the raw-LoRa P2P transport (#118,
 * doc/p2p.md §14): the "network side" stand-in for the two-STICKER rig. A
 * second STICKER, flashed with THIS firmware instead of the main app, listens
 * for / injects raw P2P frames to/from a device running `radio-mode p2p`.
 *
 * Fully controlled over the RTT shell (`p2p ...` commands below) -- no other
 * UI, no persistence: every reboot starts blank, re-enter identity/session
 * via shell each bench session (a deliberate simplification -- this is a
 * bench tool, not a deployable image).
 *
 * Wire format mirrors app_p2p.c exactly. Kept in sync BY HAND -- this is a
 * deliberately separate, minimal firmware (only app_ccm.c is shared), not a
 * common module, so a wire-format change in app_p2p.c must be mirrored here:
 *
 *   header:  net_id(4 BE) | dev_addr(2 BE) | frame_type(1) | counter(4 BE)
 *
 * Join handshake (JoinRequest 0xF0 / JoinAccept 0xF1, #118 phase 2 revision,
 * proximos-v2 MR!7 §7): CLEARTEXT body + a full 16 B plain AES-CMAC tag =
 * CMAC(app_key, label || header || body) -- NOT AES-CCM, no nonce involved
 * at all (neither frame carries an actual secret). `p2p key` sets app_key --
 * the DUT's existing LoRaWAN OTAA AppKey (read it off the DUT, e.g. `config
 * lrw_appkey`) -- so both sides compute identical tags. There is no longer a
 * separate join_key/secret_key involved in P2P at all.
 *
 * Data plane (telemetry/alarm/response/ack, only once net_id/dev_addr are
 * join-assigned): AES-CCM (AES-128, 4 B tag) under session_key, 11 B header
 * as AAD, nonce = counter(4 BE) | dev_addr(2 BE) | frame_type(1) |
 * direction(1) | 0*5. `p2p session <dev_nonce> <central_nonce>` derives
 * session_key from app_key + those nonces + serial_number (see
 * derive_session_key() below, identical to app_p2p.c's) -- read
 * dev_nonce/central_nonce off the join exchange this sim observed or
 * crafted; this sim does not track a join state machine itself.
 */

#include "app_ccm.h"

/* Zephyr includes */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(p2p_gw_sim, LOG_LEVEL_INF);

#define HDR_LEN   11
#define TAG_LEN   4 /* data-plane (session_key) CCM tag length only */
#define NONCE_LEN 13
#define KEY_LEN   16
#define DIR_TX    0x00 /* device -> network (what the DUT's own frames use) */
#define DIR_RX    0x01 /* network -> device (this sim's default TX direction) */
#define LORA_MTU  255
#define MAX_BODY  (LORA_MTU - HDR_LEN - TAG_LEN) /* 240 */
#define FRAME_MAX (HDR_LEN + MAX_BODY + TAG_LEN)

/* Join handshake constants -- see the file header comment above and
 * app_p2p.c's identical P2P_JOIN_TAG_LABEL/P2P_JOINACCEPT_TAG_LABEL/
 * P2P_SESSION_KEY_LABEL comment for the full rationale. */
#define JOIN_TAG_LEN         16             /* full CMAC output; NOT TAG_LEN */
#define JOIN_TAG_LABEL       "HIO-P2P-JOIN" /* 12 B -- JoinRequest tag */
#define JOINACCEPT_TAG_LABEL "HIO-P2P-ACC"  /* 11 B -- JoinAccept tag */
#define SESSION_KEY_LABEL    "HIO-P2P-SES"
#define JOIN_ACCEPT_BODY_LEN 15 /* largest of the two join body sizes */

#define FRAME_TYPE_JOIN_REQUEST 0xF0
#define FRAME_TYPE_JOIN_ACCEPT  0xF1

static const struct device *const m_lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));

/* Radio config, applied by `p2p radio` and on every listen/tx toggle. Defaults
 * match the main app's p2p config group defaults (app_config.yml). BW125/CR4-5
 * are fixed, same as app_p2p.c -- the DUT and this sim must always agree. */
static uint32_t m_freq = 868100000;
static uint8_t m_sf = SF_10;
static int8_t m_tx_power = 14;

/* DUT identity, set by `p2p key` -- its LoRaWAN OTAA AppKey, the sole root
 * secret for the whole P2P transport (#118 phase 2 revision). */
static uint8_t m_app_key[KEY_LEN];
static uint32_t m_serial_number;
static bool m_key_set;

/* Data-plane key, set by `p2p session` once dev_nonce/central_nonce from the
 * join exchange are known -- see derive_session_key() below. */
static uint8_t m_session_key[KEY_LEN];
static bool m_session_set;

static bool m_listening;
static uint32_t m_tx_counter; /* auto-increment fallback for `p2p tx` */

struct rx_msg {
	uint16_t len;
	int16_t rssi;
	int8_t snr;
	uint8_t buf[FRAME_MAX];
};

K_MSGQ_DEFINE(m_rx_msgq, sizeof(struct rx_msg), 4, 4);
static struct k_work m_rx_work;

/* Constant-time 16 B tag compare (mirrors app_ccm.c's app_ccm_auth_decrypt()
 * and app_p2p.c's p2p_tag_eq()) -- a short-circuiting memcmp() would leak
 * how many leading bytes matched via timing. Not a hard security boundary
 * for THIS bench tool (it isn't a protocol participant an attacker targets),
 * but kept constant-time anyway for consistency with the code it mirrors. */
static bool tag_eq(const uint8_t a[JOIN_TAG_LEN], const uint8_t b[JOIN_TAG_LEN])
{
	uint8_t diff = 0;

	for (size_t i = 0; i < JOIN_TAG_LEN; i++) {
		diff |= a[i] ^ b[i];
	}
	return diff == 0;
}

/* tag = CMAC(app_key, label || hdr_body) -- shared by JoinRequest/JoinAccept
 * crafting (cmd_p2p_tx) and verification (rx_work_handler). `label` a
 * NUL-terminated string; `hdr_body` the frame's header+body (already
 * contiguous in both callers). */
static void cmac_tag(const char *label, const uint8_t *hdr_body, size_t hdr_body_len,
		     uint8_t out[KEY_LEN])
{
	uint8_t buf[16 + HDR_LEN + JOIN_ACCEPT_BODY_LEN]; /* label max 12 B */
	size_t label_len = strlen(label);

	memcpy(buf, label, label_len);
	memcpy(&buf[label_len], hdr_body, hdr_body_len);
	(void)app_ccm_cmac(m_app_key, buf, label_len + hdr_body_len, out);
}

/* session_key = AES128-CMAC(app_key, "HIO-P2P-SES" || 0x01 || dev_nonce(4 BE)
 * || central_nonce(4 BE) || serial_number(4 BE) || zero-pad to 32 B) --
 * identical derivation to app_p2p.c's derive_session_key(). */
static void derive_session_key(uint32_t dev_nonce, uint32_t central_nonce, uint8_t out[KEY_LEN])
{
	uint8_t block[32] = {0};
	size_t label_len = strlen(SESSION_KEY_LABEL);

	memcpy(block, SESSION_KEY_LABEL, label_len);
	block[label_len] = 0x01;
	sys_put_be32(dev_nonce, &block[label_len + 1]);
	sys_put_be32(central_nonce, &block[label_len + 5]);
	sys_put_be32(m_serial_number, &block[label_len + 9]);
	(void)app_ccm_cmac(m_app_key, block, sizeof(block), out);
}

static void build_nonce(uint8_t nonce[NONCE_LEN], uint32_t counter, uint16_t dev_addr,
			uint8_t frame_type, uint8_t dir)
{
	memset(nonce, 0, NONCE_LEN);
	sys_put_be32(counter, &nonce[0]);
	sys_put_be16(dev_addr, &nonce[4]);
	nonce[6] = frame_type;
	nonce[7] = dir;
}

static void to_hex(char *out, const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		snprintf(&out[2 * i], 3, "%02x", buf[i]);
	}
}

static int radio_apply(bool tx)
{
	struct lora_modem_config cfg = {
		.frequency = m_freq,
		.bandwidth = BW_125_KHZ,
		.datarate = (enum lora_datarate)m_sf,
		.coding_rate = CR_4_5,
		.preamble_len = 8,
		.tx_power = m_tx_power,
		.tx = tx,
		.iq_inverted = false,
		.public_network = false,
	};

	return lora_config(m_lora_dev, &cfg);
}

/* ======================================================================== */
/* RX (listen)                                                              */
/* ======================================================================== */

static void rx_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct rx_msg msg;
	static char hex[2 * FRAME_MAX + 1];
	static uint8_t pt[MAX_BODY];
	static char body_hex[2 * MAX_BODY + 1];

	while (k_msgq_get(&m_rx_msgq, &msg, K_NO_WAIT) == 0) {
		to_hex(hex, msg.buf, msg.len);

		if (msg.len < HDR_LEN) {
			LOG_WRN("RX runt frame (%u B): %s", msg.len, hex);
			continue;
		}

		uint32_t net_id = sys_get_be32(&msg.buf[0]);
		uint16_t dev_addr = sys_get_be16(&msg.buf[4]);
		uint8_t frame_type = msg.buf[6];
		uint32_t counter = sys_get_be32(&msg.buf[7]);

		LOG_INF("RX %u B, RSSI %d dBm, SNR %d dB: %s", msg.len, msg.rssi, msg.snr, hex);
		LOG_INF("  net_id=%u dev_addr=%u frame_type=%u counter=%u", net_id, dev_addr,
			frame_type, counter);

		/* JoinRequest/JoinAccept: CLEARTEXT body + 16 B plain CMAC tag, no
		 * CCM/nonce (see the file header comment). */
		if (frame_type == FRAME_TYPE_JOIN_REQUEST || frame_type == FRAME_TYPE_JOIN_ACCEPT) {
			if (!m_key_set) {
				LOG_WRN("  no app_key set (`p2p key`) -- cannot verify");
				continue;
			}
			if (msg.len < HDR_LEN + JOIN_TAG_LEN) {
				LOG_WRN("  runt join frame (%u B)", msg.len);
				continue;
			}

			size_t body_len = msg.len - HDR_LEN - JOIN_TAG_LEN;
			const char *label = frame_type == FRAME_TYPE_JOIN_REQUEST
						    ? JOIN_TAG_LABEL
						    : JOINACCEPT_TAG_LABEL;
			uint8_t expected[KEY_LEN];

			cmac_tag(label, msg.buf, HDR_LEN + body_len, expected);

			bool ok = tag_eq(expected, &msg.buf[HDR_LEN + body_len]);

			to_hex(body_hex, &msg.buf[HDR_LEN], body_len);
			LOG_INF("  %s body (%zu B, cleartext, tag %s): %s",
				frame_type == FRAME_TYPE_JOIN_REQUEST ? "JoinRequest"
								      : "JoinAccept",
				body_len, ok ? "OK" : "BAD", body_hex);
			continue;
		}

		/* Data-plane frame: AES-CCM under session_key. */
		if (!m_session_set) {
			LOG_WRN("  no session_key set (`p2p session`) -- cannot decrypt");
			continue;
		}

		size_t ct_len = msg.len - HDR_LEN - TAG_LEN;
		uint8_t nonce[NONCE_LEN];

		/* The DUT's own frames are TX-direction (device -> network). */
		build_nonce(nonce, counter, dev_addr, frame_type, DIR_TX);

		int ret = app_ccm_auth_decrypt(m_session_key, nonce, NONCE_LEN, msg.buf, HDR_LEN,
					       &msg.buf[HDR_LEN], ct_len,
					       &msg.buf[HDR_LEN + ct_len], TAG_LEN, pt);
		if (ret) {
			LOG_WRN("  auth failed: %d (wrong session_key, or not DIR_TX)", ret);
			continue;
		}

		to_hex(body_hex, pt, ct_len);
		LOG_INF("  body (%zu B): %s", ct_len, body_hex);
	}
}

static void recv_cb(const struct device *dev, uint8_t *data, uint16_t size, int16_t rssi,
		    int8_t snr, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	struct rx_msg msg;

	msg.len = MIN(size, (uint16_t)sizeof(msg.buf));
	msg.rssi = rssi;
	msg.snr = snr;
	memcpy(msg.buf, data, msg.len);

	if (k_msgq_put(&m_rx_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("RX queue full; dropping frame");
		return;
	}
	k_work_submit(&m_rx_work);
}

/* ======================================================================== */
/* Shell commands                                                            */
/* ======================================================================== */

static int cmd_p2p_radio(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	uint32_t freq = (uint32_t)strtoul(argv[1], NULL, 10);
	int sf = atoi(argv[2]);
	int tx_power = atoi(argv[3]);

	if (sf < SF_6 || sf > SF_12) {
		shell_error(sh, "sf must be 6..12");
		return -EINVAL;
	}

	m_freq = freq;
	m_sf = (uint8_t)sf;
	m_tx_power = (int8_t)tx_power;

	/* Re-apply immediately in whatever direction is currently active, so a
	 * mid-session radio change (e.g. tuning SF) takes effect right away. */
	int ret = radio_apply(!m_listening);

	if (ret) {
		shell_error(sh, "lora_config failed: %d", ret);
		return ret;
	}
	shell_print(sh, "radio: %u Hz, SF%d, %d dBm (BW125/CR4-5 fixed)", m_freq, m_sf, m_tx_power);
	return 0;
}

static int cmd_p2p_key(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strlen(argv[1]) != 2 * KEY_LEN) {
		shell_error(sh, "app_key must be %d hex digits", 2 * KEY_LEN);
		return -EINVAL;
	}
	for (int i = 0; i < KEY_LEN; i++) {
		char b[3] = {argv[1][2 * i], argv[1][2 * i + 1], 0};

		m_app_key[i] = (uint8_t)strtoul(b, NULL, 16);
	}
	m_serial_number = (uint32_t)strtoul(argv[2], NULL, 0);
	m_key_set = true;
	m_session_set = false; /* app_key changed; any cached session_key is now stale */

	shell_print(sh, "key set: serial_number=%u", m_serial_number);
	return 0;
}

static int cmd_p2p_session(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (!m_key_set) {
		shell_error(sh, "no app_key set (`p2p key`)");
		return -EINVAL;
	}

	uint32_t dev_nonce = (uint32_t)strtoul(argv[1], NULL, 0);
	uint32_t central_nonce = (uint32_t)strtoul(argv[2], NULL, 0);

	derive_session_key(dev_nonce, central_nonce, m_session_key);
	m_session_set = true;

	shell_print(sh, "session_key set (dev_nonce=%u, central_nonce=%u)", dev_nonce,
		    central_nonce);
	return 0;
}

static int cmd_p2p_listen(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	bool enable;

	if (strcmp(argv[1], "on") == 0) {
		enable = true;
	} else if (strcmp(argv[1], "off") == 0) {
		enable = false;
	} else {
		shell_error(sh, "usage: p2p listen on|off");
		return -EINVAL;
	}

	if (enable == m_listening) {
		return 0;
	}

	int ret;

	if (enable) {
		ret = radio_apply(false);
		if (ret) {
			shell_error(sh, "lora_config failed: %d", ret);
			return ret;
		}
		ret = lora_recv_async(m_lora_dev, recv_cb, NULL);
		if (ret) {
			shell_error(sh, "lora_recv_async failed: %d", ret);
			return ret;
		}
		m_listening = true;
		shell_print(sh, "listen ON (%u Hz, SF%d)", m_freq, m_sf);
	} else {
		(void)lora_recv_async(m_lora_dev, NULL, NULL);
		m_listening = false;
		(void)radio_apply(true);
		shell_print(sh, "listen OFF");
	}
	return 0;
}

static int cmd_p2p_tx(const struct shell *sh, size_t argc, char **argv)
{
	int frame_type = atoi(argv[1]);
	size_t body_hex_len = strlen(argv[2]);

	if (body_hex_len % 2 != 0 || body_hex_len / 2 > MAX_BODY) {
		shell_error(sh, "body must be an even-length hex string, max %d B", MAX_BODY);
		return -EINVAL;
	}
	if (m_listening) {
		shell_error(sh, "stop listening first (`p2p listen off`)");
		return -EBUSY;
	}

	size_t body_len = body_hex_len / 2;
	static uint8_t body[MAX_BODY];

	for (size_t i = 0; i < body_len; i++) {
		char b[3] = {argv[2][2 * i], argv[2][2 * i + 1], 0};

		body[i] = (uint8_t)strtoul(b, NULL, 16);
	}

	/* Optional overrides: default counter auto-increments (fine for ad hoc
	 * testing); default direction is network->device (this sim's usual
	 * role) -- dir only matters for the data-plane's CCM nonce, JOIN_REQUEST/
	 * JOIN_ACCEPT have no direction-dependent construction at all. */
	uint32_t counter = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 0) : m_tx_counter++;
	uint8_t dir = (argc >= 5) ? (uint8_t)atoi(argv[4]) : DIR_RX;

	static uint8_t frame[FRAME_MAX];

	sys_put_be32(0, &frame[0]); /* net_id: this sim always addresses a single
				     * hard-coded DUT at the pre-join value (§5.3) */
	sys_put_be16(0, &frame[4]); /* dev_addr: ditto */
	frame[6] = (uint8_t)frame_type;
	sys_put_be32(counter, &frame[7]);
	memcpy(&frame[HDR_LEN], body, body_len);

	size_t wire_len;

	if (frame_type == FRAME_TYPE_JOIN_REQUEST || frame_type == FRAME_TYPE_JOIN_ACCEPT) {
		if (!m_key_set) {
			shell_error(sh, "no app_key set (`p2p key`)");
			return -EINVAL;
		}

		const char *label = frame_type == FRAME_TYPE_JOIN_REQUEST ? JOIN_TAG_LABEL
									  : JOINACCEPT_TAG_LABEL;
		uint8_t tag[JOIN_TAG_LEN];

		cmac_tag(label, frame, HDR_LEN + body_len, tag);
		memcpy(&frame[HDR_LEN + body_len], tag, JOIN_TAG_LEN);
		wire_len = HDR_LEN + body_len + JOIN_TAG_LEN;
	} else {
		if (!m_session_set) {
			shell_error(sh, "no session_key set (`p2p session`)");
			return -EINVAL;
		}

		uint8_t nonce[NONCE_LEN];

		build_nonce(nonce, counter, 0, (uint8_t)frame_type, dir);

		int ret = app_ccm_encrypt_and_tag(m_session_key, nonce, NONCE_LEN, frame, HDR_LEN,
						  body, body_len, &frame[HDR_LEN],
						  &frame[HDR_LEN + body_len], TAG_LEN);
		if (ret) {
			shell_error(sh, "app_ccm_encrypt_and_tag failed: %d", ret);
			return ret;
		}
		wire_len = HDR_LEN + body_len + TAG_LEN;
	}

	int ret = radio_apply(true);

	if (ret) {
		shell_error(sh, "lora_config failed: %d", ret);
		return ret;
	}
	ret = lora_send(m_lora_dev, frame, wire_len);
	if (ret) {
		shell_error(sh, "lora_send failed: %d", ret);
		return ret;
	}

	static char hex[2 * FRAME_MAX + 1];

	to_hex(hex, frame, wire_len);
	shell_print(sh, "TX %zu B (type=%d counter=%u dir=%u): %s", wire_len, frame_type, counter,
		    dir, hex);
	return 0;
}

static int cmd_p2p_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "radio:   %u Hz, SF%d, %d dBm (BW125/CR4-5 fixed)", m_freq, m_sf,
		    m_tx_power);
	if (m_key_set) {
		shell_print(sh, "key:     set, serial_number=%u", m_serial_number);
	} else {
		shell_print(sh, "key:     NOT SET (`p2p key <app_key 32hex> <serial_number>`)");
	}
	if (m_session_set) {
		shell_print(sh, "session: set");
	} else {
		shell_print(sh, "session: NOT SET (`p2p session <dev_nonce> <central_nonce>`)");
	}
	shell_print(sh, "listen:  %s", m_listening ? "ON" : "OFF");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_p2p,
	SHELL_CMD_ARG(radio, NULL,
		      "Set radio params. Usage: radio <freq_hz> <sf 6-12> <tx_power_dbm>",
		      cmd_p2p_radio, 4, 0),
	SHELL_CMD_ARG(key, NULL,
		      "Set the DUT identity (its LoRaWAN OTAA AppKey) to authenticate the "
		      "join handshake with. Usage: key <app_key 32hex> <serial_number>",
		      cmd_p2p_key, 3, 0),
	SHELL_CMD_ARG(session, NULL,
		      "Derive session_key for post-join data-plane frames from "
		      "dev_nonce/central_nonce read off the join exchange. "
		      "Usage: session <dev_nonce> <central_nonce>",
		      cmd_p2p_session, 3, 0),
	SHELL_CMD_ARG(listen, NULL,
		      "Continuous RX: verify/decrypt + log DUT frames. Usage: listen on|off",
		      cmd_p2p_listen, 2, 0),
	SHELL_CMD_ARG(tx, NULL,
		      "Craft + send a frame: JOIN_REQUEST(0xF0)/JOIN_ACCEPT(0xF1) get a "
		      "cleartext body + CMAC tag under app_key, any other type gets "
		      "AES-CCM under session_key. "
		      "Usage: tx <frame_type> <hex_body> [counter] [dir 0|1]",
		      cmd_p2p_tx, 3, 2),
	SHELL_CMD_ARG(status, NULL, "Show current config.", cmd_p2p_status, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(p2p, &sub_p2p, "P2P gateway-simulator bench commands (#118).", NULL);

int main(void)
{
	if (!device_is_ready(m_lora_dev)) {
		LOG_ERR("LoRa device not ready");
		return 0;
	}

	k_work_init(&m_rx_work, rx_work_handler);

	LOG_INF("P2P gw-sim ready (doc/p2p.md §14) -- try `p2p status`");
	return 0;
}
