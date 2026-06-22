/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for app_cmd: command dispatch, real range validation, and the
 * deferred-action contract. Vectors are current-schema DownlinkCommand frames.
 */

#include "app_cmd.h"
#include "app_config.h"
#include "app_config_ingest.h"

#include <pb_decode.h>
#include "src/app_config.pb.h"

#include <zephyr/ztest.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

extern struct app_config g_app_config;
extern bool test_clock_has;
extern uint32_t test_clock_unix;

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = 0;
	for (; hex[0] && hex[1] && n < cap; hex += 2, n++) {
		char b[3] = {hex[0], hex[1], 0};
		out[n] = (uint8_t)strtoul(b, NULL, 16);
	}
	return n;
}

/* Run app_cmd_handle on a hex command over `transport`; decode the Response. */
static enum app_cmd_action handle_via(enum app_cmd_transport transport, const char *hex,
				      Response *resp)
{
	uint8_t in[64], out[128];
	size_t in_len = unhex(hex, in, sizeof(in));
	size_t out_len = 0;
	enum app_cmd_action action = APP_CMD_ACTION_NONE;

	int ret = app_cmd_handle(transport, in, in_len, out, sizeof(out), &out_len, &action);
	zassert_equal(ret, 0, "app_cmd_handle ret %d", ret);

	/* out[0] is the APP_PROTO_VERSION prefix (#55); decode the protobuf after it. */
	zassert_true(out_len >= 1, "missing version byte");
	zassert_equal(out[0], APP_PROTO_VERSION, "bad version 0x%02x", out[0]);
	*resp = (Response)Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, resp), "Response decode failed");
	return action;
}

/* Most tests exercise the LoRaWAN transport. */
static enum app_cmd_action handle(const char *hex, Response *resp)
{
	return handle_via(APP_CMD_TRANSPORT_LRW, hex, resp);
}

static void reset_cfg(void)
{
	memset(&g_app_config, 0, sizeof(g_app_config));
	test_clock_has = false;
}

ZTEST(cmd, test_set_param_applies_and_acks)
{
	Response r;

	reset_cfg();
	/* seq1 set_param{ lorawan.adr=true, application{interval_report=120},
	 *                 sensors{cap_barometer=true} } (contiguous #166 ids) */
	enum app_cmd_action a = handle("0801120c0a0220011202187822023001", &r);

	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action expected");
	zassert_equal(r.which_body, Response_ack_tag, "expected Ack, which=%d", r.which_body);
	zassert_equal(r.seq, 1, "seq");
	/* config applied through the real ingest path (4 submessages -> flat struct) */
	zassert_true(g_app_config.lrw_adr, "adr not applied");
	zassert_equal(g_app_config.interval_report, 120, "interval_report not applied");
	zassert_true(g_app_config.cap_barometer, "cap_barometer not applied");
}

ZTEST(cmd, test_set_param_out_of_range)
{
	Response r;

	reset_cfg();
	/* seq3 set_param{ application{ interval_report=10 } } — below min 60 */
	handle("080312041202180a", &r);

	zassert_equal(r.which_body, Response_error_tag, "expected Error, which=%d", r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_OUT_OF_RANGE, "code %d",
		      r.body.error.code);
	zassert_equal(r.body.error.fault_field, 3, "fault_field %u", r.body.error.fault_field);
	/* invalid value must NOT be applied */
	zassert_not_equal(g_app_config.interval_report, 10, "out-of-range value leaked");
}

ZTEST(cmd, test_get_param_config_dump)
{
	Response r;

	reset_cfg();
	g_app_config.lrw_adr = true;
	g_app_config.interval_report = 120;
	g_app_config.cap_barometer = true;

	/* seq2 get_param{ lorawan_field=[4 adr], application_field=[3 ireport],
	 *                 sensors_field=[6 cap_barometer] } */
	handle("08021a090a01041201031a0106", &r);

	zassert_equal(r.which_body, Response_config_dump_tag, "expected ConfigDump, which=%d",
		      r.which_body);
	zassert_true(r.body.config_dump.lorawan.has_adr, "adr not dumped");
	zassert_true(r.body.config_dump.lorawan.adr, "adr value");
	zassert_true(r.body.config_dump.application.has_interval_report, "ireport not dumped");
	zassert_equal(r.body.config_dump.application.interval_report, 120, "ireport value");
	zassert_true(r.body.config_dump.sensors.has_cap_barometer, "cap_barometer not dumped");
	zassert_true(r.body.config_dump.sensors.cap_barometer, "cap_barometer value");
}

/* get_param pages like get_config when the requested fields don't fit one DR0
 * response (#93.3). deveui(18 B) + joineui(18 B) + devaddr(10 B) = 46 B > the
 * 30 B page budget, so they split: page 0 = {deveui}, page 1 = {joineui,
 * devaddr}, page_count = 2. */
ZTEST(cmd, test_get_param_paging)
{
	Response r;

	/* seq2 get_param{ lorawan_field=[6 deveui, 7 joineui, 10 devaddr] } — page omitted (0). */
	reset_cfg();
	handle("08021a050a0306070a", &r);
	zassert_equal(r.which_body, Response_config_dump_tag, "page0 which=%d", r.which_body);
	zassert_equal(r.body.config_dump.page_index, 0, "page0 index");
	zassert_equal(r.body.config_dump.page_count, 2, "page_count %u",
		      r.body.config_dump.page_count);
	zassert_true(r.body.config_dump.lorawan.has_deveui, "deveui on page0");
	zassert_false(r.body.config_dump.lorawan.has_joineui, "joineui must not be on page0");
	zassert_false(r.body.config_dump.lorawan.has_devaddr, "devaddr must not be on page0");

	/* Same request with page=1 (field 5 varint = 0x28 0x01). */
	reset_cfg();
	handle("08021a070a0306070a2801", &r);
	zassert_equal(r.which_body, Response_config_dump_tag, "page1 which=%d", r.which_body);
	zassert_equal(r.body.config_dump.page_index, 1, "page1 index");
	zassert_equal(r.body.config_dump.page_count, 2, "page1 count");
	zassert_false(r.body.config_dump.lorawan.has_deveui, "deveui must not be on page1");
	zassert_true(r.body.config_dump.lorawan.has_joineui, "joineui on page1");
	zassert_true(r.body.config_dump.lorawan.has_devaddr, "devaddr on page1");

	/* Out-of-range page → Error. */
	reset_cfg();
	handle("08021a070a0306070a2805", &r);
	zassert_equal(r.which_body, Response_error_tag, "oob page should Error, which=%d",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_OUT_OF_RANGE, "code %d",
		      r.body.error.code);
}

/* The LoRaWAN crypto keys are read-back over NFC only — never over LoRaWAN
 * (the fPort-85 payload is plain protobuf). A get_param requesting nwkkey (tag 8)
 * must dump it over NFC and omit it over LoRaWAN. (#162) */
ZTEST(cmd, test_get_param_keys_nfc_only)
{
	Response r;

	reset_cfg();
	g_app_config.lrw_nwkkey[0] = 0xAB;
	g_app_config.lrw_nwkkey[15] = 0xCD;

	/* seq3 get_param{ lorawan_field=[8 nwkkey] } */
	const char *cmd = "08031a030a0108";

	/* NFC: key is dumped. */
	handle_via(APP_CMD_TRANSPORT_NFC, cmd, &r);
	zassert_equal(r.which_body, Response_config_dump_tag, "NFC which=%d", r.which_body);
	zassert_true(r.body.config_dump.has_lorawan, "NFC: lorawan section missing");
	zassert_true(r.body.config_dump.lorawan.has_nwkkey, "NFC: nwkkey not dumped");
	zassert_equal(r.body.config_dump.lorawan.nwkkey[0], 0xAB, "NFC: nwkkey[0]");
	zassert_equal(r.body.config_dump.lorawan.nwkkey[15], 0xCD, "NFC: nwkkey[15]");

	/* LoRaWAN: key must NOT be dumped (omitted entirely → empty lorawan section). */
	handle_via(APP_CMD_TRANSPORT_LRW, cmd, &r);
	zassert_equal(r.which_body, Response_config_dump_tag, "LRW which=%d", r.which_body);
	zassert_false(r.body.config_dump.lorawan.has_nwkkey, "LRW: nwkkey leaked over LoRaWAN!");
}

ZTEST(cmd, test_build_info)
{
	uint8_t out[128];
	size_t out_len = 0;

	reset_cfg();
	g_app_config.serial_number = 1234567890;

	int ret = app_cmd_build_info(out, sizeof(out), &out_len);
	zassert_equal(ret, 0, "build_info ret %d", ret);

	/* Skip the APP_PROTO_VERSION prefix (#55). */
	zassert_equal(out[0], APP_PROTO_VERSION, "bad version 0x%02x", out[0]);
	Response r = Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "decode");
	zassert_equal(r.which_body, Response_info_tag, "expected Info");
	zassert_equal(r.body.info.serial_number, 1234567890, "serial");
}

ZTEST(cmd, test_deferred_actions)
{
	Response r;

	reset_cfg();
	zassert_equal(handle("08043200", &r), APP_CMD_ACTION_SETTINGS_SAVE, "settings_save");
	zassert_equal(r.which_body, Response_ack_tag, "save acks");

	zassert_equal(handle("08083a00", &r), APP_CMD_ACTION_REBOOT, "reboot");
	zassert_equal(handle("08094200", &r), APP_CMD_ACTION_FACTORY_RESET, "factory");
}

/* force_send / req_history answer only via a LoRaWAN uplink, so over NFC they
 * must be rejected with an Error (NOT_READY) rather than firing a side effect
 * the NFC caller can never observe. (clock_sync is NOT here — it runs over NFC,
 * see test_clock_sync_over_nfc.) */
ZTEST(cmd, test_lrw_only_commands_rejected_over_nfc)
{
	Response r;

	/* force_send (field 9, empty body) — seq=6. */
	reset_cfg();
	handle_via(APP_CMD_TRANSPORT_NFC, "08064a00", &r);
	zassert_equal(r.which_body, Response_error_tag, "force_send/NFC should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_READY, "code %d",
		      r.body.error.code);

	/* req_history (field 11) — seq=7, empty window. */
	reset_cfg();
	handle_via(APP_CMD_TRANSPORT_NFC, "08075a00", &r);
	zassert_equal(r.which_body, Response_error_tag, "req_history/NFC should error (which=%d)",
		      r.which_body);
}

/* #107: clock_sync carrying unix_time sets the RTC directly (NFC time bootstrap).
 * A bare clock_sync over NFC just acks here — there is no LoRaWAN in this build
 * to query for network time. */
ZTEST(cmd, test_clock_sync_over_nfc)
{
	Response r;

	/* clock_sync{ unix_time = 1735689600 } over NFC -> sets the RTC, answers Info. */
	reset_cfg();
	handle_via(APP_CMD_TRANSPORT_NFC, "0801620608808bd2bb06", &r);
	zassert_equal(r.which_body, Response_info_tag, "clock_sync+unix/NFC -> Info (which=%d)",
		      r.which_body);
	zassert_equal(test_clock_unix, 1735689600u, "RTC not set (%u)", test_clock_unix);
	zassert_equal(r.body.info.unix_time, 1735689600u, "Info.unix_time %u",
		      r.body.info.unix_time);

	/* Out-of-range epoch (unix_time = 1) -> BAD_REQUEST, RTC left untouched. */
	reset_cfg();
	handle_via(APP_CMD_TRANSPORT_NFC, "080162020801", &r);
	zassert_equal(r.which_body, Response_error_tag, "bad epoch -> error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_BAD_REQUEST, "code %d",
		      r.body.error.code);
	zassert_false(test_clock_has, "RTC must stay unset after a rejected epoch");

	/* Bare clock_sync over NFC (no unix_time, seq=5) just acks (no network). */
	reset_cfg();
	handle_via(APP_CMD_TRANSPORT_NFC, "08056200", &r);
	zassert_equal(r.which_body, Response_ack_tag, "bare clock_sync/NFC -> ack (which=%d)",
		      r.which_body);
}

/* #89: a fully-populated history frame (48 samples, synced 5-byte t0) must fit
 * the staging buffer. The pre-fix 64-byte buffer overflowed (~68 B encoded),
 * killing replay silently on DR3+. */
ZTEST(cmd, test_history_frame_full_fits_buffer)
{
	uint8_t samples[48];
	uint8_t out[APP_CMD_HISTORY_FRAME_BUF_SIZE];
	size_t out_len = 0;

	memset(samples, 0xAB, sizeof(samples));
	int ret = app_cmd_build_history_frame(/*seq*/ 200, /*idx*/ 200, /*count*/ 200,
					      /*t0*/ 1770000000u, /*present*/ 0x7, /*interval*/ 900,
					      samples, sizeof(samples), out, sizeof(out), &out_len);
	zassert_equal(ret, 0, "full frame did not fit (ret %d)", ret);
	zassert_true(out_len <= sizeof(out), "out_len %zu overflows", out_len);

	Response r = Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "decode");
	zassert_equal(r.which_body, Response_history_frame_tag, "expected HistoryFrame");
	zassert_equal(r.body.history_frame.samples.size, 48, "sample count");
}

/* The capacity helper must be exact: a frame built with `cap` samples fits the
 * given out_cap, and one more byte does not. */
ZTEST(cmd, test_history_sample_capacity_is_exact)
{
	uint8_t samples[64];
	uint8_t out[APP_CMD_HISTORY_FRAME_BUF_SIZE];
	size_t out_len = 0;

	memset(samples, 0x5A, sizeof(samples));

	/* Worst-case varints, mirroring history_frame_cap() in app_lrw.c. */
	size_t cap = app_cmd_history_sample_capacity(200, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0x7,
						     900, sizeof(out));
	zassert_true(cap > 0 && cap <= 48, "cap %zu out of range", cap);

	/* Exactly `cap` samples must encode within out_cap. */
	int ret = app_cmd_build_history_frame(200, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0x7, 900,
					      samples, cap, out, sizeof(out), &out_len);
	zassert_equal(ret, 0, "cap samples did not fit (ret %d)", ret);
	zassert_true(out_len <= sizeof(out), "out_len %zu > out_cap", out_len);

	/* A tighter budget yields a strictly smaller (or zero) capacity. */
	size_t tight = app_cmd_history_sample_capacity(200, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0x7,
						       900, 32);
	zassert_true(tight < cap, "tight cap %zu not below %zu", tight, cap);

	/* A budget below the fixed overhead yields zero. */
	zassert_equal(app_cmd_history_sample_capacity(200, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0x7,
						      900, 8),
		      0, "tiny budget should give 0");
}

ZTEST_SUITE(cmd, NULL, NULL, NULL, NULL, NULL);
