/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_cmd.h"
#include "app_alarm_rules.h"
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

/* 1-Wire bus enumeration (W1Scan command). Present only when the DS2484 bridge
 * is configured; the response returns the discovered ROMs so the host can teach
 * a slot via SetParam sensorN_rom. */
#if defined(CONFIG_W1)
#include "app_w1.h"
#define APP_CMD_HAVE_W1 1
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

/* Command handlers share a uniform signature (transport, cmd, resp, action) so
 * the generated app_cmd_dispatch() switch can call any of them the same way; a
 * handler simply ignores the parameters it does not need. They fill `resp`
 * (response body or error) and may set `*action` for deferred work. */
static void app_cmd_handle_set_param(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				     enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	const Command_SetParam *sp = &cmd->body.set_param;
	uint32_t fault = 0;
	int rc = 0;

	/* Apply atomically: snapshot the staging config, apply both sections. On any
	 * fault, restore the snapshot so a rejected batch leaves nothing partially
	 * staged for a later SettingsSave to persist. (Threshold-pair cross-validation
	 * was removed with the fixed alarm keys — alarm rules validate on their own
	 * SET path in app_alarm_rules.) */
	struct app_config snapshot = *app_config();

	if (sp->has_lorawan) {
		rc = app_config_apply_lorawan(&sp->lorawan, &fault);
	}
	if (rc == 0 && sp->has_application) {
		rc = app_config_apply_application(&sp->application, &fault);
	}
	if (rc == 0 && sp->has_sensors) {
		rc = app_config_apply_sensors(&sp->sensors, &fault);
	}
	if (rc == 0 && sp->has_alarms) {
		rc = app_config_apply_alarms(&sp->alarms, &fault);
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

static void app_cmd_handle_get_param(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				     enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(action);
	const Command_GetParam *gp = &cmd->body.get_param;

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
	if (gp->sensors_field_count > 0) {
		resp->body.config_dump.has_sensors = true;
		app_config_fill_sensors(&resp->body.config_dump.sensors, gp->sensors_field,
					gp->sensors_field_count);
	}
	if (gp->alarms_field_count > 0) {
		resp->body.config_dump.has_alarms = true;
		app_config_fill_alarms(&resp->body.config_dump.alarms, gp->alarms_field,
				       gp->alarms_field_count);
	}
}

static void app_cmd_handle_get_info(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				    enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);

	resp->which_body = Response_info_tag;
	fill_info(&resp->body.info);
}

/* Dumpable config fields in fixed order, each with a conservative upper bound
 * on its encoded size (field tag + value; hex fields include the length byte).
 * Mirrors the non-secret fields emitted by app_config_fill_lorawan/application
 * — a new config field needs a row here too (a codegen target once #44 lands).
 * Drives get_config paging: greedy bin-pack into DR0-sized ConfigDump pages. */
/* Sections mirror the config submessages (one fill_<group>() each). Order is
 * the ConfigDump submessage order. */
#define DUMP_SECTION_LORAWAN     0
#define DUMP_SECTION_APPLICATION 1
#define DUMP_SECTION_SENSORS     2
#define DUMP_SECTION_ALARMS      3

#define LRW(tag, size) {DUMP_SECTION_LORAWAN, (tag), (size)}
#define APP(tag)       {DUMP_SECTION_APPLICATION, (tag), 2}
#define SEN(tag, size) {DUMP_SECTION_SENSORS, (tag), (size)}
#define ALM(tag)       {DUMP_SECTION_ALARMS, (tag), 2}

static const struct {
	uint8_t section;
	uint8_t tag;
	uint8_t size;
} DUMP_FIELDS[] = {
	/* Lorawan (non-secret): varints 2 B, deveui/joineui 18 B, devaddr 10 B. */
	LRW(1, 2),
	LRW(2, 2),
	LRW(3, 2),
	LRW(4, 2),
	LRW(5, 18),
	LRW(6, 18),
	LRW(9, 10),
	LRW(12, 2),
	/* Application: calibration, intervals, history (varints, 2 B). */
	APP(1),
	APP(2),
	APP(4),
	APP(49),
	APP(50),
	/* Sensors: caps/enum 2 B, sensorN_rom 8 B -> 18 B hex. */
	SEN(40, 2),
	SEN(41, 2),
	SEN(42, 2),
	SEN(43, 2),
	SEN(44, 2),
	SEN(45, 2),
	SEN(46, 2),
	SEN(54, 2),
	SEN(55, 2),
	SEN(60, 2),
	SEN(56, 18),
	SEN(57, 18),
	SEN(58, 18),
	SEN(59, 18),
	/* Alarms: alarm_limit/notif_time + hall/input counters (varints, 2 B). */
	ALM(51),
	ALM(52),
	ALM(25),
	ALM(28),
	ALM(31),
	ALM(34),
};

/* Per-page byte budget for the field payload inside one ConfigDump. The encoded
 * Response adds ~14 B of fixed overhead around these fields (seq +
 * config_dump wrapper + page_index + page_count + the two submessage wrappers),
 * so the on-air frame is roughly budget + 14. DR0 MTU is 51 B; 30 keeps the
 * worst-case frame near 44 B with margin. Conservative — a page can never
 * overflow (the largest single field is 18 B). */
#define DUMP_PAGE_BUDGET 30

static void app_cmd_handle_get_config(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				      enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(action);
	const Command_GetConfig *gc = &cmd->body.get_config;
	uint32_t page = gc->has_page ? gc->page : 0;

	/* One tag buffer per section, each sized to the whole table so any single
	 * section can hold all of a page's tags without overflow. */
	uint32_t ids[4][ARRAY_SIZE(DUMP_FIELDS)];
	size_t n[4] = {0};

	/* Single greedy pass: pack fields into pages by DUMP_PAGE_BUDGET, collect
	 * the requested page's tags per section, and learn the total page count. */
	uint32_t cur_page = 0, used = 0;
	for (size_t i = 0; i < ARRAY_SIZE(DUMP_FIELDS); i++) {
		if (used > 0 && used + DUMP_FIELDS[i].size > DUMP_PAGE_BUDGET) {
			cur_page++;
			used = 0;
		}
		used += DUMP_FIELDS[i].size;

		if (cur_page == page) {
			uint8_t s = DUMP_FIELDS[i].section;
			ids[s][n[s]++] = DUMP_FIELDS[i].tag;
		}
	}
	uint32_t page_count = cur_page + 1;

	if (page >= page_count) {
		make_error(resp, Response_Error_Code_OUT_OF_RANGE, "page");
		resp->body.error.fault_field = 1;
		return;
	}

	Response_ConfigDump *cd = &resp->body.config_dump;
	resp->which_body = Response_config_dump_tag;
	cd->page_index = page;
	cd->page_count = page_count;

	if (n[DUMP_SECTION_LORAWAN] > 0) {
		cd->has_lorawan = true;
		app_config_fill_lorawan(&cd->lorawan, ids[DUMP_SECTION_LORAWAN],
					n[DUMP_SECTION_LORAWAN]);
	}
	if (n[DUMP_SECTION_APPLICATION] > 0) {
		cd->has_application = true;
		app_config_fill_application(&cd->application, ids[DUMP_SECTION_APPLICATION],
					    n[DUMP_SECTION_APPLICATION]);
	}
	if (n[DUMP_SECTION_SENSORS] > 0) {
		cd->has_sensors = true;
		app_config_fill_sensors(&cd->sensors, ids[DUMP_SECTION_SENSORS],
					n[DUMP_SECTION_SENSORS]);
	}
	if (n[DUMP_SECTION_ALARMS] > 0) {
		cd->has_alarms = true;
		app_config_fill_alarms(&cd->alarms, ids[DUMP_SECTION_ALARMS],
				       n[DUMP_SECTION_ALARMS]);
	}
}

static void app_cmd_handle_reset_counters(enum app_cmd_transport tp, const Command *cmd,
					  Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(action);
	const Command_ResetCounters *rc = &cmd->body.reset_counters;

	app_hall_reset_count(rc->has_hall_left && rc->hall_left,
			     rc->has_hall_right && rc->hall_right);
	app_input_reset_count(rc->has_input_a && rc->input_a, rc->has_input_b && rc->input_b);
	resp->which_body = Response_ack_tag;
}

/* force_send / req_history / clock_sync are LRW-only (transports: [lrw] in the
 * YAML); the generated dispatch enforces that before calling the handler, so
 * the handlers below assume the LoRaWAN transport. */
static void app_cmd_handle_force_send(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				      enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(resp);
	ARG_UNUSED(action);
#if defined(CONFIG_LORAWAN)
	app_lrw_send();
#endif
	/* No ack — the triggered telemetry uplink IS the answer; an extra ack
	 * would just cost a second uplink. Leave which_body == 0 (emit nothing). */
}

static void app_cmd_handle_req_history(enum app_cmd_transport tp, const Command *cmd,
				       Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(action);
#if defined(APP_CMD_HAVE_HISTORY) && defined(CONFIG_LORAWAN)
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
	ARG_UNUSED(cmd);
	make_error(resp, Response_Error_Code_HISTORY_UNAVAILABLE, "no history");
#endif
}

static void app_cmd_handle_alarm_rule(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				      enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	const Command_AlarmRule *ar = &cmd->body.alarm_rule;
	int ret;

	/* Mutate the in-RAM rule list here (cheap), but DEFER the NVS persist to the
	 * post-command action. settings_save_one() is too stack-heavy to run on the
	 * m_work_q (2048 B) while app_cmd_handle's Command/Response locals are still
	 * live on the stack — doing it inline overflowed the stack on a real device.
	 * The deferred action runs after this frame unwinds. */
	switch (ar->op) {
	case Command_AlarmRule_Op_CLEAR_ALL:
		app_alarm_rules_clear_all();
		ret = 0;
		break;
	case Command_AlarmRule_Op_CLEAR:
		ret = app_alarm_rules_clear((enum app_alarm_source)ar->source,
					    (enum app_alarm_quantity)ar->quantity);
		break;
	case Command_AlarmRule_Op_SET:
	default: {
		struct app_alarm_rule r = {
			.source = (uint8_t)ar->source,
			.quantity = (uint8_t)ar->quantity,
			.enabled = ar->enabled ? 1 : 0,
			.lo = ar->has_lo ? ar->lo : 0.0f,
			.hi = ar->has_hi ? ar->hi : 0.0f,
			.hst = ar->has_hst ? ar->hst : 0.0f,
			.from_state = (uint8_t)((ar->has_from_state && ar->from_state) ? 1 : 0),
			.to_state = (uint8_t)((ar->has_to_state && ar->to_state) ? 1 : 0),
		};
		ret = app_alarm_rules_set(&r);
		break;
	}
	}

	if (ret == 0) {
		*action = APP_CMD_ACTION_ALARM_RULES_SAVE;
		resp->which_body = Response_ack_tag;
	} else {
		make_error(resp,
			   ret == -EINVAL ? Response_Error_Code_OUT_OF_RANGE
					  : Response_Error_Code_NOT_READY,
			   "alarm rule");
	}
}

#if defined(APP_CMD_HAVE_W1)
/* w1_scan ROM-discovery callback: append each ROM (8 bytes: family + 6-byte
 * serial + CRC) to the response, capped at the field's max_count. */
static int w1_scan_cb(struct w1_rom rom, void *user_data)
{
	Response_W1Scan *w1 = user_data;

	if (w1->rom_count >= ARRAY_SIZE(w1->rom)) {
		return 0; /* response is full; ignore the rest */
	}

	BUILD_ASSERT(sizeof(rom) == 8, "w1_rom must be 8 bytes");
	w1->rom[w1->rom_count].size = sizeof(rom);
	memcpy(w1->rom[w1->rom_count].bytes, &rom, sizeof(rom));
	w1->rom_count++;

	return 0;
}

#endif /* APP_CMD_HAVE_W1 */

static void app_cmd_handle_clock_sync(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				      enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);
#if defined(APP_CMD_HAVE_CLOCK) && defined(CONFIG_LORAWAN)
	ARG_UNUSED(resp);
	/* Re-sync, then answer with an Info uplink once the network time lands
	 * (carries the synced unix_time). No ack — see app_lrw. */
	app_clock_force_resync();
	app_lrw_send_info_on_clock_sync();
#else
	resp->which_body = Response_ack_tag; /* no clock/LRW: just confirm */
#endif
}

static void app_cmd_handle_w1_scan(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				   enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);
#if defined(APP_CMD_HAVE_W1)
	static const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ds2484));
	struct app_w1 w1 = {0};
	int ret;

	if (!device_is_ready(dev)) {
		make_error(resp, Response_Error_Code_NOT_READY, "1-wire bus");
		return;
	}

	ret = app_w1_acquire(&w1, dev);
	if (ret) {
		make_error(resp, Response_Error_Code_NOT_READY, "1-wire acquire");
		return;
	}

	resp->which_body = Response_w1_scan_tag;
	resp->body.w1_scan.rom_count = 0;
	ret = app_w1_scan(&w1, dev, w1_scan_cb, &resp->body.w1_scan);

	(void)app_w1_release(&w1, dev);

	if (ret < 0) {
		make_error(resp, Response_Error_Code_NOT_READY, "1-wire scan");
	}
#else
	make_error(resp, Response_Error_Code_NOT_READY, "no 1-wire");
#endif
}

// BEGIN GENERATED DISPATCH
static void app_cmd_dispatch(enum app_cmd_transport tp, const Command *cmd, Response *resp,
			     enum app_cmd_action *action)
{
	resp->seq = cmd->seq;

	switch (cmd->which_body) {
	case Command_set_param_tag:
		app_cmd_handle_set_param(tp, cmd, resp, action);
		break;
	case Command_get_param_tag:
		app_cmd_handle_get_param(tp, cmd, resp, action);
		break;
	case Command_get_info_tag:
		app_cmd_handle_get_info(tp, cmd, resp, action);
		break;
	case Command_get_config_tag:
		app_cmd_handle_get_config(tp, cmd, resp, action);
		break;
	case Command_settings_save_tag:
		*action = APP_CMD_ACTION_SETTINGS_SAVE;
		resp->which_body = Response_ack_tag;
		break;
	case Command_reboot_tag:
		*action = APP_CMD_ACTION_REBOOT;
		resp->which_body = Response_ack_tag;
		break;
	case Command_factory_reset_tag:
		*action = APP_CMD_ACTION_FACTORY_RESET;
		resp->which_body = Response_ack_tag;
		break;
	case Command_force_send_tag:
		/* transports: [lrw] — the answer is an uplink, meaningless over NFC */
		if (tp != APP_CMD_TRANSPORT_LRW) {
			make_error(resp, Response_Error_Code_NOT_READY, "lrw only");
			break;
		}
		app_cmd_handle_force_send(tp, cmd, resp, action);
		break;
	case Command_reset_counters_tag:
		app_cmd_handle_reset_counters(tp, cmd, resp, action);
		break;
	case Command_req_history_tag:
		/* transports: [lrw] — the answer is an uplink, meaningless over NFC */
		if (tp != APP_CMD_TRANSPORT_LRW) {
			make_error(resp, Response_Error_Code_NOT_READY, "lrw only");
			break;
		}
		app_cmd_handle_req_history(tp, cmd, resp, action);
		break;
	case Command_clock_sync_tag:
		/* transports: [lrw] — the answer is an uplink, meaningless over NFC */
		if (tp != APP_CMD_TRANSPORT_LRW) {
			make_error(resp, Response_Error_Code_NOT_READY, "lrw only");
			break;
		}
		app_cmd_handle_clock_sync(tp, cmd, resp, action);
		break;
	case Command_alarm_rule_tag:
		app_cmd_handle_alarm_rule(tp, cmd, resp, action);
		break;
	case Command_w1_scan_tag:
		app_cmd_handle_w1_scan(tp, cmd, resp, action);
		break;
	default:
		LOG_WRN("Command tag %u not implemented", cmd->which_body);
		make_error(resp, Response_Error_Code_UNKNOWN, "not implemented");
		break;
	}
}
// END GENERATED DISPATCH

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
		app_cmd_dispatch(transport, &cmd, &resp, &act);
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
