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
	 *                 sensors{cap_barometer=true} } (config regroup) */
	enum app_cmd_action a = handle("0801120d0a021801120220782203e80201", &r);

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
	handle("080312041202200a", &r);

	zassert_equal(r.which_body, Response_error_tag, "expected Error, which=%d", r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_OUT_OF_RANGE, "code %d",
		      r.body.error.code);
	zassert_equal(r.body.error.fault_field, 4, "fault_field %u", r.body.error.fault_field);
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

	/* seq2 get_param{ lorawan_field=[3 adr], application_field=[4 ireport],
	 *                 sensors_field=[45 cap_barometer] } */
	handle("08021a090a01031201041a012d", &r);

	zassert_equal(r.which_body, Response_config_dump_tag, "expected ConfigDump, which=%d",
		      r.which_body);
	zassert_true(r.body.config_dump.lorawan.has_adr, "adr not dumped");
	zassert_true(r.body.config_dump.lorawan.adr, "adr value");
	zassert_true(r.body.config_dump.application.has_interval_report, "ireport not dumped");
	zassert_equal(r.body.config_dump.application.interval_report, 120, "ireport value");
	zassert_true(r.body.config_dump.sensors.has_cap_barometer, "cap_barometer not dumped");
	zassert_true(r.body.config_dump.sensors.cap_barometer, "cap_barometer value");
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

/* force_send / clock_sync / req_history answer only via a LoRaWAN uplink, so
 * over NFC they must be rejected with an Error (NOT_READY) rather than firing a
 * side effect the NFC caller can never observe. */
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

	/* clock_sync (field 12, empty body) — seq=5. */
	reset_cfg();
	handle_via(APP_CMD_TRANSPORT_NFC, "08056200", &r);
	zassert_equal(r.which_body, Response_error_tag, "clock_sync/NFC should error (which=%d)",
		      r.which_body);

	/* req_history (field 11) — seq=7, empty window. */
	reset_cfg();
	handle_via(APP_CMD_TRANSPORT_NFC, "08075a00", &r);
	zassert_equal(r.which_body, Response_error_tag, "req_history/NFC should error (which=%d)",
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
