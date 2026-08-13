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
extern void test_set_active_alarm_count(size_t n);
extern int g_clm_ack_calls;
extern int g_clm_rearm_calls;
extern int g_buzzer_play_calls;
extern uint32_t g_buzzer_play_last_kind;
extern uint16_t g_buzzer_play_last_repeat_s;
extern int test_buzzer_play_ret;

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
	test_set_active_alarm_count(0);
	g_clm_ack_calls = 0;
	g_clm_rearm_calls = 0;
	g_buzzer_play_calls = 0;
	g_buzzer_play_last_kind = 0;
	g_buzzer_play_last_repeat_s = 0;
	test_buzzer_play_ret = 0;
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

/* #170: claim_token is NFC-only. app_cmd_build_info() is the
 * device-info-on-join uplink and is hardcoded to APP_CMD_TRANSPORT_LRW, so it
 * must NOT carry the token — at 18 B it pushed the Info past the EU868 DR0
 * payload budget, and an over-budget Info is dropped whole (Info has no paging
 * fields). Keeping it out is what makes the LoRaWAN Info fit at every DR. */
ZTEST(cmd, test_build_info_claim_token_omitted_over_lrw)
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
	zassert_false(r.body.info.has_claim_token, "claim_token must not be emitted over LoRaWAN");

	/* And the whole join Info must fit the tightest budget it can face: EU868
	 * DR0 carries 51 B of application payload. */
	zassert_true(out_len <= 51, "join Info is %zu B, over the DR0 budget", out_len);
}

/* #170: over NFC the token is still emitted once commissioned — that is now the
 * only channel a backend can learn it from. */
ZTEST(cmd, test_get_info_claim_token_present_over_nfc)
{
	uint8_t out[256];
	size_t out_len = 0;
	enum app_cmd_action action = APP_CMD_ACTION_NONE;
	const uint8_t token[16] = {0x15, 0x8a, 0x6a, 0x5d, 0x5b, 0x54, 0xc5, 0x11,
				   0x8e, 0x62, 0xa8, 0xf4, 0xaf, 0x0d, 0xe8, 0xd2};
	/* seq=1, get_info (field 4, empty body) */
	const uint8_t req[] = {0x08, 0x01, 0x22, 0x00};

	reset_cfg();
	memcpy(g_app_config.claim_token, token, sizeof(token));

	int ret = app_cmd_handle(APP_CMD_TRANSPORT_NFC, req, sizeof(req), out, sizeof(out),
				 &out_len, &action);
	zassert_equal(ret, 0, "app_cmd_handle ret %d", ret);
	Response r = Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "decode");
	zassert_true(r.body.info.has_claim_token, "claim_token must be present over NFC");
	zassert_mem_equal(r.body.info.claim_token, token, sizeof(token), "claim_token bytes");
}

/* Count Response.Info.active_alarms (field 15) entries by walking the raw
 * wire bytes, rather than via a pb_decode callback: nanopb's oneof decoder
 * memsets the whole target submessage struct -- wiping out any pre-set field
 * callback -- whenever which_body does not already match the incoming tag,
 * and Response_init_zero has already zeroed which_body by the time a caller
 * could set it, so there is no window where both survive (pb_decode.c,
 * PB_HTYPE_ONEOF case: "We memset to zero so that any callbacks are set to
 * NULL..."). A minimal wire walk sidesteps that nanopb quirk entirely. */
struct wire_field {
	uint32_t number;
	uint32_t wire_type;
	size_t content_off; /* wire_type 2 (length-delimited) only */
	size_t content_len;
	size_t next_off;
};

static bool wire_next_field(const uint8_t *buf, size_t len, size_t pos, struct wire_field *out)
{
	if (pos >= len) {
		return false;
	}

	uint32_t tag = 0;
	int shift = 0;
	while (pos < len) {
		uint8_t b = buf[pos++];
		tag |= (uint32_t)(b & 0x7f) << shift;
		shift += 7;
		if (!(b & 0x80)) {
			break;
		}
	}
	out->number = tag >> 3;
	out->wire_type = tag & 0x7;

	if (out->wire_type == 2) {
		uint32_t sublen = 0;
		shift = 0;
		while (pos < len) {
			uint8_t b = buf[pos++];
			sublen |= (uint32_t)(b & 0x7f) << shift;
			shift += 7;
			if (!(b & 0x80)) {
				break;
			}
		}
		out->content_off = pos;
		out->content_len = sublen;
		pos += sublen;
	} else if (out->wire_type == 0) {
		while (pos < len && (buf[pos] & 0x80)) {
			pos++;
		}
		pos++;
	} else if (out->wire_type == 5) {
		pos += 4;
	} else if (out->wire_type == 1) {
		pos += 8;
	}
	out->next_off = pos;
	return true;
}

static size_t count_wire_field(const uint8_t *buf, size_t len, uint32_t field_no)
{
	size_t pos = 0, count = 0;
	struct wire_field f;

	while (wire_next_field(buf, len, pos, &f)) {
		if (f.number == field_no) {
			count++;
		}
		pos = f.next_off;
	}
	return count;
}

static bool find_wire_submsg(const uint8_t *buf, size_t len, uint32_t field_no,
			     const uint8_t **content, size_t *content_len)
{
	size_t pos = 0;
	struct wire_field f;

	while (wire_next_field(buf, len, pos, &f)) {
		if (f.number == field_no && f.wire_type == 2) {
			*content = buf + f.content_off;
			*content_len = f.content_len;
			return true;
		}
		pos = f.next_off;
	}
	return false;
}

static size_t count_info_active_alarms(const uint8_t *out, size_t out_len)
{
	const uint8_t *info_content;
	size_t info_len;

	zassert_true(
		find_wire_submsg(out + 1, out_len - 1, Response_info_tag, &info_content, &info_len),
		"Response.info field not found");
	return count_wire_field(info_content, info_len, 15);
}

/* #335 tier-2: active_alarms is repeated/unbounded (~8 B/entry) and can push
 * Info back over a low DR's budget on its own, even with claim_token now
 * NFC-only. app_cmd_build_info() must drop alarm entries one at a time to fit
 * out_cap instead of failing (and losing firmware/serial/battery too). */
ZTEST(cmd, test_build_info_trims_alarms_over_dr_budget)
{
	uint8_t out[256];
	size_t full_len = 0, out_len = 0;

	reset_cfg();
	g_app_config.serial_number = 1234567890;
	test_battery_v = 3.3f;
	test_battery_ret = 0;
	g_app_sensor_data.voltage = 3.3f;
	test_set_active_alarm_count(5);

	/* Baseline: plenty of room, all 5 alarms present. */
	int ret = app_cmd_build_info(out, sizeof(out), &full_len);
	zassert_equal(ret, 0, "baseline build_info ret %d", ret);
	zassert_equal(count_info_active_alarms(out, full_len), 5,
		      "expected all 5 alarms unconstrained");

	/* One byte short of the untrimmed size: must still succeed, with fewer
	 * alarms (dropping even one entry frees far more than 1 B of headroom). */
	ret = app_cmd_build_info(out, full_len - 1, &out_len);
	zassert_equal(ret, 0, "trimmed build_info ret %d", ret);
	zassert_true(out_len <= full_len - 1, "out_len %zu over cap %zu", out_len, full_len - 1);
	size_t trimmed_count = count_info_active_alarms(out, out_len);
	zassert_true(trimmed_count < 5, "expected alarms to be trimmed, got %zu", trimmed_count);

	/* The rest of Info survives untouched -- alarms are what gets cut. */
	Response r = Response_init_zero;
	pb_istream_t is = pb_istream_from_buffer(out + 1, out_len - 1);
	zassert_true(pb_decode(&is, Response_fields, &r), "decode trimmed Info");
	zassert_equal(r.body.info.serial_number, 1234567890, "serial must survive trimming");
	zassert_equal(r.body.info.battery, 3300, "battery must survive trimming");

	/* A cap too small even for zero alarms genuinely fails -- no silent
	 * truncation of the rest of Info. */
	ret = app_cmd_build_info(out, 2, &out_len);
	zassert_equal(ret, -EMSGSIZE, "expected -EMSGSIZE for an impossible cap, got %d", ret);
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
 * immediately and defers the persist+reboot (#322 — the reboot is what makes the
 * new key live, since the NFC channel authenticates from g_app_config); missing
 * key -> BAD_REQUEST, no action; all-zero key -> BAD_REQUEST, no action (#322). */
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

	/* #322: an all-zero key is the "unprovisioned" sentinel that makes
	 * key_is_provisioned() refuse the encrypted NFC channel outright — accepting
	 * it would lock the caller out of the channel it just used (and, now that the
	 * deferred action reboots, immediately). Rejected, key left untouched. */
	uint8_t keep_key[16];
	memset(keep_key, 0x22, sizeof(keep_key));

	reset_cfg();
	memcpy(g_app_config.secret_key, keep_key, sizeof(keep_key));
	a = handle_via(APP_CMD_TRANSPORT_NFC, "080dc201120a1000000000000000000000000000000000", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "zero key: no action");
	zassert_equal(r.which_body, Response_error_tag, "zero key should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_BAD_REQUEST, "code %d",
		      r.body.error.code);
	/* seq echoed back proves the frame decoded and the handler rejected it, rather
	 * than a malformed frame failing to decode into the same BAD_REQUEST. */
	zassert_equal(r.seq, 13, "seq %u", r.seq);
	zassert_mem_equal(g_app_config.secret_key, keep_key, sizeof(keep_key),
			  "zero key must not overwrite the current secret_key");
}

/* #308 clm_ack (field 25): nfc/shell only (rejected over lrw, like
 * factory_reset/set_secret_key above). The actual clm-latch transition lives in
 * app_nfc.c (HIL-verified, #247/#308 — see manual-test-plan.md); here we only
 * confirm the command reaches app_nfc_clm_ack() and acks, via the stub call
 * counter (g_clm_ack_calls). */
ZTEST(cmd, test_clm_ack)
{
	Response r;

	reset_cfg();
	zassert_equal(handle("080cca0100", &r), APP_CMD_ACTION_NONE, "clm_ack rejected over lrw");
	zassert_equal(r.which_body, Response_error_tag, "lrw should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_READY, "code %d",
		      r.body.error.code);
	zassert_equal(g_clm_ack_calls, 0, "must not call app_nfc_clm_ack over lrw");

	reset_cfg();
	enum app_cmd_action a = handle_via(APP_CMD_TRANSPORT_NFC, "080cca0100", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "clm_ack over nfc: no deferred action");
	zassert_equal(r.which_body, Response_ack_tag, "clm_ack acks (which=%d)", r.which_body);
	zassert_equal(g_clm_ack_calls, 1, "app_nfc_clm_ack called exactly once");
}

/* #351 clm_rearm (field 27): nfc/shell only (rejected over lrw, same pattern as
 * clm_ack/set_secret_key above). No/zero new_claim_token re-arms immediately via
 * app_nfc_clm_reset() (no deferred action, staging untouched); a non-zero
 * new_claim_token stages it into g_app_config and defers
 * APP_CMD_ACTION_CLM_REARM_SAVE INSTEAD of calling app_nfc_clm_reset()
 * synchronously (see app_cmd_handle_clm_rearm for why: avoids exposing the
 * still-live OLD token via an NFC poll before the deferred reboot lands it). */
ZTEST(cmd, test_clm_rearm)
{
	Response r;
	uint8_t expect_token[16];

	memset(expect_token, 0x33, sizeof(expect_token));

	reset_cfg();
	zassert_equal(handle("080dda0100", &r), APP_CMD_ACTION_NONE, "clm_rearm rejected over lrw");
	zassert_equal(r.which_body, Response_error_tag, "lrw should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_READY, "code %d",
		      r.body.error.code);
	zassert_equal(g_clm_rearm_calls, 0, "must not call app_nfc_clm_reset over lrw");

	reset_cfg();
	enum app_cmd_action a = handle_via(APP_CMD_TRANSPORT_NFC, "080dda0100", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "clm_rearm without token: no deferred action");
	zassert_equal(r.which_body, Response_ack_tag, "clm_rearm acks (which=%d)", r.which_body);
	zassert_equal(g_clm_rearm_calls, 1, "app_nfc_clm_reset called exactly once");

	reset_cfg();
	a = handle_via(APP_CMD_TRANSPORT_NFC, "080eda01120a1033333333333333333333333333333333", &r);
	zassert_equal(a, APP_CMD_ACTION_CLM_REARM_SAVE, "clm_rearm with token defers save+reboot");
	zassert_equal(r.which_body, Response_ack_tag, "clm_rearm acks (which=%d)", r.which_body);
	zassert_mem_equal(g_app_config.claim_token, expect_token, sizeof(expect_token),
			  "new_claim_token not staged");
	zassert_equal(g_clm_rearm_calls, 0,
		      "app_nfc_clm_reset must NOT run synchronously when staging a new token");
}

/* #338 buzzer_play (field 28): lrw/nfc only (rejected over shell, like
 * clock_sync/enter_calibration are lrw/nfc-only too). app_cmd never validates
 * `kind` itself — it forwards the raw id to app_buzzer_play_repeating()
 * unchanged (stubbed here) and only turns -ENOENT into BAD_REQUEST; repeat_s
 * is the one field app_cmd DOES range-check (0-999) before forwarding. */
ZTEST(cmd, test_buzzer_play)
{
	Response r;

	/* seq1 buzzer_play{kind=2 (warning), repeat_s=15} over lrw -> forwarded
	 * as-is, acks. */
	reset_cfg();
	enum app_cmd_action a = handle("0801e201040802100f", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action");
	zassert_equal(r.which_body, Response_ack_tag, "buzzer_play acks (which=%d)", r.which_body);
	zassert_equal(g_buzzer_play_calls, 1, "app_buzzer_play_repeating called exactly once");
	zassert_equal(g_buzzer_play_last_kind, 2, "kind not forwarded (%d)",
		      g_buzzer_play_last_kind);
	zassert_equal(g_buzzer_play_last_repeat_s, 15, "repeat_s not forwarded (%d)",
		      g_buzzer_play_last_repeat_s);

	/* kind=0 (the stop request) is forwarded like any other id — the
	 * dispatch layer does not special-case it; app_buzzer.c decides it
	 * means "silence + cancel repeat". */
	reset_cfg();
	a = handle("0801e201020800", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action");
	zassert_equal(r.which_body, Response_ack_tag, "stop acks (which=%d)", r.which_body);
	zassert_equal(g_buzzer_play_calls, 1, "app_buzzer_play_repeating called for stop");
	zassert_equal(g_buzzer_play_last_kind, 0, "kind 0 not forwarded (%d)",
		      g_buzzer_play_last_kind);

	/* repeat_s=1000 (over the 0-999 range) -> BAD_REQUEST, never reaches
	 * app_buzzer.c. */
	reset_cfg();
	a = handle("0801e20105080110e807", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action");
	zassert_equal(r.which_body, Response_error_tag, "out-of-range repeat_s -> error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_BAD_REQUEST, "code %d",
		      r.body.error.code);
	zassert_equal(g_buzzer_play_calls, 0, "must not call app_buzzer_play_repeating");

	/* kind=5 (reserved/unassigned) -> app_buzzer.c (stubbed) rejects with
	 * -ENOENT, app_cmd turns that into BAD_REQUEST. */
	reset_cfg();
	test_buzzer_play_ret = -ENOENT;
	a = handle("0801e201020805", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action");
	zassert_equal(r.which_body, Response_error_tag, "unknown kind -> error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_BAD_REQUEST, "code %d",
		      r.body.error.code);
	zassert_equal(g_buzzer_play_calls, 1, "app_buzzer_play_repeating still called once");

	/* Rejected over shell (not in [lrw, nfc]). */
	reset_cfg();
	a = handle_via(APP_CMD_TRANSPORT_SHELL_DEBUG, "0801e201040802100f", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "buzzer_play rejected over shell");
	zassert_equal(r.which_body, Response_error_tag, "shell should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_READY, "code %d",
		      r.body.error.code);
	zassert_equal(g_buzzer_play_calls, 0, "must not call app_buzzer_play_repeating over shell");
}

/* #316: vendor_reset is a generic Command reachable ONLY over the vendor
 * transport (NFC hio.stck:vnd). Its body reuses SetSecretKey (field 26 — 25 was
 * taken by clm_ack, #308) — the replacement secret_key, mandatory because
 * vendor_reset zeroes the old one. The handler stages the key + defers
 * APP_CMD_ACTION_VENDOR_RESET; a missing key is BAD_REQUEST (checked before the
 * allow gate). */
ZTEST(cmd, test_vendor_reset_command)
{
	Response r;
	uint8_t expect_key[16];

	memset(expect_key, 0x22, sizeof(expect_key));

	reset_cfg();
	g_app_config.vendor_reset_allow = true;
	/* seq1 vendor_reset{ key = 16x0x22 } */
	enum app_cmd_action a = handle_via(APP_CMD_TRANSPORT_VENDOR,
					   "0801d201120a1022222222222222222222222222222222", &r);
	zassert_equal(a, APP_CMD_ACTION_VENDOR_RESET, "vendor_reset over vendor acts");
	zassert_equal(r.which_body, Response_ack_tag, "vendor_reset acks (which=%d)", r.which_body);
	zassert_mem_equal(app_cmd_take_pending_vendor_secret_key(), expect_key, sizeof(expect_key),
			  "replacement key not staged");

	reset_cfg();
	g_app_config.vendor_reset_allow = true;
	/* seq2 vendor_reset{} — no key */
	a = handle_via(APP_CMD_TRANSPORT_VENDOR, "0802d20100", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "missing key: no action");
	zassert_equal(r.which_body, Response_error_tag, "missing key should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_BAD_REQUEST, "code %d",
		      r.body.error.code);
}

/* #316 Option A: the vendor_reset Command still honors vendor_reset_allow — when
 * false it is refused with NOT_READY (immediate feedback, not a silent no-op). The
 * vendor re-enables it via a vendor-channel set_param (test_vendor_reset_allow_
 * write_gate) and then vendor_reset succeeds. */
ZTEST(cmd, test_vendor_reset_gated_by_allow)
{
	Response r;

	reset_cfg(); /* vendor_reset_allow = false */
	enum app_cmd_action a = handle_via(APP_CMD_TRANSPORT_VENDOR,
					   "0801d201120a1022222222222222222222222222222222", &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "disabled: no action");
	zassert_equal(r.which_body, Response_error_tag, "disabled should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_READY, "code %d",
		      r.body.error.code);
}

/* #316: vendor_reset (transports:[vendor]) is rejected on every other transport by
 * the generated dispatch guard, before the handler runs. */
ZTEST(cmd, test_vendor_reset_rejected_off_vendor)
{
	Response r;
	const char *hex = "0801d201120a1022222222222222222222222222222222";

	reset_cfg();
	g_app_config.vendor_reset_allow = true;
	zassert_equal(handle_via(APP_CMD_TRANSPORT_LRW, hex, &r), APP_CMD_ACTION_NONE,
		      "vendor_reset rejected over lrw");
	zassert_equal(r.which_body, Response_error_tag, "lrw should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_READY, "code %d",
		      r.body.error.code);

	reset_cfg();
	g_app_config.vendor_reset_allow = true;
	zassert_equal(handle_via(APP_CMD_TRANSPORT_NFC, hex, &r), APP_CMD_ACTION_NONE,
		      "vendor_reset rejected over nfc");
	zassert_equal(r.which_body, Response_error_tag, "nfc should error (which=%d)",
		      r.which_body);
}

/* #316: set_secret_key gains the vendor transport ([nfc, shell, vendor]) so the
 * vendor can rotate the key over hio.stck:vnd (authenticated by vendor_token). */
ZTEST(cmd, test_set_secret_key_over_vendor)
{
	Response r;
	uint8_t expect_key[16];

	memset(expect_key, 0x11, sizeof(expect_key));

	reset_cfg();
	enum app_cmd_action a = handle_via(APP_CMD_TRANSPORT_VENDOR,
					   "080bc201120a1011111111111111111111111111111111", &r);
	zassert_equal(a, APP_CMD_ACTION_SECRET_KEY_SAVE, "set_secret_key over vendor");
	zassert_equal(r.which_body, Response_ack_tag, "set_secret_key acks (which=%d)",
		      r.which_body);
	zassert_mem_equal(g_app_config.secret_key, expect_key, sizeof(expect_key),
			  "secret_key not applied to staging");
}

/* #316: vendor_reset_allow is writable ONLY over the vendor transport, and the
 * write is never gated on its current value (recovery from false must always
 * succeed). Over lrw/nfc it is NOT_WRITABLE with fault_field 207 (application
 * group *100 + tag 7) and the batch rolls back. */
ZTEST(cmd, test_vendor_reset_allow_write_gate)
{
	Response r;
	/* seq1 set_param{ application{ vendor_reset_allow = true } } */
	const char *hex = "0801120412023801";

	/* vendor: false -> true always succeeds (no current-value check). */
	reset_cfg();
	g_app_config.vendor_reset_allow = false;
	enum app_cmd_action a = handle_via(APP_CMD_TRANSPORT_VENDOR, hex, &r);
	zassert_equal(a, APP_CMD_ACTION_NONE, "no deferred action");
	zassert_equal(r.which_body, Response_ack_tag, "vendor write acks (which=%d)", r.which_body);
	zassert_true(g_app_config.vendor_reset_allow, "vendor set_param did not flip false->true");

	/* lrw: rejected as NOT_WRITABLE, nothing applied. */
	reset_cfg();
	g_app_config.vendor_reset_allow = false;
	handle_via(APP_CMD_TRANSPORT_LRW, hex, &r);
	zassert_equal(r.which_body, Response_error_tag, "lrw should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_WRITABLE, "code %d",
		      r.body.error.code);
	zassert_equal(r.body.error.fault_field, 207, "fault_field %u (want 207)",
		      r.body.error.fault_field);
	zassert_false(g_app_config.vendor_reset_allow, "lrw write applied despite gate");

	/* nfc: also rejected — writable is vendor-only. */
	reset_cfg();
	g_app_config.vendor_reset_allow = false;
	handle_via(APP_CMD_TRANSPORT_NFC, hex, &r);
	zassert_equal(r.which_body, Response_error_tag, "nfc should error (which=%d)",
		      r.which_body);
	zassert_equal(r.body.error.code, Response_Error_Code_NOT_WRITABLE, "code %d",
		      r.body.error.code);
	zassert_false(g_app_config.vendor_reset_allow, "nfc write applied despite gate");
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
