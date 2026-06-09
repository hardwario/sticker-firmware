/*
 * Copyright (c) 2026 HARDWARIO a.s.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for app_cmd: command dispatch, real range validation, and the
 * deferred-action contract. Vectors are current-schema DownlinkCommand frames.
 */

#include "app_cmd.h"
#include "app_config.h"

#include <pb_decode.h>
#include "src/app_config.pb.h"

#include <zephyr/ztest.h>

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

/* Run app_cmd_handle on a hex command; decode the Response into `resp`. */
static enum app_cmd_action handle(const char *hex, Response *resp)
{
	uint8_t in[64], out[128];
	size_t in_len = unhex(hex, in, sizeof(in));
	size_t out_len = 0;
	enum app_cmd_action action = APP_CMD_ACTION_NONE;

	int ret = app_cmd_handle(APP_CMD_TRANSPORT_LRW, in, in_len, out, sizeof(out), &out_len,
				 &action);
	zassert_equal(ret, 0, "app_cmd_handle ret %d", ret);

	/* out[0] is the APP_PROTO_VERSION prefix (#55); decode the protobuf after it. */
	zassert_true(out_len >= 1, "missing version byte");
	zassert_equal(out[0], APP_PROTO_VERSION, "bad version 0x%02x", out[0]);
	*resp = (Response)Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, resp), "Response decode failed");
	return action;
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
	/* seq1 set_param{ lorawan.adr=true, application{interval_report=120,
	 *                 alarm_temperature_hi=50.0} } */
	enum app_cmd_action a = handle("0801120d0a021801120720783d00004842", &r);

	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action expected");
	zassert_equal(r.which_body, Response_ack_tag, "expected Ack, which=%d", r.which_body);
	zassert_equal(r.seq, 1, "seq");
	/* config applied through the real ingest path */
	zassert_true(g_app_config.lrw_adr, "adr not applied");
	zassert_equal(g_app_config.interval_report, 120, "interval_report not applied");
	zassert_within(g_app_config.alarm_temperature_hi, 50.0f, 0.01f, "alarm hi not applied");
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
	g_app_config.alarm_temperature_hi = 50.0f;

	/* seq2 get_param{ lorawan_field=[3 adr], application_field=[4 ireport, 7 alarm_hi] } */
	handle("08021a070a010312020407", &r);

	zassert_equal(r.which_body, Response_config_dump_tag, "expected ConfigDump, which=%d",
		      r.which_body);
	zassert_true(r.body.config_dump.lorawan.has_adr, "adr not dumped");
	zassert_true(r.body.config_dump.lorawan.adr, "adr value");
	zassert_true(r.body.config_dump.application.has_interval_report, "ireport not dumped");
	zassert_equal(r.body.config_dump.application.interval_report, 120, "ireport value");
	zassert_true(r.body.config_dump.application.has_alarm_temperature_hi, "alarm not dumped");
	zassert_within(r.body.config_dump.application.alarm_temperature_hi, 50.0f, 0.01f, "alarm");
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

ZTEST_SUITE(cmd, NULL, NULL, NULL, NULL, NULL);
