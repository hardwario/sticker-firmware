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
#include "app_nfc_ingest.h"

/* Wall-clock source (PR #41, branch lrw-rtc-time). Until that lands on this
 * branch, app_clock.h is absent and Info.unix_time stays 0 (omitted by proto3).
 * The __has_include guard flips on automatically once the module is merged. */
#if defined(__has_include) && __has_include("app_clock.h")
#include "app_clock.h"
#define APP_CMD_HAVE_CLOCK 1
#endif

/* Nanopb includes */
#include <pb_decode.h>
#include <pb_encode.h>
#include "src/nfc_config.pb.h"

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

static void fill_info(DownlinkResponse_Info *info)
{
	info->fw_major = APP_VERSION_MAJOR;
	info->fw_minor = APP_VERSION_MINOR;
	info->fw_patch = APP_VERSION_PATCH;
	info->build_type = (DownlinkResponse_Info_BuildType)APP_BUILD_TYPE;
	info->serial_number = g_app_config.serial_number;
	info->uptime_s = (uint32_t)(k_uptime_get() / 1000);
	info->debug = IS_ENABLED(CONFIG_FW_DEBUG);

#ifdef APP_CMD_HAVE_CLOCK
	uint32_t unix_s;
	if (app_clock_get_unix(&unix_s) == 0) {
		info->unix_time = unix_s;
	}
#endif
}

static void make_error(DownlinkResponse *resp, DownlinkResponse_Error_Code code,
		       const char *detail)
{
	resp->which_body = DownlinkResponse_error_tag;
	resp->body.error.code = code;
	resp->body.error.fault_field = 0;

	if (detail) {
		strncpy(resp->body.error.detail, detail, sizeof(resp->body.error.detail) - 1);
		resp->body.error.detail[sizeof(resp->body.error.detail) - 1] = '\0';
	} else {
		resp->body.error.detail[0] = '\0';
	}
}

static void handle_set_param(const DownlinkCommand_SetParam *sp, DownlinkResponse *resp)
{
	uint32_t fault = 0;
	int rc = 0;

	if (sp->has_lorawan) {
		rc = app_config_apply_lorawan(&sp->lorawan, &fault);
	}
	if (rc == 0 && sp->has_application) {
		rc = app_config_apply_application(&sp->application, &fault);
	}

	if (rc) {
		make_error(resp, DownlinkResponse_Error_Code_OUT_OF_RANGE, "invalid value");
		resp->body.error.fault_field = fault;
	} else {
		resp->which_body = DownlinkResponse_ack_tag;
	}
}

static void handle_get_param(const DownlinkCommand_GetParam *gp, DownlinkResponse *resp)
{
	resp->which_body = DownlinkResponse_config_dump_tag;
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

static void handle_reset_counters(const DownlinkCommand_ResetCounters *rc, DownlinkResponse *resp)
{
	app_hall_reset_count(rc->has_hall_left && rc->hall_left,
			     rc->has_hall_right && rc->hall_right);
	app_input_reset_count(rc->has_input_a && rc->input_a, rc->has_input_b && rc->input_b);
	resp->which_body = DownlinkResponse_ack_tag;
}

static void dispatch(enum app_cmd_transport transport, const DownlinkCommand *cmd,
		     DownlinkResponse *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(transport);

	resp->seq = cmd->seq;

	switch (cmd->which_body) {
	case DownlinkCommand_get_info_tag:
		resp->which_body = DownlinkResponse_info_tag;
		fill_info(&resp->body.info);
		break;

	case DownlinkCommand_set_param_tag:
		handle_set_param(&cmd->body.set_param, resp);
		break;

	case DownlinkCommand_get_param_tag:
		handle_get_param(&cmd->body.get_param, resp);
		break;

	case DownlinkCommand_settings_save_tag:
		resp->which_body = DownlinkResponse_ack_tag;
		*action = APP_CMD_ACTION_SETTINGS_SAVE;
		break;

	case DownlinkCommand_reboot_tag:
		resp->which_body = DownlinkResponse_ack_tag;
		*action = APP_CMD_ACTION_REBOOT;
		break;

	case DownlinkCommand_factory_reset_tag:
		resp->which_body = DownlinkResponse_ack_tag;
		*action = APP_CMD_ACTION_FACTORY_RESET;
		break;

	case DownlinkCommand_force_send_tag:
#if defined(CONFIG_LORAWAN)
		app_lrw_send();
#endif
		resp->which_body = DownlinkResponse_ack_tag;
		break;

	case DownlinkCommand_reset_counters_tag:
		handle_reset_counters(&cmd->body.reset_counters, resp);
		break;

	case DownlinkCommand_clock_sync_tag:
#ifdef APP_CMD_HAVE_CLOCK
		app_clock_force_resync();
#endif
		resp->which_body = DownlinkResponse_ack_tag;
		break;

	default:
		LOG_WRN("Command tag %u not implemented", cmd->which_body);
		make_error(resp, DownlinkResponse_Error_Code_UNKNOWN, "not implemented");
		break;
	}
}

int app_cmd_handle(enum app_cmd_transport transport, const uint8_t *in, size_t in_len,
		   uint8_t *out, size_t out_cap, size_t *out_len, enum app_cmd_action *action)
{
	if (!in || !out || !out_len) {
		return -EINVAL;
	}

	*out_len = 0;
	enum app_cmd_action act = APP_CMD_ACTION_NONE;

	DownlinkCommand cmd = DownlinkCommand_init_zero;
	DownlinkResponse resp = DownlinkResponse_init_zero;

	pb_istream_t istream = pb_istream_from_buffer(in, in_len);
	if (!pb_decode(&istream, DownlinkCommand_fields, &cmd)) {
		LOG_ERR_CALL_FAILED_STR("pb_decode", PB_GET_ERROR(&istream));
		resp.seq = 0;
		make_error(&resp, DownlinkResponse_Error_Code_BAD_REQUEST,
			   PB_GET_ERROR(&istream));
	} else {
		dispatch(transport, &cmd, &resp, &act);
	}

	pb_ostream_t ostream = pb_ostream_from_buffer(out, out_cap);
	if (!pb_encode(&ostream, DownlinkResponse_fields, &resp)) {
		LOG_ERR_CALL_FAILED_STR("pb_encode", PB_GET_ERROR(&ostream));
		return -EMSGSIZE;
	}

	*out_len = ostream.bytes_written;
	if (action) {
		*action = act;
	}
	return 0;
}
