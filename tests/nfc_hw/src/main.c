/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * native_sim suite linking the REAL app_nfc.c against an emulated ST25DV (see
 * emul_st25dv.c) — issue #361. First two tests establish that the harness
 * itself works; the rest cover the claim window (#415): a provisioned device is
 * ACTIVE by default and a decrypted command — secret_key or vendor_token — no
 * longer closes it (the #308 implicit close is gone; only claim_done does).
 */

#include "app_nfc.h"
#include "app_cmd.h"
#include "app_config.h"

#include <zephyr/ztest.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "emul_st25dv.h"

/* Claim window states are exposed as APP_NFC_CLAIM_ACTIVE / APP_NFC_CLAIM_DONE
 * (app_nfc.h); app_nfc_claim_state_get() returns the raw uint8_t. */

static void nfc_hw_before(void *fixture)
{
	ARG_UNUSED(fixture);
	st25dv_emul_reset();
	memset(&g_app_config, 0, sizeof(g_app_config));
	/* app_nfc.c's claim state (m_claim_state) is a private static that survives
	 * across tests in the same ztest binary — CONFIG_SETTINGS_NONE makes
	 * app_nfc_init()'s settings_load_subtree("clm") a no-op, so it does NOT
	 * reset to the ACTIVE default on its own. Force it back explicitly. */
	app_nfc_claim_active();
}

ZTEST(nfc_hw, test_init_succeeds_on_empty_tag)
{
	zassert_equal(app_nfc_init(), 0, "app_nfc_init failed against the emulated ST25DV");
}

/* ---- FTM mailbox (#313) ---------------------------------------------------- */

#define GPO_RF_PUT_MSG_EN 0x10
#define GPO_RF_WRITE_EN   0x40
#define GPO_EN            0x80
#define MB_CTRL_MB_EN     0x01
#define MB_CTRL_HOST_PUT  0x02

/* AES-CCM(secret_key = 000102..0f, serial 0, counter 1..2) of Command{get_info}
 * and AES-CCM(vendor_token = 101112..1f, counter 3) of the same — direction
 * request, from sticker_nfc_frame.py, same wire contract as tests/nfc_crypto. */
#define KEY_HEX        "000102030405060708090a0b0c0d0e0f"
#define VND_KEY_HEX    "101112131415161718191a1b1c1d1e1f"
#define GETINFO_C1     "00000000000000019798f777cd12b7b425c5893eb72a63479ec93ec8"
#define GETINFO_C2     "0000000000000002a74df0cb60de2c4225f8dd4459690b9125da17ec"
#define VND_GETINFO_C3 "0000000000000003548d0343ec24ecb0eaeadfa4b80aa2b4c35a5376"

static size_t unhex_local(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = 0;

	for (; hex[0] && hex[1] && n < cap; hex += 2, n++) {
		char b[3] = {hex[0], hex[1], 0};

		out[n] = (uint8_t)strtoul(b, NULL, 16);
	}
	return n;
}

/* The "phone": drives the RF side of the mailbox from a cooperative thread so it
 * interleaves with app_nfc_poll()'s (blocking) mb_serve_locked() running in the
 * test thread. Each entry is one [chan][wire] request; the reply is read back
 * and its channel byte captured. After the last request it drops the RF field so
 * the session ends. */
struct mb_phone {
	const uint8_t *const *reqs;
	const size_t *req_lens;
	size_t n;
	uint8_t reply_chan[8];
	size_t reply_len[8];
	int put_err[8];
};

static K_THREAD_STACK_DEFINE(phone_stack, 3072);
static struct k_thread phone_thread;

static void mb_phone_fn(void *a, void *b, void *c)
{
	struct mb_phone *ph = a;
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	k_msleep(50); /* let app_nfc_poll() reach its serve loop first */
	st25dv_emul_rf_set_mb_en(true);

	for (size_t i = 0; i < ph->n; i++) {
		ph->put_err[i] = st25dv_emul_rf_put_message(ph->reqs[i], ph->req_lens[i]);
		if (ph->put_err[i]) {
			continue;
		}
		/* Poll for the firmware's reply (bounded). */
		uint8_t reply[256];
		size_t rlen = 0;

		for (int spin = 0; spin < 150; spin++) {
			if (st25dv_emul_rf_read_message(reply, sizeof(reply), &rlen) == 0) {
				ph->reply_chan[i] = reply[0];
				ph->reply_len[i] = rlen;
				break;
			}
			k_msleep(5);
		}
	}

	/* Give a rejected-frame case a moment to be seen as "no reply", then drop the
	 * field so mb_serve_locked() exits and app_nfc_poll() returns. */
	k_msleep(50);
	st25dv_emul_set_field_on(false);
}

/* Init with the mailbox authorised + a field present, and the crypto identity
 * the golden frames are sealed against. */
static void mb_bring_up(const char *key_hex)
{
	zassert_equal(app_nfc_init(), 0, "app_nfc_init failed");
	zassert_true(app_nfc_mailbox_available(), "MB_MODE should be authorised at boot");

	g_app_config.serial_number = 0;
	g_app_config.nonce_counter = 0;
	if (key_hex) {
		unhex_local(key_hex, g_app_config.secret_key, sizeof(g_app_config.secret_key));
	}
	unhex_local(VND_KEY_HEX, g_app_config.vendor_token, sizeof(g_app_config.vendor_token));
	/* Field stays OFF here; the caller turns the RF field on for the mailbox
	 * session. The claim window is ACTIVE by default (nfc_hw_before). */
}

static struct mb_phone run_phone(const uint8_t *const *reqs, const size_t *lens, size_t n)
{
	static struct mb_phone ph;

	ph = (struct mb_phone){.reqs = reqs, .req_lens = lens, .n = n};

	k_thread_create(&phone_thread, phone_stack, K_THREAD_STACK_SIZEOF(phone_stack), mb_phone_fn,
			&ph, NULL, NULL, K_PRIO_COOP(1), 0, K_NO_WAIT);
	int ret = app_nfc_poll();

	zassert_true(ret == 0, "app_nfc_poll returned %d", ret);
	k_thread_join(&phone_thread, K_FOREVER);
	return ph;
}

ZTEST(nfc_hw, test_mb_boot_authorises_ftm_and_configures_gpo)
{
	zassert_equal(app_nfc_init(), 0, "app_nfc_init failed");

	zassert_true(app_nfc_mailbox_available(), "mailbox should be available");
	zassert_true(st25dv_emul_mb_mode(), "MB_MODE (static) must be set at boot");

	uint8_t gpo = st25dv_emul_gpo_reg();

	zassert_true(gpo & GPO_RF_PUT_MSG_EN, "GPO RF_PUT_MSG_EN must be set (0x%02x)", gpo);
	zassert_true(gpo & GPO_EN, "GPO_EN must be set (0x%02x)", gpo);
	zassert_true(gpo & GPO_RF_WRITE_EN, "GPO RF_WRITE_EN must be set (0x%02x)", gpo);
	zassert_false(st25dv_emul_mb_ctrl() & MB_CTRL_MB_EN, "MB_EN must be 0 at boot");

	struct app_cmd_info info;

	app_cmd_get_info(&info);
	zassert_false(info.device_status & APP_DEVICE_STATUS_MAILBOX_DOWN,
		      "MAILBOX_DOWN must be clear on a healthy unit");
}

ZTEST(nfc_hw, test_mb_boot_pwd_fail_marks_unavailable)
{
	st25dv_emul_set_pwd_fail(true);

	zassert_equal(app_nfc_init(), 0, "app_nfc_init must still succeed (degraded)");
	zassert_false(app_nfc_mailbox_available(),
		      "mailbox must be unavailable when MB_MODE fails");
	zassert_false(st25dv_emul_mb_mode(), "MB_MODE must not be set when the password fails");

	struct app_cmd_info info;

	app_cmd_get_info(&info);
	zassert_true(info.device_status & APP_DEVICE_STATUS_MAILBOX_DOWN,
		     "device_status must flag MAILBOX_DOWN (bit 13) for the production tester");
}

ZTEST(nfc_hw, test_mb_boot_clears_stuck_mb_en)
{
	/* Simulate a mailbox left enabled by an aborted session that survived a
	 * reset. First init authorises MB_MODE; then force MB_EN on (as a stuck
	 * session would leave it) and check a second init (a reboot) clears it. */
	zassert_equal(app_nfc_init(), 0, "first init");
	st25dv_emul_rf_set_mb_en(true);
	zassert_true(st25dv_emul_mb_ctrl() & MB_CTRL_MB_EN, "precondition: MB_EN set");

	/* A second init (a reboot) must clear the stuck MB_EN before touching EEPROM. */
	zassert_equal(app_nfc_init(), 0, "second init");
	zassert_false(st25dv_emul_mb_ctrl() & MB_CTRL_MB_EN,
		      "boot must clear a stuck MB_EN (0x%02x)", st25dv_emul_mb_ctrl());
}

ZTEST(nfc_hw, test_mb_session_cmd_keeps_claim_active_and_advances_nonce)
{
	memset(g_app_config.claim_token, 0xAB, sizeof(g_app_config.claim_token));
	mb_bring_up(KEY_HEX);
	zassert_equal(app_nfc_claim_state_get(), APP_NFC_CLAIM_ACTIVE,
		      "a provisioned unit is claim-active by default");
	st25dv_emul_set_field_on(true); /* now the phone arrives */

	uint8_t req[64];
	size_t rl = unhex_local(GETINFO_C1, req, sizeof(req));
	uint8_t frame[65];

	frame[0] = 0x01; /* chan: owner command */
	memcpy(&frame[1], req, rl);
	const uint8_t *reqs[] = {frame};
	const size_t lens[] = {rl + 1};

	struct mb_phone ph = run_phone(reqs, lens, 1);

	zassert_equal(ph.put_err[0], 0, "RF put failed: %d", ph.put_err[0]);
	zassert_true(ph.reply_len[0] > 1, "no reply received (%zu B)", ph.reply_len[0]);
	zassert_equal(ph.reply_chan[0], 0x01, "reply channel byte");
	zassert_equal(g_app_config.nonce_counter, 1, "nonce must advance to 1");
	zassert_equal(app_nfc_claim_state_get(), APP_NFC_CLAIM_ACTIVE,
		      "#415: a decrypted owner command must NOT close the claim window");
}

ZTEST(nfc_hw, test_mb_session_vendor_keeps_claim_active)
{
	memset(g_app_config.claim_token, 0xAB, sizeof(g_app_config.claim_token));
	mb_bring_up(KEY_HEX);
	zassert_equal(app_nfc_claim_state_get(), APP_NFC_CLAIM_ACTIVE,
		      "a provisioned unit is claim-active by default");
	st25dv_emul_set_field_on(true); /* now the phone arrives */

	uint8_t req[64];
	size_t rl = unhex_local(VND_GETINFO_C3, req, sizeof(req));
	uint8_t frame[65];

	frame[0] = 0x02; /* chan: vendor command */
	memcpy(&frame[1], req, rl);
	g_app_config.nonce_counter = 2; /* accept counter 3 */
	const uint8_t *reqs[] = {frame};
	const size_t lens[] = {rl + 1};

	struct mb_phone ph = run_phone(reqs, lens, 1);

	zassert_equal(ph.put_err[0], 0, "RF put failed: %d", ph.put_err[0]);
	zassert_true(ph.reply_len[0] > 1, "no reply received");
	zassert_equal(ph.reply_chan[0], 0x02, "reply channel byte");
	zassert_equal(app_nfc_claim_state_get(), APP_NFC_CLAIM_ACTIVE,
		      "a vendor command leaves the claim window active (#316/#415)");
}

ZTEST(nfc_hw, test_mb_bad_channel_prefix_rejected)
{
	mb_bring_up(KEY_HEX);
	st25dv_emul_set_field_on(true);

	uint8_t req[64];
	size_t rl = unhex_local(GETINFO_C1, req, sizeof(req));
	uint8_t frame[65];

	/* 0x04 is not a defined channel (0x01 owner / 0x02 vendor / 0x03 plaintext);
	 * an unknown prefix gets no reply and advances nothing. */
	frame[0] = 0x04;
	memcpy(&frame[1], req, rl);
	const uint8_t *reqs[] = {frame};
	const size_t lens[] = {rl + 1};

	struct mb_phone ph = run_phone(reqs, lens, 1);

	zassert_equal(ph.put_err[0], 0, "RF put failed");
	zassert_equal(ph.reply_len[0], 0, "an unknown channel must get no reply (%zu B)",
		      ph.reply_len[0]);
	zassert_equal(g_app_config.nonce_counter, 0, "a rejected frame must not advance the nonce");
}

/* #415/#313: the plaintext channel 0x03 routes a raw Command to the plain_text
 * transport. get_basic_info (the mailbox-only replacement for the inf record)
 * answers with no key and no nonce advance. */
ZTEST(nfc_hw, test_mb_session_plain_get_basic_info)
{
	memset(g_app_config.claim_token, 0xAB, sizeof(g_app_config.claim_token));
	g_app_config.serial_number = 0x12345678;
	mb_bring_up(KEY_HEX);
	st25dv_emul_set_field_on(true);

	/* [0x03] Command{ seq=1, get_basic_info={} } — field 30 (0xF2 0x01), empty. */
	uint8_t frame[] = {0x03, 0x08, 0x01, 0xF2, 0x01, 0x00};
	const uint8_t *reqs[] = {frame};
	const size_t lens[] = {sizeof(frame)};

	struct mb_phone ph = run_phone(reqs, lens, 1);

	zassert_equal(ph.put_err[0], 0, "RF put failed: %d", ph.put_err[0]);
	zassert_true(ph.reply_len[0] > 1, "no basic_info reply (%zu B)", ph.reply_len[0]);
	zassert_equal(ph.reply_chan[0], 0x03, "reply channel byte");
	zassert_equal(g_app_config.nonce_counter, 0, "plain_text must not touch the nonce");
}

/* ---- Field-present hold bounds (#414 review) ------------------------------- */

/* AES-CCM(secret_key = KEY_HEX, serial 0) of Command{seq=1, settings_save={}} at
 * counter 1 and Command{seq=2, reboot={}} at counter 2 — direction request, from
 * sticker_nfc_frame.py (same wire contract as GETINFO_C1/C2 above). */
#define SETTINGS_SAVE_C1 "00000000000000019798e777424a9f4ff48ecc4bde43a43564b14f84"
#define REBOOT_C2        "0000000000000002a74de8cb01a98c6fc3706b64ccf2cf0abd7d2de9"

/* [0x03] Command{ seq=1, get_basic_info={} } — see test_mb_session_plain_get_basic_info. */
static const uint8_t PLAIN_GET_BASIC_INFO[] = {0x03, 0x08, 0x01, 0xF2, 0x01, 0x00};

/* A second cooperative thread for the hold tests: sleeps `delay_ms` while the
 * test thread sits in app_nfc_poll()'s field-present hold, then either probes
 * app_cmd_get_info() (the m_work_q GetInfo path) or runs one plaintext mailbox
 * exchange, recording when it finished. It never drops the field — the test
 * decides whether the hold must end on its own. */
struct hold_probe {
	int32_t delay_ms;
	bool exchange; /* true: one get_basic_info exchange; false: app_cmd_get_info() */
	int64_t done_ms;
	uint32_t device_status;
	size_t reply_len;
};

static void hold_probe_fn(void *a, void *b, void *c)
{
	struct hold_probe *pr = a;
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	k_msleep(pr->delay_ms);
	if (pr->exchange) {
		st25dv_emul_rf_set_mb_en(true);
		if (st25dv_emul_rf_put_message(PLAIN_GET_BASIC_INFO,
					       sizeof(PLAIN_GET_BASIC_INFO)) == 0) {
			uint8_t reply[256];

			for (int spin = 0; spin < 150; spin++) {
				if (st25dv_emul_rf_read_message(reply, sizeof(reply),
								&pr->reply_len) == 0) {
					break;
				}
				k_msleep(5);
			}
		}
	} else {
		struct app_cmd_info info;

		app_cmd_get_info(&info);
		pr->device_status = info.device_status;
	}
	pr->done_ms = k_uptime_get();
}

static int64_t run_hold(struct hold_probe *pr)
{
	int64_t t0 = k_uptime_get();

	k_thread_create(&phone_thread, phone_stack, K_THREAD_STACK_SIZEOF(phone_stack),
			hold_probe_fn, pr, NULL, NULL, K_PRIO_COOP(1), 0, K_NO_WAIT);
	int ret = app_nfc_poll();
	int64_t elapsed = k_uptime_get() - t0;

	zassert_equal(ret, 0, "app_nfc_poll returned %d", ret);
	k_thread_join(&phone_thread, K_FOREVER);
	pr->done_ms -= t0;
	return elapsed;
}

/* Finding 1: app_cmd_get_info() runs on m_work_q (GetInfo-on-join, the
 * clock-sync Info, a LoRaWAN get_info downlink) and reads the claim state. It
 * must not wait on the NFC access lock the poll thread holds for a whole
 * field-present hold / mailbox session — m_work_q's 30 s liveness heartbeat would
 * go stale and the IWDG reset the device. */
ZTEST(nfc_hw, test_get_info_does_not_wait_on_a_held_field)
{
	memset(g_app_config.claim_token, 0xAB, sizeof(g_app_config.claim_token));
	mb_bring_up(KEY_HEX);
	st25dv_emul_set_field_on(true); /* a phone parked on the tag, no mailbox */

	struct hold_probe pr = {.delay_ms = 1000, .exchange = false};

	run_hold(&pr);
	st25dv_emul_set_field_on(false);

	zassert_true(pr.done_ms < 2000,
		     "app_cmd_get_info() blocked on the NFC lock until %lld ms (probe at 1000 ms)",
		     (long long)pr.done_ms);
	zassert_true(pr.device_status & APP_DEVICE_STATUS_CLAIM_ACTIVE,
		     "claim-active bit read while the poll thread held the tag");
}

/* Finding 2: a field held with no mailbox traffic (a phone left lying on the
 * STICKER) must not keep the chip powered, the CPU out of Stop2 and the access
 * lock taken forever — the hold ends after 120 s. */
ZTEST(nfc_hw, test_field_held_without_mailbox_releases_after_120s)
{
	mb_bring_up(KEY_HEX);
	st25dv_emul_set_field_on(true);

	struct hold_probe pr = {.delay_ms = 0, .exchange = false};
	int64_t elapsed = run_hold(&pr);

	st25dv_emul_set_field_on(false);
	zassert_true(elapsed >= 120000 && elapsed < 122000,
		     "field-present hold ended after %lld ms (expected ~120 s)",
		     (long long)elapsed);
}

/* Finding 2, "longer communication": mailbox traffic restarts the 120 s hold, so
 * an exchange late in a long tap is served and the release comes 120 s after it,
 * not 120 s after the tap started. */
ZTEST(nfc_hw, test_field_hold_restarts_after_mailbox_traffic)
{
	mb_bring_up(KEY_HEX);
	st25dv_emul_set_field_on(true);

	struct hold_probe pr = {.delay_ms = 100000, .exchange = true};
	int64_t elapsed = run_hold(&pr);

	st25dv_emul_set_field_on(false);
	zassert_true(pr.reply_len > 1, "the exchange at 100 s got no reply (%zu B)", pr.reply_len);
	zassert_true(elapsed >= 100000 + 120000 && elapsed < 100000 + 120000 + 6000,
		     "hold ended after %lld ms (expected ~120 s after the exchange)",
		     (long long)elapsed);
}

/* Finding 2: without FTM authorised the phone can never enable the mailbox, so
 * app_nfc_poll() returns at once instead of holding the field-present loop. */
ZTEST(nfc_hw, test_poll_returns_at_once_when_mailbox_unavailable)
{
	st25dv_emul_set_pwd_fail(true);
	zassert_equal(app_nfc_init(), 0, "app_nfc_init must still succeed (degraded)");
	zassert_false(app_nfc_mailbox_available(), "precondition: mailbox unavailable");
	st25dv_emul_set_field_on(true);

	int64_t t0 = k_uptime_get();

	zassert_equal(app_nfc_poll(), 0, "app_nfc_poll");
	int64_t elapsed = k_uptime_get() - t0;

	st25dv_emul_set_field_on(false);
	zassert_true(elapsed < 100, "app_nfc_poll held the tag for %lld ms", (long long)elapsed);
}

/* Finding 3: the phone for the deferred-action test. Sends settings_save, reads
 * the Ack, then — still holding its field — re-enables the mailbox the firmware
 * just cleared and sends a reboot, as a phone driver that re-arms MB_EN would. */
struct action_phone {
	size_t reply1_len;
	int put2_err;
	size_t reply2_len;
};

static void action_phone_fn(void *a, void *b, void *c)
{
	struct action_phone *ph = a;
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uint8_t frame[65];
	uint8_t reply[256];
	size_t rl;

	k_msleep(50); /* let app_nfc_poll() reach its hold loop first */
	st25dv_emul_rf_set_mb_en(true);
	frame[0] = 0x01;
	rl = unhex_local(SETTINGS_SAVE_C1, &frame[1], sizeof(frame) - 1);
	if (st25dv_emul_rf_put_message(frame, rl + 1) == 0) {
		for (int spin = 0; spin < 150; spin++) {
			if (st25dv_emul_rf_read_message(reply, sizeof(reply), &ph->reply1_len) ==
			    0) {
				break;
			}
			k_msleep(5);
		}
	}

	/* Wait for the firmware to close the session (clears MB_EN), then re-arm it. */
	for (int spin = 0; spin < 400 && (st25dv_emul_mb_ctrl() & MB_CTRL_MB_EN); spin++) {
		k_msleep(5);
	}
	st25dv_emul_rf_set_mb_en(true);
	rl = unhex_local(REBOOT_C2, &frame[1], sizeof(frame) - 1);
	ph->put2_err = st25dv_emul_rf_put_message(frame, rl + 1);
	for (int spin = 0; spin < 200; spin++) {
		if (st25dv_emul_rf_read_message(reply, sizeof(reply), &ph->reply2_len) == 0) {
			break;
		}
		k_msleep(5);
	}
	st25dv_emul_set_field_on(false);
}

/* Finding 3: a deferred action ends app_nfc_poll() right after its session, even
 * while the phone still holds the field — so the poll thread runs it at once, and
 * a follow-up command can neither run against the unapplied state nor replace the
 * action (here: a reboot dropping the acked settings save). */
ZTEST(nfc_hw, test_mb_deferred_action_ends_poll_while_field_held)
{
	mb_bring_up(KEY_HEX);
	st25dv_emul_set_field_on(true);

	static struct action_phone ph;

	ph = (struct action_phone){0};
	while (app_nfc_wait_event(0) == 0) {
		/* drain GPO events left by earlier tests: the re-arm check below */
	}
	k_thread_create(&phone_thread, phone_stack, K_THREAD_STACK_SIZEOF(phone_stack),
			action_phone_fn, &ph, NULL, NULL, K_PRIO_COOP(1), 0, K_NO_WAIT);
	zassert_equal(app_nfc_poll(), 0, "app_nfc_poll");
	enum app_cmd_action action = app_nfc_take_cmd_action();

	k_thread_join(&phone_thread, K_FOREVER);

	zassert_true(ph.reply1_len > 1, "settings_save got no Ack (%zu B)", ph.reply1_len);
	zassert_equal(action, APP_CMD_ACTION_SETTINGS_SAVE, "staged action %d replaced", action);
	zassert_equal(g_app_config.nonce_counter, 1, "the follow-up reboot must not have run");
	zassert_equal(ph.reply2_len, 0, "the follow-up command must get no reply");
	zassert_equal(app_nfc_take_cmd_action(), APP_CMD_ACTION_NONE, "action taken once");
	zassert_equal(app_nfc_wait_event(0), 0,
		      "the poll must be re-armed so a non-rebooting action resumes the tap");
}

ZTEST_SUITE(nfc_hw, NULL, NULL, nfc_hw_before, NULL, NULL);
