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
#include "app_sensor.h"

#include <pb_decode.h>
#include "src/app_config.pb.h"

#include <zephyr/ztest.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

extern struct app_config g_app_config;
extern bool test_clock_has;
extern uint32_t test_clock_unix;
extern float test_battery_v;
extern int test_battery_ret;
extern void test_set_lrw_dirty(bool v);

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
	test_set_lrw_dirty(false);
}

ZTEST(cmd, test_set_param_applies_and_acks)
{
	Response r;

	reset_cfg();
	/* seq1 set_param{ application{interval_report=120}, sensors{cap_barometer=true} }
	 * over LRW — the non-identity groups stay remotely writable. The lorawan group is
	 * excluded here because it is LRW-write-blocked (H-3); see
	 * test_set_param_lorawan_rejected_over_lrw. */
	enum app_cmd_action a = handle("080112081202187822023001", &r);

	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action expected");
	zassert_equal(r.which_body, Response_ack_tag, "expected Ack, which=%d", r.which_body);
	zassert_equal(r.seq, 1, "seq");
	/* config applied through the real ingest path (submessages -> flat struct) */
	zassert_equal(g_app_config.interval_report, 120, "interval_report not applied");
	zassert_true(g_app_config.cap_barometer, "cap_barometer not applied");
}

/* M-3: a SetParam writing a lorawan provisioning/identity field is rejected over a
 * LoRaWAN downlink per-field (the network server must never be able to rewrite the
 * DevEUI / root keys / DevAddr), reported as NOT_WRITABLE with fault_field pointing
 * at the offending field; the whole batch rolls back. Accepted over NFC (writable
 * includes nfc). Enforcement is now generated from app_config.yml `writable`, not a
 * hand-coded group guard. */
ZTEST(cmd, test_set_param_lorawan_rejected_over_lrw)
{
	Response r;

	/* seq1 set_param{ lorawan.adr=true, application{interval_report=120},
	 *                 sensors{cap_barometer=true} }. lorawan is applied first, so
	 *  adr (tag 4) trips the write-transport reject → fault_field = 1*100 + 4. */
	const char *hex = "0801120c0a0220011202187822023001";

	reset_cfg();
	enum app_cmd_action a = handle_via(APP_CMD_TRANSPORT_LRW, hex, &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action expected");
	zassert_equal(r.which_body, Response_error_tag, "expected Error, which=%d", r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_WRITABLE, "code %d",
		      r.body.error.code);
	zassert_equal(r.body.error.fault_field, 104, "fault_field %u (want 104 = lorawan adr)",
		      r.body.error.fault_field);
	/* the whole message is rejected — nothing is applied */
	zassert_false(g_app_config.lrw_adr, "lorawan applied over LRW despite guard");
	zassert_not_equal(g_app_config.interval_report, 120,
			  "message applied over LRW despite guard");

	/* same payload over NFC is accepted and applies every group */
	reset_cfg();
	a = handle_via(APP_CMD_TRANSPORT_NFC, hex, &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action expected");
	zassert_equal(r.which_body, Response_ack_tag, "expected Ack over NFC, which=%d",
		      r.which_body);
	zassert_true(g_app_config.lrw_adr, "lorawan.adr not applied over NFC");
	zassert_equal(g_app_config.interval_report, 120, "interval_report not applied over NFC");
	zassert_true(g_app_config.cap_barometer, "cap_barometer not applied over NFC");
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
	/* #196: fault_field encodes group*100 + tag (group 2 = application,
	 * interval_report tag 3) so the host can disambiguate the tag across groups. */
	zassert_equal(r.body.error.fault_field, 203, "fault_field %u", r.body.error.fault_field);
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
 * response (#93.3). After #192 the byte-field dump estimate is the native
 * fixed_length size (deveui/joineui = 10 B, devaddr = 6 B), so the old
 * deveui+joineui+devaddr trio now fits a single 30 B page. To still exercise the
 * split we request deveui + joineui + five small lorawan fields (region…activation,
 * 2 B each) + devaddr, in that order: deveui(10)+joineui(10)+5×2 = 30 fills page 0,
 * devaddr(6) overflows to page 1 → page_count = 2. */
ZTEST(cmd, test_get_param_paging)
{
	Response r;

	/* seq2 get_param{ lorawan_field=[6 deveui, 7 joineui, 1,2,3,4,5, 10 devaddr] }, page 0. */
	reset_cfg();
	handle("08021a0a0a08060701020304050a", &r);
	zassert_equal(r.which_body, Response_config_dump_tag, "page0 which=%d", r.which_body);
	zassert_equal(r.body.config_dump.page_index, 0, "page0 index");
	zassert_equal(r.body.config_dump.page_count, 2, "page_count %u",
		      r.body.config_dump.page_count);
	zassert_true(r.body.config_dump.lorawan.has_deveui, "deveui on page0");
	zassert_true(r.body.config_dump.lorawan.has_joineui, "joineui on page0");
	zassert_false(r.body.config_dump.lorawan.has_devaddr, "devaddr must not be on page0");

	/* Same request with page=1 (field 5 varint = 0x28 0x01). */
	reset_cfg();
	handle("08021a0c0a08060701020304050a2801", &r);
	zassert_equal(r.which_body, Response_config_dump_tag, "page1 which=%d", r.which_body);
	zassert_equal(r.body.config_dump.page_index, 1, "page1 index");
	zassert_equal(r.body.config_dump.page_count, 2, "page1 count");
	zassert_false(r.body.config_dump.lorawan.has_deveui, "deveui must not be on page1");
	zassert_true(r.body.config_dump.lorawan.has_devaddr, "devaddr on page1");

	/* Out-of-range page → Error. */
	reset_cfg();
	handle("08021a0c0a08060701020304050a2805", &r);
	zassert_equal(r.which_body, Response_error_tag, "oob page should Error, which=%d",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_OUT_OF_RANGE, "code %d",
		      r.body.error.code);
}

/* #267: a duplicate field id in one get_param must not be counted twice against
 * the page budget. deveui (tag 6, 10 B) requested 4× would be 40 B > the ~30 B DR0
 * page budget and split into 2 pages before the dedup fix; deduped it is a single
 * 10 B field on one page. */
ZTEST(cmd, test_get_param_duplicate_field_deduped)
{
	Response r;

	reset_cfg();
	/* seq2 get_param{ lorawan_field=[6, 6, 6, 6] }, page 0. */
	handle("08021a060a0406060606", &r);
	zassert_equal(r.which_body, Response_config_dump_tag, "which=%d", r.which_body);
	zassert_equal(r.body.config_dump.page_count, 1, "duplicates inflated page_count to %u",
		      r.body.config_dump.page_count);
	zassert_true(r.body.config_dump.lorawan.has_deveui, "deveui not dumped");
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
	test_battery_v = 3.3f;
	test_battery_ret = 0;
	/* get_info reads the cached sample voltage, not a fresh ADC read. */
	g_app_sensor_data.voltage = 3.3f;

	int ret = app_cmd_build_info(out, sizeof(out), &out_len);
	zassert_equal(ret, 0, "build_info ret %d", ret);

	/* Skip the APP_PROTO_VERSION prefix (#55). */
	zassert_equal(out[0], APP_PROTO_VERSION, "bad version 0x%02x", out[0]);
	Response r = Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "decode");
	zassert_equal(r.which_body, Response_info_tag, "expected Info");
	zassert_equal(r.body.info.serial_number, 1234567890, "serial");
	/* Battery (mV) from the cached sample voltage; 3.3 V -> 3300 mV. */
	zassert_equal(r.body.info.battery, 3300, "battery %u", r.body.info.battery);
	/* #170: uncommissioned device (all-zero claim_token) omits the field. */
	zassert_false(r.body.info.has_claim_token, "claim_token must be omitted when unset");
}

/* sample over NFC: the device answers synchronously with the fresh telemetry
 * snapshot (Response.sample) so the phone can show the readings. */
ZTEST(cmd, test_sample_over_nfc)
{
	Response r;

	reset_cfg();
	/* seq5 sample{} (field 21, empty message). */
	handle_via(APP_CMD_TRANSPORT_NFC, "0805aa0100", &r);

	zassert_equal(r.which_body, Response_sample_tag, "expected Sample, which=%d", r.which_body);
	zassert_equal(r.seq, 5, "seq");
	zassert_true(r.body.sample.has_temperature, "snapshot temperature missing");
	zassert_equal(r.body.sample.temperature, 2345, "temperature %d", r.body.sample.temperature);
	zassert_true(r.body.sample.has_voltage, "snapshot voltage missing");
}

/* sample over LoRaWAN: the fPort-2 telemetry uplink is the answer, so no
 * fPort-85 body is emitted (out_len == 0, like force_send / req_history). */
ZTEST(cmd, test_sample_over_lrw_emits_no_body)
{
	uint8_t in[16], out[128];
	size_t in_len = unhex("0805aa0100", in, sizeof(in));
	size_t out_len = 123;
	enum app_cmd_action action = APP_CMD_ACTION_NONE;

	reset_cfg();
	int ret = app_cmd_handle(APP_CMD_TRANSPORT_LRW, in, in_len, out, sizeof(out), &out_len,
				 &action);

	zassert_equal(ret, 0, "app_cmd_handle ret %d", ret);
	zassert_equal(out_len, 0, "LoRaWAN sample must emit no fPort-85 body, out_len=%zu",
		      out_len);
	zassert_equal(action, APP_CMD_ACTION_NONE, "no deferred action");
}

/* #170: once commissioned, the Info carries the 16-byte claim_token. */
ZTEST(cmd, test_build_info_claim_token)
{
	uint8_t out[128];
	size_t out_len = 0;
	const uint8_t token[16] = {0x15, 0x8a, 0x6a, 0x5d, 0x5b, 0x54, 0xc5, 0x11,
				   0x8e, 0x62, 0xa8, 0xf4, 0xaf, 0x0d, 0xe8, 0xd2};

	reset_cfg();
	memcpy(g_app_config.claim_token, token, sizeof(token));

	zassert_equal(app_cmd_build_info(out, sizeof(out), &out_len), 0, "build_info");
	Response r = Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "decode");
	zassert_true(r.body.info.has_claim_token, "claim_token must be present once set");
	zassert_mem_equal(r.body.info.claim_token, token, sizeof(token), "claim_token bytes");
}

/* #320: the alarm-config report carries ONLY the changed (masked) non-empty slots — a
 * delta, not the whole config. A slot that is set but NOT in the changed mask is omitted;
 * a slot in the mask but empty (a cleared rule) is omitted too (clears aren't reported).
 * With a roomy frame the whole delta fits one frame (next_slot == slot count = done). */
ZTEST(cmd, test_build_alarm_config_report)
{
	uint8_t out[128];
	size_t out_len = 0;
	uint8_t next = 0;

	reset_cfg();
	g_app_config.alarm_limit = 30;
	g_app_config.alarm_notif_time = 15;
	/* Slots 0, 2, 5 configured (opaque bytes; only non-zero matters); slot 1 empty. */
	memset(g_app_config.alarm_0, 0xA5, sizeof(g_app_config.alarm_0));
	memset(g_app_config.alarm_2, 0x5A, sizeof(g_app_config.alarm_2));
	memset(g_app_config.alarm_5, 0x11, sizeof(g_app_config.alarm_5));

	/* Changed mask = slots 0 and 2 only. Slot 5 is set but NOT changed -> must be
	 * excluded; slot 1 is in-range but empty. Roomy frame (200 B) fits the whole delta. */
	uint16_t mask = (uint16_t)((1u << 0) | (1u << 2));
	int ret =
		app_cmd_build_alarm_config_report(out, sizeof(out), &out_len, mask, 0, 200, &next);
	zassert_equal(ret, 0, "build ret %d", ret);
	zassert_equal(next, 16, "whole delta should fit one frame, next=%u", next);

	zassert_equal(out[0], APP_PROTO_VERSION, "bad version 0x%02x", out[0]);
	Response r = Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "decode");

	zassert_equal(r.which_body, Response_config_dump_tag, "expected ConfigDump, which=%d",
		      r.which_body);
	/* Alarms group only — no other config group is echoed. */
	zassert_true(r.body.config_dump.has_alarms, "alarms section missing");
	zassert_false(r.body.config_dump.has_lorawan, "lorawan must not be in an alarm report");
	zassert_false(r.body.config_dump.has_application, "application must not be present");
	zassert_false(r.body.config_dump.has_sensors, "sensors must not be present");
	/* Scalars + the two CHANGED slots; the set-but-unchanged slot 5 and empty slot 1 out. */
	zassert_true(r.body.config_dump.alarms.has_alarm_limit, "alarm_limit missing");
	zassert_equal(r.body.config_dump.alarms.alarm_limit, 30, "alarm_limit value");
	zassert_true(r.body.config_dump.alarms.has_alarm_notif_time, "alarm_notif_time missing");
	zassert_true(r.body.config_dump.alarms.has_alarm_0, "changed alarm_0 missing");
	zassert_true(r.body.config_dump.alarms.has_alarm_2, "changed alarm_2 missing");
	zassert_false(r.body.config_dump.alarms.has_alarm_1, "empty alarm_1 must be omitted");
	zassert_false(r.body.config_dump.alarms.has_alarm_5,
		      "set-but-unchanged alarm_5 must NOT be reported (delta)");
	zassert_equal(r.body.config_dump.alarms.alarm_0[0], 0xA5, "alarm_0 bytes");
	zassert_equal(r.body.config_dump.alarms.alarm_2[0], 0x5A, "alarm_2 bytes");
}

/* #320: when more changed slots than fit one frame, the report packs each frame to the
 * given max_frame (measured) and reports next_slot so the emitter walks the rest. Three
 * changed rules at a DR0-sized frame (51 B) => 2 frames (2 rules + 1), not 1 rule/frame. */
ZTEST(cmd, test_build_alarm_config_report_packs_to_frame)
{
	uint8_t out[128];
	size_t out_len = 0;
	uint8_t next = 0;

	reset_cfg();
	g_app_config.alarm_limit = 5;
	g_app_config.alarm_notif_time = 10;
	memset(g_app_config.alarm_0, 0x11, sizeof(g_app_config.alarm_0));
	memset(g_app_config.alarm_1, 0x22, sizeof(g_app_config.alarm_1));
	memset(g_app_config.alarm_2, 0x33, sizeof(g_app_config.alarm_2));
	uint16_t mask = (uint16_t)((1u << 0) | (1u << 1) | (1u << 2));

	/* Frame 0: pack as many as fit 51 B. Two 17-byte rules + scalars (~49 B) fit; the
	 * third overflows, so next_slot points at it. */
	int ret = app_cmd_build_alarm_config_report(out, sizeof(out), &out_len, mask, 0, 51, &next);
	zassert_equal(ret, 0, "frame0 build");
	zassert_true(out_len <= 51, "frame0 %zu B must fit 51", out_len);
	zassert_equal(next, 2, "frame0 should hold slots 0-1, next=%u", next);
	Response r = Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "frame0 decode");
	zassert_true(r.body.config_dump.alarms.has_alarm_0, "alarm_0 in frame0");
	zassert_true(r.body.config_dump.alarms.has_alarm_1, "alarm_1 in frame0");
	zassert_false(r.body.config_dump.alarms.has_alarm_2, "alarm_2 overflows to frame1");

	/* Frame 1: the remaining slot; next reaches the slot count => delta complete. */
	ret = app_cmd_build_alarm_config_report(out, sizeof(out), &out_len, mask, next, 51, &next);
	zassert_equal(ret, 0, "frame1 build");
	zassert_equal(next, 16, "frame1 finishes the delta, next=%u", next);
	r = (Response)Response_init_zero;
	is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "frame1 decode");
	zassert_false(r.body.config_dump.alarms.has_alarm_0, "alarm_0 only in frame0");
	zassert_false(r.body.config_dump.alarms.has_alarm_1, "alarm_1 only in frame0");
	zassert_true(r.body.config_dump.alarms.has_alarm_2, "alarm_2 in frame1");
	zassert_equal(r.body.config_dump.alarms.alarm_2[0], 0x33, "alarm_2 bytes");
}

ZTEST(cmd, test_deferred_actions)
{
	Response r;

	reset_cfg();
	zassert_equal(handle("08043200", &r), APP_CMD_ACTION_SETTINGS_SAVE, "settings_save");
	zassert_equal(r.which_body, Response_ack_tag, "save acks");

	zassert_equal(handle("08083a00", &r), APP_CMD_ACTION_REBOOT, "reboot");

	/* device_reset (field 8, same wire id as the old, single factory_reset,
	 * renamed #299) — was tag=0x42 (field8, LEN), still is: only the C enum/oneof
	 * case name changed, not the wire byte. Reachable over LoRaWAN (unlike
	 * factory_reset below): it keeps the LoRaWAN session, so a downlink
	 * triggering it can still be acked normally. */
	zassert_equal(handle("08094200", &r), APP_CMD_ACTION_DEVICE_RESET, "device_reset");
}

/* factory_reset (field 23, NEW #299 tier — narrower than device_reset above, a
 * different command/id, not a renumbering of it) is nfc/shell only: a LoRaWAN
 * downlink triggering it would destroy the very session/keys carrying it, so
 * it could never even confirm delivery. */
ZTEST(cmd, test_factory_reset_nfc_shell_only)
{
	Response r;

	reset_cfg();
	zassert_equal(handle("080aba0100", &r), APP_CMD_ACTION_NONE,
		      "factory_reset rejected over lrw");
	zassert_equal(r.which_body, Response_error_tag, "lrw should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_READY, "code %d",
		      r.body.error.code);

	reset_cfg();
	enum app_cmd_action a = handle_via(APP_CMD_TRANSPORT_NFC, "080aba0100", &r);
	zassert_equal(a, APP_CMD_ACTION_FACTORY_RESET, "factory_reset over nfc");
	zassert_equal(r.which_body, Response_ack_tag, "factory_reset acks (which=%d)",
		      r.which_body);
}

/* #299 set_secret_key (field 24): nfc/shell only (rejected over lrw, like
 * force_send/req_history are rejected the other way in
 * test_lrw_only_commands_rejected_over_nfc); applies the new key to staging
 * immediately and defers the persist; missing key -> BAD_REQUEST, no action. */
ZTEST(cmd, test_set_secret_key)
{
	Response r;
	uint8_t expect_key[16];

	memset(expect_key, 0x11, sizeof(expect_key));

	reset_cfg();
	zassert_equal(handle("080bc201120a1011111111111111111111111111111111", &r),
		      APP_CMD_ACTION_NONE, "set_secret_key rejected over lrw");
	zassert_equal(r.which_body, Response_error_tag, "lrw should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_READY, "code %d",
		      r.body.error.code);

	reset_cfg();
	enum app_cmd_action a = handle_via(APP_CMD_TRANSPORT_NFC,
					   "080bc201120a1011111111111111111111111111111111", &r);
	zassert_equal(a, APP_CMD_ACTION_SECRET_KEY_SAVE, "set_secret_key over nfc");
	zassert_equal(r.which_body, Response_ack_tag, "set_secret_key acks (which=%d)",
		      r.which_body);
	zassert_mem_equal(g_app_config.secret_key, expect_key, sizeof(expect_key),
			  "secret_key not applied to staging");

	reset_cfg();
	a = handle_via(APP_CMD_TRANSPORT_NFC, "080cc20100", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "missing key: no action");
	zassert_equal(r.which_body, Response_error_tag, "missing key should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_BAD_REQUEST, "code %d",
		      r.body.error.code);
}

/* F-1: lrw_join (proto_id 17) / lrw_reset (16) must be rejected with NOT_READY
 * while unsaved LoRaWAN staging changes exist — otherwise the (re)join would
 * silently use the OLD credentials while GetParam already echoes the NEW ones.
 * With clean staging they fire their deferred actions as before. */
ZTEST(cmd, test_lrw_join_rejected_when_staging_dirty)
{
	Response r;

	reset_cfg();
	zassert_equal(handle("08018a0100", &r), APP_CMD_ACTION_LRW_JOIN, "clean join acts");
	zassert_equal(r.which_body, Response_ack_tag, "clean join acks");
	zassert_equal(handle("0801820100", &r), APP_CMD_ACTION_LRW_RESET, "clean reset acts");

	reset_cfg();
	test_set_lrw_dirty(true);
	zassert_equal(handle("08018a0100", &r), APP_CMD_ACTION_NONE, "dirty join no action");
	zassert_equal(r.which_body, Response_error_tag, "dirty join errors (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_READY, "code %d",
		      r.body.error.code);
	zassert_equal(handle("0801820100", &r), APP_CMD_ACTION_NONE, "dirty reset no action");
	zassert_equal(r.which_body, Response_error_tag, "dirty reset errors");
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
					      /*time_synced*/ true, samples, sizeof(samples), out,
					      sizeof(out), &out_len);
	zassert_equal(ret, 0, "full frame did not fit (ret %d)", ret);
	zassert_true(out_len <= sizeof(out), "out_len %zu overflows", out_len);

	Response r = Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "decode");
	zassert_equal(r.which_body, Response_history_frame_tag, "expected HistoryFrame");
	zassert_equal(r.body.history_frame.samples.size, 48, "sample count");
	zassert_true(r.body.history_frame.has_time_synced, "time_synced must be present");
	zassert_true(r.body.history_frame.time_synced, "time_synced should be true");
}

/* The capacity helper must be exact: a frame built with `cap` samples fits the
 * given out_cap, and one more byte does not. */
ZTEST(cmd, test_history_sample_capacity_is_exact)
{
	uint8_t samples[APP_CMD_HISTORY_FRAME_BUF_SIZE];
	uint8_t out[APP_CMD_HISTORY_FRAME_BUF_SIZE];
	size_t out_len = 0;

	memset(samples, 0x5A, sizeof(samples));

	/* Worst-case varints, mirroring history_frame_cap() in app_lrw.c. cap is
	 * bounded by out_cap minus the frame envelope and by the samples field size
	 * (440 B, #260) — for this out_cap the buffer, not the field, binds. */
	size_t cap = app_cmd_history_sample_capacity(200, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0x7,
						     900, sizeof(out));
	zassert_true(cap > 0 && cap < sizeof(out), "cap %zu out of range", cap);

	/* Exactly `cap` samples must encode within out_cap. */
	int ret = app_cmd_build_history_frame(200, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0x7, 900,
					      /*time_synced*/ true, samples, cap, out, sizeof(out),
					      &out_len);
	zassert_equal(ret, 0, "cap samples did not fit (ret %d)", ret);
	zassert_true(out_len <= sizeof(out), "out_len %zu > out_cap", out_len);

	/* Exactness: one more sample byte must NOT fit the same out_cap. */
	ret = app_cmd_build_history_frame(200, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0x7, 900,
					  /*time_synced*/ true, samples, cap + 1, out, sizeof(out),
					  &out_len);
	zassert_equal(ret, -EMSGSIZE, "cap+1 samples should overflow (ret %d)", ret);

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
