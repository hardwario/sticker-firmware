/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_cmd.h"
#include "app_config.h"
#include "app_log.h"

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

static void dispatch(enum app_cmd_transport transport, const DownlinkCommand *cmd,
		     DownlinkResponse *resp)
{
	ARG_UNUSED(transport);

	resp->seq = cmd->seq;

	switch (cmd->which_body) {
	case DownlinkCommand_get_info_tag:
		resp->which_body = DownlinkResponse_info_tag;
		fill_info(&resp->body.info);
		break;

	default:
		LOG_WRN("Command tag %u not implemented in Phase 1", cmd->which_body);
		make_error(resp, DownlinkResponse_Error_Code_UNKNOWN,
			   "not implemented in phase 1");
		break;
	}
}

int app_cmd_handle(enum app_cmd_transport transport, const uint8_t *in, size_t in_len,
		   uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!in || !out || !out_len) {
		return -EINVAL;
	}

	*out_len = 0;

	DownlinkCommand cmd = DownlinkCommand_init_zero;
	DownlinkResponse resp = DownlinkResponse_init_zero;

	pb_istream_t istream = pb_istream_from_buffer(in, in_len);
	if (!pb_decode(&istream, DownlinkCommand_fields, &cmd)) {
		LOG_ERR_CALL_FAILED_STR("pb_decode", PB_GET_ERROR(&istream));
		resp.seq = 0;
		make_error(&resp, DownlinkResponse_Error_Code_BAD_REQUEST,
			   PB_GET_ERROR(&istream));
	} else {
		dispatch(transport, &cmd, &resp);
	}

	pb_ostream_t ostream = pb_ostream_from_buffer(out, out_cap);
	if (!pb_encode(&ostream, DownlinkResponse_fields, &resp)) {
		LOG_ERR_CALL_FAILED_STR("pb_encode", PB_GET_ERROR(&ostream));
		return -EMSGSIZE;
	}

	*out_len = ostream.bytes_written;
	return 0;
}
