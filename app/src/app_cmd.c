/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_cmd.h"
#include "app_config.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_config_ingest.h"

/* Wall-clock source (PR #41, branch lrw-rtc-time). Until that lands on this
 * branch, app_clock.h is absent and Info.unix_time stays 0 (omitted by proto3).
 * The __has_include guard flips on automatically once the module is merged. */
#if defined(__has_include) && __has_include("app_clock.h")
#include "app_clock.h"
#define APP_CMD_HAVE_CLOCK 1
#endif

/* Sensor history store-and-forward (#24). When the module is present, ReqHistory
 * replays stored records as paged HistoryFrame responses. */
#if defined(__has_include) && __has_include("app_history.h")
#include "app_history.h"
#define APP_CMD_HAVE_HISTORY 1
#endif

/* Nanopb includes */
#include <pb_decode.h>
#include <pb_encode.h>
#include "src/app_config.pb.h"

/* Zephyr includes */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

/* Standard includes */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

LOG_MODULE_REGISTER(app_cmd, LOG_LEVEL_DBG);

/* Firmware version + build type. Defined by the build (CI passes
 * -DAPP_VERSION_* / -DAPP_BUILD_TYPE, see app/CMakeLists.txt). Fallbacks keep
 * IDE/standalone tooling happy and mark local builds as CUSTOM. */
#ifndef APP_VERSION_MAJOR
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 0
#define APP_BUILD_TYPE    2
#endif

void app_cmd_get_info(struct app_cmd_info *info)
{
	if (!info) {
		return;
	}

	*info = (struct app_cmd_info){
		.fw_major = APP_VERSION_MAJOR,
		.fw_minor = APP_VERSION_MINOR,
		.fw_patch = APP_VERSION_PATCH,
		.build_type = APP_BUILD_TYPE,
		.debug = IS_ENABLED(CONFIG_FW_DEBUG),
		.serial_number = g_app_config.serial_number,
		.uptime_s = (uint32_t)(k_uptime_get() / 1000),
	};

#ifdef APP_CMD_HAVE_CLOCK
	uint32_t unix_s;
	if (app_clock_get_unix(&unix_s) == 0) {
		info->has_unix_time = true;
		info->unix_time = unix_s;
	}
#endif
}

/* Map the plain-C info snapshot onto the protobuf Response_Info. */
static void fill_info(Response_Info *info)
{
	struct app_cmd_info i;
	app_cmd_get_info(&i);

	info->fw_major = i.fw_major;
	info->fw_minor = i.fw_minor;
	info->fw_patch = i.fw_patch;
	info->build_type = (Response_Info_BuildType)i.build_type;
	info->serial_number = i.serial_number;
	info->uptime_s = i.uptime_s;
	info->debug = i.debug;
	if (i.has_unix_time) {
		info->unix_time = i.unix_time;
	}
}

static void make_error(Response *resp, Response_Error_Code code, const char *detail)
{
	resp->which_body = Response_error_tag;
	resp->body.error.code = code;
	resp->body.error.fault_field = 0;

	if (detail) {
		strncpy(resp->body.error.detail, detail, sizeof(resp->body.error.detail) - 1);
		resp->body.error.detail[sizeof(resp->body.error.detail) - 1] = '\0';
	} else {
		resp->body.error.detail[0] = '\0';
	}
}

static void handle_set_param(const Command_SetParam *sp, Response *resp,
			     enum app_cmd_action *action)
{
	uint32_t fault = 0;
	int rc = 0;

	/* Apply atomically: snapshot the staging config, apply both sections, then
	 * cross-validate. On any fault, restore the snapshot so a rejected batch
	 * leaves nothing partially staged for a later SettingsSave to persist. */
	struct app_config snapshot = *app_config();

	if (sp->has_lorawan) {
		rc = app_config_apply_lorawan(&sp->lorawan, &fault);
	}
	if (rc == 0 && sp->has_application) {
		rc = app_config_apply_application(&sp->application, &fault);
	}
	if (rc == 0) {
		rc = app_config_validate_alarm_pairs(app_config(), &fault);
	}

	if (rc) {
		*app_config() = snapshot; /* roll back the whole batch */
		make_error(resp, Response_Error_Code_OUT_OF_RANGE, "invalid value");
		resp->body.error.fault_field = fault;
	} else {
		resp->which_body = Response_ack_tag;
		/* Optional one-shot commit: persist staged config + reboot, same path
		 * as SettingsSave. Used as the last message of a multi-downlink batch. */
		if (sp->has_save && sp->save) {
			*action = APP_CMD_ACTION_SETTINGS_SAVE;
		}
	}
}

static void handle_get_param(const Command_GetParam *gp, Response *resp)
{
	resp->which_body = Response_config_dump_tag;
	resp->body.config_dump.page_index = 0;
	resp->body.config_dump.page_count = 1;

	if (gp->lorawan_field_count > 0) {
		resp->body.config_dump.has_lorawan = true;
		app_config_fill_lorawan(&resp->body.config_dump.lorawan, gp->lorawan_field,
					gp->lorawan_field_count);
	}
	if (gp->application_field_count > 0) {
		resp->body.config_dump.has_application = true;
		app_config_fill_application(&resp->body.config_dump.application,
					    gp->application_field, gp->application_field_count);
	}
}

/* Dumpable config fields in fixed order, each with a conservative upper bound
 * on its encoded size (field tag + value; hex fields include the length byte).
 * Mirrors the non-secret fields emitted by app_config_fill_lorawan/application
 * — a new config field needs a row here too (a codegen target once #44 lands).
 * Drives get_config paging: greedy bin-pack into DR0-sized ConfigDump pages. */
#define DUMP_SECTION_LORAWAN     0
#define DUMP_SECTION_APPLICATION 1

#define LRW(tag, size) {DUMP_SECTION_LORAWAN, (tag), (size)}
#define APP2(tag)      {DUMP_SECTION_APPLICATION, (tag), 2}
#define APP5(tag)      {DUMP_SECTION_APPLICATION, (tag), 5}

static const struct {
	uint8_t section;
	uint8_t tag;
	uint8_t size;
} DUMP_FIELDS[] = {
	/* Lorawan (non-secret): varints 2 B, deveui/joineui 18 B, devaddr 10 B */
	LRW(1, 2),
	LRW(2, 2),
	LRW(3, 2),
	LRW(4, 2),
	LRW(5, 18),
	LRW(6, 18),
	LRW(9, 10),
	LRW(12, 2),
	/* Application (tag 3 interval_aggreg has no config field — skipped, like
	 * fill_application). Floats 5 B, everything else 2 B. */
	APP2(1),
	APP2(2),
	APP2(4),
	APP2(5),
	APP5(6),
	APP5(7),
	APP5(8),
	APP2(9),
	APP5(10),
	APP5(11),
	APP5(12),
	APP2(13),
	APP5(14),
	APP5(15),
	APP5(16),
	APP2(17),
	APP5(18),
	APP5(19),
	APP5(20),
	APP2(21),
	APP5(22),
	APP5(23),
	APP5(24),
	APP2(25),
	APP2(26),
	APP2(27),
	APP2(28),
	APP2(29),
	APP2(30),
	APP2(31),
	APP2(32),
	APP2(33),
	APP2(34),
	APP2(35),
	APP2(36),
	APP5(37),
	APP5(38),
	APP5(39),
	APP2(40),
	APP2(41),
	APP2(42),
	APP2(43),
	APP2(44),
	APP2(45),
	APP2(46),
	APP2(47),
	APP2(48),
};

/* Per-page byte budget for the field payload inside one ConfigDump. The encoded
 * Response adds ~14 B of fixed overhead around these fields (seq +
 * config_dump wrapper + page_index + page_count + the two submessage wrappers),
 * so the on-air frame is roughly budget + 14. DR0 MTU is 51 B; 30 keeps the
 * worst-case frame near 44 B with margin. Conservative — a page can never
 * overflow (the largest single field is 18 B). */
#define DUMP_PAGE_BUDGET 30

static void handle_get_config(const Command_GetConfig *gc, Response *resp)
{
	uint32_t page = gc->has_page ? gc->page : 0;

	/* Both sized to the whole table: a LoRaWAN row added to DUMP_FIELDS must not
	 * overflow lrw_ids (was a fixed [8] matching exactly today's LoRaWAN rows). */
	uint32_t lrw_ids[ARRAY_SIZE(DUMP_FIELDS)];
	uint32_t app_ids[ARRAY_SIZE(DUMP_FIELDS)];
	size_t n_lrw = 0, n_app = 0;

	/* Single greedy pass: pack fields into pages by DUMP_PAGE_BUDGET, collect
	 * the requested page's tags, and learn the total page count. */
	uint32_t cur_page = 0, used = 0;
	for (size_t i = 0; i < ARRAY_SIZE(DUMP_FIELDS); i++) {
		if (used > 0 && used + DUMP_FIELDS[i].size > DUMP_PAGE_BUDGET) {
			cur_page++;
			used = 0;
		}
		used += DUMP_FIELDS[i].size;

		if (cur_page == page) {
			if (DUMP_FIELDS[i].section == DUMP_SECTION_LORAWAN) {
				lrw_ids[n_lrw++] = DUMP_FIELDS[i].tag;
			} else {
				app_ids[n_app++] = DUMP_FIELDS[i].tag;
			}
		}
	}
	uint32_t page_count = cur_page + 1;

	if (page >= page_count) {
		make_error(resp, Response_Error_Code_OUT_OF_RANGE, "page");
		resp->body.error.fault_field = 1;
		return;
	}

	resp->which_body = Response_config_dump_tag;
	resp->body.config_dump.page_index = page;
	resp->body.config_dump.page_count = page_count;

	if (n_lrw > 0) {
		resp->body.config_dump.has_lorawan = true;
		app_config_fill_lorawan(&resp->body.config_dump.lorawan, lrw_ids, n_lrw);
	}
	if (n_app > 0) {
		resp->body.config_dump.has_application = true;
		app_config_fill_application(&resp->body.config_dump.application, app_ids, n_app);
	}
}

static void handle_reset_counters(const Command_ResetCounters *rc, Response *resp)
{
	app_hall_reset_count(rc->has_hall_left && rc->hall_left,
			     rc->has_hall_right && rc->hall_right);
	app_input_reset_count(rc->has_input_a && rc->input_a, rc->has_input_b && rc->input_b);
	resp->which_body = Response_ack_tag;
}

static void handle_req_history(enum app_cmd_transport transport, const Command *cmd, Response *resp)
{
#if defined(APP_CMD_HAVE_HISTORY) && defined(CONFIG_LORAWAN)
	if (transport != APP_CMD_TRANSPORT_LRW) {
		/* Replay is inherently a stream of LoRaWAN uplinks. */
		make_error(resp, Response_Error_Code_NOT_READY, "lrw only");
		return;
	}

	const Command_ReqHistory *rq = &cmd->body.req_history;
	uint32_t from = rq->has_from_unix ? rq->from_unix : 0;
	uint32_t to = rq->has_to_unix ? rq->to_unix : UINT32_MAX;

	/* Device-driven replay: the device streams all matching records back as N
	 * HistoryFrame uplinks on port 85. The first frame is the reply, so leave
	 * the response body unset (which_body stays 0) to suppress a redundant Ack
	 * uplink. Only when nothing replays (empty window / DR too low) do we send
	 * an Error so the host still gets a definitive answer. */
	if (!app_lrw_start_history_replay(from, to, cmd->seq)) {
		make_error(resp, Response_Error_Code_HISTORY_UNAVAILABLE, "no records");
	}
#else
	ARG_UNUSED(transport);
	ARG_UNUSED(cmd);
	make_error(resp, Response_Error_Code_HISTORY_UNAVAILABLE, "no history");
#endif
}

static void dispatch(enum app_cmd_transport transport, const Command *cmd, Response *resp,
		     enum app_cmd_action *action)
{
	ARG_UNUSED(transport);

	resp->seq = cmd->seq;

	switch (cmd->which_body) {
	case Command_get_info_tag:
		resp->which_body = Response_info_tag;
		fill_info(&resp->body.info);
		break;

	case Command_set_param_tag:
		handle_set_param(&cmd->body.set_param, resp, action);
		break;

	case Command_get_param_tag:
		handle_get_param(&cmd->body.get_param, resp);
		break;

	case Command_get_config_tag:
		handle_get_config(&cmd->body.get_config, resp);
		break;

	case Command_settings_save_tag:
		resp->which_body = Response_ack_tag;
		*action = APP_CMD_ACTION_SETTINGS_SAVE;
		break;

	case Command_reboot_tag:
		resp->which_body = Response_ack_tag;
		*action = APP_CMD_ACTION_REBOOT;
		break;

	case Command_factory_reset_tag:
		resp->which_body = Response_ack_tag;
		*action = APP_CMD_ACTION_FACTORY_RESET;
		break;

	case Command_force_send_tag:
#if defined(CONFIG_LORAWAN)
		app_lrw_send();
#endif
		resp->which_body = Response_ack_tag;
		break;

	case Command_reset_counters_tag:
		handle_reset_counters(&cmd->body.reset_counters, resp);
		break;

	case Command_clock_sync_tag:
#ifdef APP_CMD_HAVE_CLOCK
		app_clock_force_resync();
#endif
		resp->which_body = Response_ack_tag;
		break;

	case Command_req_history_tag:
		handle_req_history(transport, cmd, resp);
		break;

	default:
		LOG_WRN("Command tag %u not implemented", cmd->which_body);
		make_error(resp, Response_Error_Code_UNKNOWN, "not implemented");
		break;
	}
}

/* Encode a Response into `out` with the 1-byte APP_PROTO_VERSION prefix at
 * out[0] (fPort 85). Sets *out_len to the total length (version + protobuf). */
static int encode_response(const Response *resp, uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (out_cap < 1) {
		return -EMSGSIZE;
	}

	out[0] = APP_PROTO_VERSION;

	pb_ostream_t ostream = pb_ostream_from_buffer(out + 1, out_cap - 1);
	if (!pb_encode(&ostream, Response_fields, resp)) {
		LOG_ERR_CALL_FAILED_STR("pb_encode", PB_GET_ERROR(&ostream));
		return -EMSGSIZE;
	}

	*out_len = ostream.bytes_written + 1;
	return 0;
}

int app_cmd_handle(enum app_cmd_transport transport, const uint8_t *in, size_t in_len, uint8_t *out,
		   size_t out_cap, size_t *out_len, enum app_cmd_action *action)
{
	if (!in || !out || !out_len) {
		return -EINVAL;
	}

	*out_len = 0;
	enum app_cmd_action act = APP_CMD_ACTION_NONE;

	Command cmd = Command_init_zero;
	Response resp = Response_init_zero;

	pb_istream_t istream = pb_istream_from_buffer(in, in_len);
	if (!pb_decode(&istream, Command_fields, &cmd)) {
		LOG_ERR_CALL_FAILED_STR("pb_decode", PB_GET_ERROR(&istream));
		resp.seq = 0;
		make_error(&resp, Response_Error_Code_BAD_REQUEST, PB_GET_ERROR(&istream));
	} else {
		dispatch(transport, &cmd, &resp, &act);
	}

	/* A handler may opt out of an immediate response by leaving the oneof unset
	 * (which_body == 0) — e.g. ReqHistory, whose HistoryFrame stream is the
	 * reply. Emit nothing so no redundant uplink is queued. */
	if (resp.which_body == 0) {
		*out_len = 0;
		if (action) {
			*action = act;
		}
		return 0;
	}

	int ret = encode_response(&resp, out, out_cap, out_len);
	if (ret) {
		return ret;
	}

	if (action) {
		*action = act;
	}
	return 0;
}

int app_cmd_build_info(uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!out || !out_len) {
		return -EINVAL;
	}

	Response resp = Response_init_zero;
	resp.seq = 0;
	resp.which_body = Response_info_tag;
	fill_info(&resp.body.info);

	return encode_response(&resp, out, out_cap, out_len);
}

#if defined(APP_CMD_HAVE_HISTORY)
size_t app_cmd_history_sample_capacity(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				       uint32_t t0_unix, uint32_t present, uint32_t interval_s,
				       size_t out_cap)
{
	Response resp = Response_init_zero;

	resp.seq = seq;
	resp.which_body = Response_history_frame_tag;
	Response_HistoryFrame *hf = &resp.body.history_frame;
	hf->frame_index = frame_index;
	hf->frame_count = frame_count;
	hf->t0_unix = t0_unix;
	hf->present = present;
	hf->interval_s = interval_s;
	hf->samples.size = 0; /* empty bytes field is omitted in proto3 */

	size_t base = 0;
	if (!pb_get_encoded_size(&base, Response_fields, &resp)) {
		return 0;
	}

	/* The encoded frame is: version(1) + base + samples field. With 1..N
	 * sample bytes (N <= 48 < 128) the samples field adds tag(1) + len(1) + N.
	 * The empty-field omission above means `base` excludes those 2 bytes. */
	const size_t fixed = 1 + base + 2;
	if (out_cap <= fixed) {
		return 0;
	}

	size_t avail = out_cap - fixed;
	return MIN(avail, sizeof(hf->samples.bytes));
}

int app_cmd_build_history_frame(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				uint32_t t0_unix, uint32_t present, uint32_t interval_s,
				const uint8_t *samples, size_t samples_len, uint8_t *out,
				size_t out_cap, size_t *out_len)
{
	Response resp = Response_init_zero;

	if (!out || !out_len || (samples_len > 0 && !samples)) {
		return -EINVAL;
	}
	if (samples_len > sizeof(resp.body.history_frame.samples.bytes)) {
		return -EMSGSIZE;
	}

	resp.seq = seq;
	resp.which_body = Response_history_frame_tag;
	Response_HistoryFrame *hf = &resp.body.history_frame;
	hf->frame_index = frame_index;
	hf->frame_count = frame_count;
	hf->t0_unix = t0_unix;
	hf->present = present;
	hf->interval_s = interval_s;
	memcpy(hf->samples.bytes, samples, samples_len);
	hf->samples.size = samples_len;

	return encode_response(&resp, out, out_cap, out_len);
}
#endif /* APP_CMD_HAVE_HISTORY */

int app_cmd_build_alarm_report(uint32_t base_time, uint32_t total,
			       const struct app_cmd_alarm_event *events, size_t n_events,
			       uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!out || !out_len || (n_events > 0 && !events)) {
		return -EINVAL;
	}

	AlarmReport report = AlarmReport_init_zero;
	report.base_time = base_time;
	report.total = total;

	size_t n = MIN(n_events, ARRAY_SIZE(report.events));
	for (size_t i = 0; i < n; i++) {
		AlarmEvent *ev = &report.events[i];
		ev->source = events[i].source;
		ev->quantity = events[i].quantity;
		ev->edge = (AlarmEvent_Edge)events[i].edge;
		ev->side = (AlarmEvent_Side)events[i].side;
		ev->rel_s = events[i].rel_s;
		ev->has_value = events[i].has_value;
		ev->value = events[i].value;
	}
	report.events_count = (pb_size_t)n;

	pb_ostream_t ostream = pb_ostream_from_buffer(out, out_cap);
	if (!pb_encode(&ostream, AlarmReport_fields, &report)) {
		LOG_ERR_CALL_FAILED_STR("pb_encode", PB_GET_ERROR(&ostream));
		return -EMSGSIZE;
	}

	*out_len = ostream.bytes_written;
	return 0;
}
