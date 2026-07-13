/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_cmd.h"
#include "app_alarm.h"
#include "app_alarm_rules.h"
#include "app_battery.h"
#include "app_compose.h"
#include "app_config.h"
#include "app_hall.h"
#include "app_input.h"
#include "app_log.h"
#include "app_lrw.h"
#include "app_nfc.h"
#include "app_report.h"
#include "app_sensor.h"
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
#include <math.h>
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

/* hwinfo reset-cause bitmask of the last boot, cached by main() (#88). */
static uint32_t m_reset_cause;

void app_cmd_set_reset_cause(uint32_t cause)
{
	m_reset_cause = cause;
}

/* F-1: SetParam mutates the staging config (m_app_config); the running copy
 * (g_app_config) — which lrw_join/lrw_reset build the join from — is only
 * re-synced from staging at boot (h_commit) or factory reset, both of which
 * reboot. This flag tracks "staging has LoRaWAN changes not yet in the running
 * copy" so a join issued before `settings save` (persist + reboot → re-sync)
 * is rejected loudly instead of silently joining with the OLD keys while
 * GetParam echoes the NEW ones. It is a plain static: every g_app_config
 * re-sync path reboots (settings save / factory reset both sys_reboot COLD),
 * which zeroes it — so there is no non-reboot re-sync to clear it on. Kept
 * local to app_cmd.c (the only setter + reader) so the generated app_config.c
 * stays byte-identical to configen output. */
static bool m_lrw_staging_dirty;

#ifdef CONFIG_ZTEST
/* Test hook: drive the F-1 dirty flag directly. Production sets it only on the
 * SetParam(lorawan) apply path and clears it via reboot; the ztest has no
 * reboot, so it toggles the flag through here (reset_cfg clears it). */
void test_set_lrw_dirty(bool v)
{
	m_lrw_staging_dirty = v;
}
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
		.reset_cause = m_reset_cause,
	};

	BUILD_ASSERT(sizeof(info->claim_token) == sizeof(g_app_config.claim_token),
		     "claim_token size mismatch");
	memcpy(info->claim_token, g_app_config.claim_token, sizeof(info->claim_token));

#ifdef CONFIG_LORAWAN
	info->lrw_state = (uint8_t)app_lrw_get_state();
#endif

	BUILD_ASSERT(sizeof(info->dev_eui) == sizeof(g_app_config.lrw_deveui),
		     "dev_eui size mismatch");
	memcpy(info->dev_eui, g_app_config.lrw_deveui, sizeof(info->dev_eui));

	/* Battery reading (mV) from the last sensor sample's cached value, NOT a
	 * fresh app_battery_measure() here. get_info runs on the boot path (NFC inf
	 * record + LoRaWAN DeviceInfo) before the ADC clock has settled, and a
	 * synchronous ADC read that early hangs the release build (no CONFIG_LOG
	 * timing slack), starving the watchdog and reset-looping before main(). The
	 * sensor work queue takes the first sample right after init, so the cache
	 * holds a real reading by the time DeviceInfo is sent. 0/NaN before the first
	 * sample -> proto omits it and the host treats battery as "unknown". */
	k_mutex_lock(&g_app_sensor_data_lock, K_FOREVER);
	float v = g_app_sensor_data.voltage;
	k_mutex_unlock(&g_app_sensor_data_lock);
	info->battery_mv = (isfinite(v) && v > 0.f) ? (uint32_t)(v * 1000.0f) : 0;

#ifdef APP_CMD_HAVE_CLOCK
	uint32_t unix_s;
	if (app_clock_get_unix(&unix_s) == 0) {
		info->has_unix_time = true;
		info->unix_time = unix_s;
	}
#endif

	/* Aggregate device status (APP_DEVICE_STATUS_* bitmask). Built here so the
	 * GetInfo response and `ats device info` share one source of truth. Alarm
	 * bits come from the read-only app_alarm_status_flags() (no side effects,
	 * unlike app_alarm_poll). Runs after the clock block so TIME_UNSYNCED
	 * reflects has_unix_time. */
	uint32_t status = app_alarm_status_flags();
	if (!app_nfc_ready()) {
		status |= APP_DEVICE_STATUS_NFC_DOWN;
	}
#ifdef APP_CMD_HAVE_HISTORY
	if (!app_history_is_ready()) {
		status |= APP_DEVICE_STATUS_HISTORY_DOWN;
	}
#endif
	if (app_sensor_i2c_wedged()) {
		status |= APP_DEVICE_STATUS_I2C_WEDGED;
	}
	if (!info->has_unix_time) {
		status |= APP_DEVICE_STATUS_TIME_UNSYNCED;
	}
	if (info->lrw_state == APP_LRW_STATE_DISABLED) {
		status |= APP_DEVICE_STATUS_LRW_DISABLED;
	}
	info->device_status = status;
}

/* nanopb encode callback for Response_Info.active_alarms (field 15): emit one
 * AlarmStatus submessage per alarm latched active at encode time. Snapshotting
 * here keeps the list variable-length (only the active alarms are sent, empty
 * when all is well) and avoids a large static array in the Info struct — the
 * debug build's RAM is tight. Shared by the LoRaWAN and NFC info paths. */
#define ACTIVE_ALARM_SNAPSHOT_MAX (APP_ALARM_SLOT_COUNT + 9) /* +8 no-data +1 battery */

static bool encode_active_alarms(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
	ARG_UNUSED(arg);

	struct app_alarm_active list[ACTIVE_ALARM_SNAPSHOT_MAX];
	size_t n = app_alarm_active_snapshot(list, ARRAY_SIZE(list));

	for (size_t i = 0; i < n; i++) {
		Response_AlarmStatus e = Response_AlarmStatus_init_zero;
		e.source = list[i].source;
		e.quantity = list[i].quantity;
		e.type = list[i].type;
		if (!pb_encode_tag_for_field(stream, field)) {
			return false;
		}
		if (!pb_encode_submessage(stream, Response_AlarmStatus_fields, &e)) {
			return false;
		}
	}
	return true;
}

/* Map the plain-C info snapshot onto the protobuf Response_Info. The transport
 * splits the Info: a LoRaWAN uplink carries only the fields the network side needs
 * (firmware / serial / uptime / battery / reset-cause / clock), while the NFC
 * (commissioning) channel additionally gets lrw_state and dev_eui — see the
 * NFC-only block below. */
static void fill_info(enum app_cmd_transport tp, Response_Info *info)
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
	info->battery = i.battery_mv;
	info->reset_cause = i.reset_cause;
	info->device_status = i.device_status;
	/* Itemized active alarms (field 15), emitted on both transports via a
	 * callback that snapshots the live alarm state at encode time. */
	info->active_alarms.funcs.encode = encode_active_alarms;
	if (i.has_unix_time) {
		info->unix_time = i.unix_time;
	}

	/* claim_token (#170): emit only once commissioned (any non-zero byte). The
	 * all-zero "unset" state is omitted so an uncommissioned device's Info stays
	 * compact. fixed_length bytes -> plain 16-byte array, no .size. */
	for (size_t j = 0; j < sizeof(i.claim_token); j++) {
		if (i.claim_token[j] != 0) {
			info->has_claim_token = true;
			memcpy(info->claim_token, i.claim_token, sizeof(info->claim_token));
			break;
		}
	}

	/* NFC-only Info fields. The phone/commissioning channel gets the full picture;
	 * a LoRaWAN uplink omits them — dev_eui would leak the identity onto the air
	 * (and the LNS already knows it), and lrw_state is redundant on a frame the
	 * network just received. dev_eui is further omitted when unset (all-zero). */
	if (tp == APP_CMD_TRANSPORT_NFC) {
		info->has_lrw_state = true;
		info->lrw_state = (Response_Info_LrwState)i.lrw_state;

		for (size_t j = 0; j < sizeof(i.dev_eui); j++) {
			if (i.dev_eui[j] != 0) {
				info->has_dev_eui = true;
				memcpy(info->dev_eui, i.dev_eui, sizeof(info->dev_eui));
				break;
			}
		}
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
	const Command_SetParam *sp = &cmd->body.set_param;

	uint32_t fault = 0;
	/* Group that produced the fault, folded into fault_field as group*100 + tag so
	 * the host can disambiguate the tag across groups (#196): 1=lorawan
	 * 2=application 3=sensors 4=alarms. */
	uint32_t fault_group = 0;
	int rc = 0;

	/* Apply atomically: snapshot the staging config, apply both sections. On any
	 * fault, restore the snapshot so a rejected batch leaves nothing partially
	 * staged for a later SettingsSave to persist. (Threshold-pair cross-validation
	 * was removed with the fixed alarm keys — alarm rules validate on their own
	 * SET path in app_alarm_rules.) */
	struct app_config snapshot = *app_config();

	if (sp->has_lorawan) {
		rc = app_config_apply_lorawan(tp, &sp->lorawan, &fault);
		fault_group = 1;
	}
	if (rc == 0 && sp->has_application) {
		rc = app_config_apply_application(tp, &sp->application, &fault);
		fault_group = 2;
	}
	if (rc == 0 && sp->has_sensors) {
		rc = app_config_apply_sensors(tp, &sp->sensors, &fault);
		fault_group = 3;
	}
	if (rc == 0 && sp->has_alarms) {
		rc = app_config_apply_alarms(tp, &sp->alarms, &fault);
		fault_group = 4;
	}

	if (rc) {
		*app_config() = snapshot; /* roll back the whole batch */
		/* M-3: a field not writable over this transport returns -EACCES → report
		 * NOT_WRITABLE (the field exists but this transport may not set it — e.g.
		 * the lorawan provisioning/identity group over a LoRaWAN downlink); a bad
		 * value returns -EINVAL → OUT_OF_RANGE. Either way fault_field pinpoints
		 * the offending field (group*100 + tag). */
		if (rc == -EACCES) {
			make_error(resp, Response_Error_Code_NOT_WRITABLE, "transport not allowed");
		} else {
			make_error(resp, Response_Error_Code_OUT_OF_RANGE, "invalid value");
		}
		resp->body.error.fault_field = fault_group * 100 + fault;
	} else {
		/* Alarm rules staged into the config slots only take effect once the
		 * decoded rule cache is rebuilt; do it here so they go live without a
		 * reboot (whether or not this batch is persisted). reload sanitizes and
		 * reports any rule that fails validation — surface that as a fault instead
		 * of a misleading ACK for a rule that was silently dropped (H-10). */
		if (sp->has_alarms && app_alarm_rules_reload_from_config() > 0) {
			*app_config() = snapshot;                   /* roll back the batch */
			(void)app_alarm_rules_reload_from_config(); /* resync cache to it */
			make_error(resp, Response_Error_Code_OUT_OF_RANGE, "invalid alarm rule");
			resp->body.error.fault_field = 4 * 100; /* group 4 = alarms */
			return;
		}
		/* F-1: LoRaWAN staging changed but the running copy (used by
		 * lrw_join/lrw_reset) only re-syncs on save+reboot. Flag it so a join
		 * before save is rejected instead of silently using the OLD keys. If
		 * this batch saves below, the reboot zeroes the flag. */
		if (sp->has_lorawan) {
			m_lrw_staging_dirty = true;
		}
		resp->which_body = Response_ack_tag;
		/* Optional one-shot commit: persist staged config + reboot, same path
		 * as SettingsSave. Used as the last message of a multi-downlink batch. */
		if (sp->has_save && sp->save) {
			*action = APP_CMD_ACTION_SETTINGS_SAVE;
		}
	}
}

/* app_cmd_handle_get_param is defined after DUMP_FIELDS (it pages like
 * get_config); see below. */

static void app_cmd_handle_get_info(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				    enum app_cmd_action *action)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);

	resp->which_body = Response_info_tag;
	fill_info(tp, &resp->body.info);
}

/* Dumpable config fields in fixed order, each with a conservative upper bound
 * on its encoded size (field tag + value; hex fields include the length byte).
 * Mirrors the non-secret fields emitted by app_config_fill_<group>().
 * Drives get_config paging: greedy bin-pack into DR0-sized ConfigDump pages.
 *
 * The rows are GENERATED from app_config.yml by `west configen` (#112): one per
 * dumpable parameter (proto_group in a ConfigDump section, not `dump: false`),
 * with the encoded-size bound derived from its type. Do not edit by hand. */
/* Sections mirror the config submessages (one fill_<group>() each). Order is
 * the ConfigDump submessage order. */
#define DUMP_SECTION_LORAWAN     0
#define DUMP_SECTION_APPLICATION 1
#define DUMP_SECTION_SENSORS     2
#define DUMP_SECTION_ALARMS      3

static const struct {
	uint8_t section;
	uint8_t tag;
	uint8_t size;
	bool nfc_only; /* only dumped over NFC (e.g. LoRaWAN keys) — never over LoRaWAN */
} DUMP_FIELDS[] = {
	// BEGIN GENERATED DUMP_FIELDS
	{DUMP_SECTION_LORAWAN, 1, 2, false},     {DUMP_SECTION_LORAWAN, 15, 2, false},
	{DUMP_SECTION_LORAWAN, 2, 2, false},     {DUMP_SECTION_LORAWAN, 3, 2, false},
	{DUMP_SECTION_LORAWAN, 4, 2, false},     {DUMP_SECTION_LORAWAN, 5, 2, false},
	{DUMP_SECTION_LORAWAN, 6, 10, false},    {DUMP_SECTION_LORAWAN, 7, 10, false},
	{DUMP_SECTION_LORAWAN, 8, 18, true},     {DUMP_SECTION_LORAWAN, 9, 18, true},
	{DUMP_SECTION_LORAWAN, 10, 6, false},    {DUMP_SECTION_LORAWAN, 11, 18, true},
	{DUMP_SECTION_LORAWAN, 12, 18, true},    {DUMP_SECTION_LORAWAN, 13, 3, false},
	{DUMP_SECTION_LORAWAN, 14, 3, false},    {DUMP_SECTION_APPLICATION, 1, 2, false},
	{DUMP_SECTION_APPLICATION, 2, 3, false}, {DUMP_SECTION_APPLICATION, 3, 4, false},
	{DUMP_SECTION_APPLICATION, 4, 2, false}, {DUMP_SECTION_APPLICATION, 5, 6, false},
	{DUMP_SECTION_APPLICATION, 6, 3, false}, {DUMP_SECTION_APPLICATION, 7, 2, false},
	{DUMP_SECTION_SENSORS, 1, 2, false},     {DUMP_SECTION_SENSORS, 2, 2, false},
	{DUMP_SECTION_SENSORS, 3, 2, false},     {DUMP_SECTION_SENSORS, 4, 2, false},
	{DUMP_SECTION_SENSORS, 5, 2, false},     {DUMP_SECTION_SENSORS, 6, 2, false},
	{DUMP_SECTION_SENSORS, 7, 2, false},     {DUMP_SECTION_SENSORS, 8, 2, false},
	{DUMP_SECTION_SENSORS, 9, 2, false},     {DUMP_SECTION_SENSORS, 10, 2, false},
	{DUMP_SECTION_SENSORS, 11, 10, false},   {DUMP_SECTION_SENSORS, 12, 10, false},
	{DUMP_SECTION_SENSORS, 13, 10, false},   {DUMP_SECTION_SENSORS, 14, 10, false},
	{DUMP_SECTION_SENSORS, 15, 2, false},    {DUMP_SECTION_SENSORS, 16, 3, false},
	{DUMP_SECTION_SENSORS, 17, 3, false},    {DUMP_SECTION_SENSORS, 18, 3, false},
	{DUMP_SECTION_ALARMS, 1, 3, false},      {DUMP_SECTION_ALARMS, 2, 2, false},
	{DUMP_SECTION_ALARMS, 3, 19, false},     {DUMP_SECTION_ALARMS, 4, 19, false},
	{DUMP_SECTION_ALARMS, 5, 19, false},     {DUMP_SECTION_ALARMS, 6, 19, false},
	{DUMP_SECTION_ALARMS, 7, 19, false},     {DUMP_SECTION_ALARMS, 8, 19, false},
	{DUMP_SECTION_ALARMS, 9, 19, false},     {DUMP_SECTION_ALARMS, 10, 19, false},
	{DUMP_SECTION_ALARMS, 11, 19, false},    {DUMP_SECTION_ALARMS, 12, 19, false},
	{DUMP_SECTION_ALARMS, 13, 19, false},    {DUMP_SECTION_ALARMS, 14, 19, false},
	{DUMP_SECTION_ALARMS, 15, 19, false},    {DUMP_SECTION_ALARMS, 16, 20, false},
	{DUMP_SECTION_ALARMS, 17, 20, false},    {DUMP_SECTION_ALARMS, 18, 20, false},
	// END GENERATED DUMP_FIELDS
};

/* Per-page byte budget for the field payload inside one ConfigDump. The encoded
 * Response adds ~14 B of fixed overhead around these fields (seq +
 * config_dump wrapper + page_index + page_count + the two submessage wrappers),
 * so the on-air frame is roughly budget + 14. DR0 MTU is 51 B; 30 keeps the
 * worst-case frame near 44 B with margin. Conservative — a page can never
 * overflow (the largest single field is 20 B: a 17-byte alarm rule with a
 * two-byte tag). */
#define DUMP_PAGE_BUDGET 30

/* Over NFC the response lands in the ST25DV user memory, so config can page far
 * coarser than the tiny DR0 LoRaWAN frame — a whole snapshot in ~2 pages instead
 * of ~30, read in one RF session (GetConfig page 0..page_count-1, each rewritten
 * to the tag on request). Bounded by the encrypted-response buffer (m_resp_buf,
 * 512 B) AND the 512 B ST25DV user memory the rsp NDEF record is written into:
 * the on-tag record is ~22 B framing + encrypted(8 B header + plaintext + 16 B
 * tag), so encrypted <= 490 -> plaintext <= 466 -> minus version + the ~14 B
 * Response/ConfigDump wrapper leaves ~452 B for field payload. The budget is in
 * DUMP_FIELDS.size units, which over-estimate native byte fields ~2x (so real
 * bytes <= budget), hence 450 keeps the worst-case page on the tag with margin
 * (encrypted ~474 + 22 framing ~= 496 <= 512). LoRaWAN keeps the small budget
 * (DR0 MTU). */
#define DUMP_PAGE_BUDGET_NFC 450

/* Per-page budget for the given transport (NFC pages coarsely, LoRaWAN tightly). */
static inline uint32_t dump_page_budget(enum app_cmd_transport tp)
{
	return (tp == APP_CMD_TRANSPORT_NFC) ? DUMP_PAGE_BUDGET_NFC : DUMP_PAGE_BUDGET;
}

static void app_cmd_handle_get_config(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				      enum app_cmd_action *action)
{
	ARG_UNUSED(action);
	const Command_GetConfig *gc = &cmd->body.get_config;
	uint32_t page = gc->has_page ? gc->page : 0;
	/* NFC-only fields (LoRaWAN keys) are read-back exclusively over the encrypted
	 * NFC channel; never include them in a LoRaWAN response (the fPort-85 payload
	 * is plain protobuf — the network server would see the keys). They are skipped
	 * for paging too, so page layout is consistent within a transport. */
	const bool allow_nfc_only = (tp == APP_CMD_TRANSPORT_NFC);

	/* Tags that land on the requested page, grouped by section. DUMP_FIELDS is
	 * ordered by section, so each section's tags form a contiguous run here;
	 * off[]/n[] record each run's start and length. Sized flat to the whole
	 * table (one NFC page can hold every field) — a per-section [4][N] matrix
	 * would cost ~4x the stack and overflowed the handler thread (#176). */
	uint32_t ids[ARRAY_SIZE(DUMP_FIELDS)];
	size_t off[4] = {0}, n[4] = {0};
	size_t total = 0;

	/* Single greedy pass: pack fields into pages by DUMP_PAGE_BUDGET, collect
	 * the requested page's tags per section, and learn the total page count. */
	uint32_t cur_page = 0, used = 0;
	for (size_t i = 0; i < ARRAY_SIZE(DUMP_FIELDS); i++) {
		if (DUMP_FIELDS[i].nfc_only && !allow_nfc_only) {
			continue;
		}
		/* Empty (all-zero) alarm slots are omitted by app_config_fill_alarms(),
		 * so they take no page budget and no tag — an unprovisioned device pages
		 * its whole config into far fewer frames (page_count stays exact). */
		if (DUMP_FIELDS[i].section == DUMP_SECTION_ALARMS &&
		    app_config_alarms_slot_empty(DUMP_FIELDS[i].tag)) {
			continue;
		}
		if (used > 0 && used + DUMP_FIELDS[i].size > dump_page_budget(tp)) {
			cur_page++;
			used = 0;
		}
		used += DUMP_FIELDS[i].size;

		if (cur_page == page) {
			uint8_t s = DUMP_FIELDS[i].section;
			if (n[s] == 0) {
				off[s] = total;
			}
			ids[total++] = DUMP_FIELDS[i].tag;
			n[s]++;
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
		app_config_fill_lorawan(&cd->lorawan, &ids[off[DUMP_SECTION_LORAWAN]],
					n[DUMP_SECTION_LORAWAN]);
	}
	if (n[DUMP_SECTION_APPLICATION] > 0) {
		cd->has_application = true;
		app_config_fill_application(&cd->application, &ids[off[DUMP_SECTION_APPLICATION]],
					    n[DUMP_SECTION_APPLICATION]);
	}
	if (n[DUMP_SECTION_SENSORS] > 0) {
		cd->has_sensors = true;
		app_config_fill_sensors(&cd->sensors, &ids[off[DUMP_SECTION_SENSORS]],
					n[DUMP_SECTION_SENSORS]);
	}
	if (n[DUMP_SECTION_ALARMS] > 0) {
		cd->has_alarms = true;
		app_config_fill_alarms(&cd->alarms, &ids[off[DUMP_SECTION_ALARMS]],
				       n[DUMP_SECTION_ALARMS]);
	}
}

/* Encoded-size bound for a (section, tag) from DUMP_FIELDS. Returns false for a
 * field that isn't dumpable (secret / unknown id): such ids never appear in the
 * response (app_config_fill_<group>() skips them) so they take no page budget. */
static bool dump_field_size(uint8_t section, uint32_t tag, uint8_t *size, bool *nfc_only)
{
	for (size_t i = 0; i < ARRAY_SIZE(DUMP_FIELDS); i++) {
		if (DUMP_FIELDS[i].section == section && DUMP_FIELDS[i].tag == tag) {
			*size = DUMP_FIELDS[i].size;
			*nfc_only = DUMP_FIELDS[i].nfc_only;
			return true;
		}
	}
	return false;
}

static void app_cmd_handle_get_param(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				     enum app_cmd_action *action)
{
	ARG_UNUSED(action);
	const Command_GetParam *gp = &cmd->body.get_param;
	uint32_t page = gp->has_page ? gp->page : 0;
	/* NFC-only fields (LoRaWAN keys) are returned over the encrypted NFC channel
	 * only — never in a LoRaWAN response (see get_config). */
	const bool allow_nfc_only = (tp == APP_CMD_TRANSPORT_NFC);

	/* Requested ids per section, in ConfigDump section order. DUMP_SECTION_*
	 * equals the index here (0..3). */
	const uint32_t *req_ids[4] = {gp->lorawan_field, gp->application_field, gp->sensors_field,
				      gp->alarms_field};
	const size_t req_n[4] = {gp->lorawan_field_count, gp->application_field_count,
				 gp->sensors_field_count, gp->alarms_field_count};

	/* Collected ids for the requested page, one buffer per section sized to its
	 * own request array (the page can't hold more than was requested). */
	uint32_t lw[ARRAY_SIZE(gp->lorawan_field)], ap[ARRAY_SIZE(gp->application_field)],
		se[ARRAY_SIZE(gp->sensors_field)], al[ARRAY_SIZE(gp->alarms_field)];
	uint32_t *out_ids[4] = {lw, ap, se, al};
	size_t out_n[4] = {0};

	/* One greedy pass over all requested (dumpable) ids, continuous across
	 * sections like get_config: pack into DR0-sized pages by DUMP_PAGE_BUDGET
	 * and collect the requested page's tags per section. */
	uint32_t cur_page = 0, used = 0;
	for (uint8_t s = 0; s < 4; s++) {
		for (size_t j = 0; j < req_n[s]; j++) {
			/* Skip a field id already requested earlier in this section: a
			 * duplicate would otherwise be counted twice against the page budget
			 * (inflating page_count) and emitted twice (#267). */
			bool dup = false;
			for (size_t k = 0; k < j; k++) {
				if (req_ids[s][k] == req_ids[s][j]) {
					dup = true;
					break;
				}
			}
			if (dup) {
				continue;
			}
			uint8_t sz;
			bool nfc_only;
			if (!dump_field_size(s, req_ids[s][j], &sz, &nfc_only)) {
				continue; /* secret/unknown → not dumpable */
			}
			if (nfc_only && !allow_nfc_only) {
				continue; /* keys: NFC transport only */
			}
			/* Empty alarm slots are omitted by fill_alarms() — skip their
			 * budget/tag here too so page_count matches the emitted response. */
			if (s == DUMP_SECTION_ALARMS &&
			    app_config_alarms_slot_empty(req_ids[s][j])) {
				continue;
			}
			if (used > 0 && used + sz > dump_page_budget(tp)) {
				cur_page++;
				used = 0;
			}
			used += sz;
			if (cur_page == page) {
				out_ids[s][out_n[s]++] = req_ids[s][j];
			}
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

	if (out_n[DUMP_SECTION_LORAWAN] > 0) {
		cd->has_lorawan = true;
		app_config_fill_lorawan(&cd->lorawan, lw, out_n[DUMP_SECTION_LORAWAN]);
	}
	if (out_n[DUMP_SECTION_APPLICATION] > 0) {
		cd->has_application = true;
		app_config_fill_application(&cd->application, ap, out_n[DUMP_SECTION_APPLICATION]);
	}
	if (out_n[DUMP_SECTION_SENSORS] > 0) {
		cd->has_sensors = true;
		app_config_fill_sensors(&cd->sensors, se, out_n[DUMP_SECTION_SENSORS]);
	}
	if (out_n[DUMP_SECTION_ALARMS] > 0) {
		cd->has_alarms = true;
		app_config_fill_alarms(&cd->alarms, al, out_n[DUMP_SECTION_ALARMS]);
	}
}

static void app_cmd_handle_reset_counters(enum app_cmd_transport tp, const Command *cmd,
					  Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	const Command_ResetCounters *rc = &cmd->body.reset_counters;

	app_hall_reset_count(rc->has_hall_left && rc->hall_left,
			     rc->has_hall_right && rc->hall_right);
	app_input_reset_count(rc->has_input_a && rc->input_a, rc->has_input_b && rc->input_b);

	/* Persist the cleared totals so a reboot cannot resurrect them. Deferred to
	 * the post-command action (off this stack frame) because settings_save_one
	 * is too stack-heavy to run inline on the m_work_q. */
	*action = APP_CMD_ACTION_COUNTERS_SAVE;
	resp->which_body = Response_ack_tag;
}

/* #299: rotate secret_key over the already-encrypted channel (the caller already
 * authenticated with the CURRENT key — decrypt() runs before app_cmd_handle() is
 * ever reached). The new key is applied to staging immediately but persisted by
 * the deferred action below (same stack-cost reason as reset_counters above);
 * never echoed back — secret_key stays proto_field:false on every read path. */
static void app_cmd_handle_set_secret_key(enum app_cmd_transport tp, const Command *cmd,
					  Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	const Command_SetSecretKey *ssk = &cmd->body.set_secret_key;

	if (!ssk->has_key) {
		make_error(resp, Response_Error_Code_BAD_REQUEST, "missing key");
		return;
	}

	memcpy(app_config()->secret_key, ssk->key, sizeof(app_config()->secret_key));

	*action = APP_CMD_ACTION_SECRET_KEY_SAVE;
	resp->which_body = Response_ack_tag;
}

/* force_send / req_history are LRW-only (transports: [lrw] in the YAML); the
 * generated dispatch enforces that before calling the handler, so the handlers
 * below assume the LoRaWAN transport. (clock_sync also runs over NFC — see its
 * handler.) */
static void app_cmd_handle_force_send(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				      enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(resp);
	ARG_UNUSED(action);
#if defined(CONFIG_LORAWAN)
	app_report_trigger();
#endif
	/* No ack — the triggered telemetry uplink IS the answer; an extra ack
	 * would just cost a second uplink. Leave which_body == 0 (emit nothing). */
}

/* sample (transports: [lrw, nfc]): take a fresh reading, push it out as
 * telemetry on fPort 2, and — over NFC — return the same readings synchronously
 * so the phone can show them. Over LoRaWAN the fPort-2 uplink is the answer
 * (no fPort-85 body, like force_send). */
static void app_cmd_handle_sample(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				  enum app_cmd_action *action)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);

	/* Fresh reading now, so the NFC response and the telemetry uplink agree. */
	app_sensor_sample();

	if (tp == APP_CMD_TRANSPORT_NFC) {
		resp->which_body = Response_sample_tag;
		app_compose_snapshot(&resp->body.sample);
	}
	/* Over LoRaWAN leave which_body == 0: the fPort-2 frame below is the answer,
	 * and a full Telemetry would not fit the 64-byte fPort-85 response buffer. */

#if defined(CONFIG_LORAWAN)
	app_report_trigger();
#endif
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

/* NFC-only paged history read (#260). Unlike req_history (LRW device-driven
 * streaming), this is client-driven and stateless: each tap returns exactly one
 * HistoryFrame and the phone advances the cursor by passing the response's
 * next_ord back as start_ord on the next request, looping until has_more=false.
 * The device keeps NO replay session (nothing to wedge) and does NOT pause
 * capture — new records may be appended between taps. Edge case: if the flash
 * ring evicts its oldest page between taps, the logical ordinal cursor shifts
 * down; over a short on-site readout (interval is minutes) this is negligible. */
static void app_cmd_handle_req_history_page(enum app_cmd_transport tp, const Command *cmd,
					    Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(action);
#if defined(APP_CMD_HAVE_HISTORY)
	const Command_ReqHistoryPage *rq = &cmd->body.req_history_page;
	uint32_t from = rq->has_from_unix ? rq->from_unix : 0;
	uint32_t to = rq->has_to_unix ? rq->to_unix : UINT32_MAX;
	size_t start = rq->has_start_ord ? rq->start_ord : 0;
	uint32_t present = app_history_get_mask();
	uint32_t interval = app_history_get_interval();

	Response_HistoryFrame *hf = &resp->body.history_frame;

	/* Clamp the sample byte budget to what one HistoryFrame encodes into the NFC
	 * response, further clamped to the samples field capacity (242 B). Worst-case
	 * (max-varint) header values keep the bound stable regardless of the actual
	 * ordinal/time values. */
	size_t cap = app_cmd_history_sample_capacity(cmd->seq, UINT32_MAX, UINT32_MAX, UINT32_MAX,
						     present, interval, DUMP_PAGE_BUDGET_NFC);
	cap = MIN(cap, sizeof(hf->samples.bytes));

	uint32_t t0 = 0;
	uint16_t n_written = 0;
	size_t next_ord = start;
	size_t written = app_history_export_page(from, to, start, hf->samples.bytes, cap, &t0,
						 &n_written, &next_ord);

	resp->which_body = Response_history_frame_tag;
	hf->frame_index = 0; /* informational only over NFC; the phone counts its own pages */
	hf->frame_count = app_history_count_frames(from, to, cap); /* progress hint */
	hf->t0_unix = t0;
	hf->samples.size = written;
	hf->present = present;
	hf->interval_s = interval;
	hf->has_time_synced = true;
	hf->time_synced = app_history_base_synced();
	/* Authoritative cursor for the phone: pass next_ord back as start_ord. When a
	 * page returns no records (buffer empty, cursor past the window/end) or no
	 * ordinals remain, has_more=false stops the tap loop (no wedge, no infinite
	 * loop). A page that fills at the to_unix boundary may cost one extra empty
	 * tap, which then reports has_more=false. */
	hf->has_next_ord = true;
	hf->next_ord = (uint32_t)next_ord;
	hf->has_has_more = true;
	hf->has_more = (n_written > 0) && (next_ord < app_history_count());
#else
	ARG_UNUSED(cmd);
	make_error(resp, Response_Error_Code_HISTORY_UNAVAILABLE, "no history");
#endif
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

/* Sanity bounds for a wall-clock time supplied over NFC: reject obviously wrong
 * epochs. 2024-01-01 .. 2100-01-01 (UTC) comfortably brackets any real device
 * provisioning while staying well inside the uint32 range (rolls over in 2106). */
#define APP_CMD_CLOCK_UNIX_MIN 1704067200UL /* 2024-01-01T00:00:00Z */
#define APP_CMD_CLOCK_UNIX_MAX 4102444800UL /* 2100-01-01T00:00:00Z */

static void app_cmd_handle_clock_sync(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				      enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(action);
#ifdef APP_CMD_HAVE_CLOCK
	/* unix_time set (NFC): the phone supplies the wall-clock time; set the RTC
	 * directly and answer with the resulting Info (carries the new unix_time).
	 * This bootstraps a device before/without a network. */
	if (cmd->body.clock_sync.has_unix_time) {
		uint32_t unix_s = cmd->body.clock_sync.unix_time;
		if (unix_s < APP_CMD_CLOCK_UNIX_MIN || unix_s > APP_CMD_CLOCK_UNIX_MAX) {
			make_error(resp, Response_Error_Code_BAD_REQUEST, "bad epoch");
			return;
		}
		if (app_clock_set_unix(unix_s) != 0) {
			make_error(resp, Response_Error_Code_UNKNOWN, "rtc set failed");
			return;
		}
		resp->which_body = Response_info_tag;
		fill_info(tp, &resp->body.info);
		return;
	}
#ifdef CONFIG_LORAWAN
	/* Empty (LRW): re-sync from the network, then answer with an Info uplink
	 * once the network time lands (carries the synced unix_time). No ack — see
	 * app_lrw. */
	app_clock_force_resync();
	app_lrw_send_info_on_clock_sync();
#else
	resp->which_body = Response_ack_tag; /* no LRW: just confirm */
#endif
#else
	ARG_UNUSED(cmd);
	resp->which_body = Response_ack_tag; /* no clock: just confirm */
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

/* F-1: lrw_join / lrw_reset both re-establish the LoRaWAN session using the
 * running config (g_app_config), which only re-syncs from staging on
 * save+reboot. If SetParam changed LoRaWAN credentials without saving, acting
 * now would silently use the OLD keys while GetParam/GetConfig already echo the
 * NEW ones — a misleading "confirmed" for the installer. Reject loudly so the
 * host saves (which reboots and re-syncs) before (re)joining. These are
 * `kind: handler` in app_config.yml so the guard lives outside the generated
 * dispatch; they otherwise mirror the old `kind: action` behaviour. */
static void lrw_action_guarded(Response *resp, enum app_cmd_action *action,
			       enum app_cmd_action want)
{
	if (m_lrw_staging_dirty) {
		make_error(resp, Response_Error_Code_NOT_READY, "unsaved lrw config; save first");
		return;
	}
	*action = want;
	resp->which_body = Response_ack_tag;
}

static void app_cmd_handle_lrw_join(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				    enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	lrw_action_guarded(resp, action, APP_CMD_ACTION_LRW_JOIN);
}

static void app_cmd_handle_lrw_reset(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				     enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	lrw_action_guarded(resp, action, APP_CMD_ACTION_LRW_RESET);
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
	case Command_device_reset_tag:
		*action = APP_CMD_ACTION_DEVICE_RESET;
		resp->which_body = Response_ack_tag;
		break;
	case Command_force_send_tag:
		/* transports: [lrw] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_force_send(tp, cmd, resp, action);
		break;
	case Command_reset_counters_tag:
		app_cmd_handle_reset_counters(tp, cmd, resp, action);
		break;
	case Command_req_history_tag:
		/* transports: [lrw] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_req_history(tp, cmd, resp, action);
		break;
	case Command_req_history_page_tag:
		/* transports: [nfc] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_NFC) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_req_history_page(tp, cmd, resp, action);
		break;
	case Command_clock_sync_tag:
		/* transports: [lrw, nfc] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_clock_sync(tp, cmd, resp, action);
		break;
	case Command_w1_scan_tag:
		app_cmd_handle_w1_scan(tp, cmd, resp, action);
		break;
	case Command_lrw_reset_tag:
		app_cmd_handle_lrw_reset(tp, cmd, resp, action);
		break;
	case Command_lrw_join_tag:
		app_cmd_handle_lrw_join(tp, cmd, resp, action);
		break;
	case Command_enter_calibration_tag:
		/* transports: [lrw, nfc] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		*action = APP_CMD_ACTION_ENTER_CALIBRATION;
		resp->which_body = Response_ack_tag;
		break;
	case Command_sample_tag:
		/* transports: [lrw, nfc] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_sample(tp, cmd, resp, action);
		break;
	case Command_factory_reset_tag:
		*action = APP_CMD_ACTION_FACTORY_RESET;
		resp->which_body = Response_ack_tag;
		break;
	case Command_set_secret_key_tag:
		/* transports: [nfc, shell] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_NFC && tp != APP_CMD_TRANSPORT_SHELL_DEBUG) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_set_secret_key(tp, cmd, resp, action);
		break;
	default:
		/* L-54: an unknown command tag (e.g. a removed command like the old
		 * enter_dfu/enter_mailbox 19/20/22 sent by an older app) is a distinct,
		 * expected outcome — report NOT_SUPPORTED so the host can tell "command
		 * removed/unknown" apart from a generic UNKNOWN failure. */
		LOG_WRN("Command tag %u not supported", cmd->which_body);
		make_error(resp, Response_Error_Code_NOT_SUPPORTED, "command not supported");
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
	if (ret == -EMSGSIZE) {
		/* The composed response doesn't fit the transport buffer. Don't fail
		 * silently (#93.3) — replace it with a compact Error carrying the same
		 * seq so the host learns the request couldn't be answered (e.g. an
		 * over-broad GetParam/GetConfig page). */
		LOG_WRN("Response too large for buffer; sending Error instead");
		Response err = Response_init_zero;
		err.seq = resp.seq;
		make_error(&err, Response_Error_Code_UNKNOWN, "response too large");
		ret = encode_response(&err, out, out_cap, out_len);
	}
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
	/* Autonomous GetInfo on join goes out over LoRaWAN, so dev_eui is omitted. */
	fill_info(APP_CMD_TRANSPORT_LRW, &resp.body.info);

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
	/* app_cmd_build_history_frame() always sets time_synced, so account for its
	 * bytes here (value 0/1 both encode to 1 byte) or the frame could overflow. */
	hf->has_time_synced = true;
	hf->time_synced = true;

	/* Binary-search the largest samples payload whose fully-encoded frame
	 * (version byte + Response) still fits out_cap, measuring each candidate with
	 * pb_get_encoded_size. Measuring beats hand-accounting the length varints:
	 * BOTH the samples field length AND the enclosing history_frame submessage
	 * length grow with the payload and cross the 1->2 byte varint boundary as it
	 * passes 127 B — reachable since #260 raised the samples field to 440 B. */
	size_t lo = 0, hi = sizeof(hf->samples.bytes);
	while (lo < hi) {
		size_t mid = (lo + hi + 1) / 2;
		size_t sz = 0;
		hf->samples.size = mid;
		if (pb_get_encoded_size(&sz, Response_fields, &resp) &&
		    (size_t)(1 + sz) <= out_cap) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return lo;
}

int app_cmd_build_history_frame(uint32_t seq, uint32_t frame_index, uint32_t frame_count,
				uint32_t t0_unix, uint32_t present, uint32_t interval_s,
				bool time_synced, const uint8_t *samples, size_t samples_len,
				uint8_t *out, size_t out_cap, size_t *out_len)
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
	/* Flag whether t0_unix is absolute (L-1/L-3): host emits time=null otherwise. */
	hf->has_time_synced = true;
	hf->time_synced = time_synced;
	memcpy(hf->samples.bytes, samples, samples_len);
	hf->samples.size = samples_len;

	return encode_response(&resp, out, out_cap, out_len);
}
#endif /* APP_CMD_HAVE_HISTORY */

int app_cmd_build_alarm_report(uint32_t base_time, uint32_t total, bool time_synced,
			       const struct app_cmd_alarm_event *events, size_t n_events,
			       uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!out || !out_len || (n_events > 0 && !events)) {
		return -EINVAL;
	}

	AlarmReport report = AlarmReport_init_zero;
	report.base_time = base_time;
	report.total = total;
	/* Flag whether base_time is absolute (L-3/L-4): host emits time=null otherwise. */
	report.has_time_synced = true;
	report.time_synced = time_synced;

	size_t n = MIN(n_events, ARRAY_SIZE(report.events));
	for (size_t i = 0; i < n; i++) {
		AlarmEvent *ev = &report.events[i];
		ev->slot = events[i].slot;
		ev->source = events[i].source;
		ev->quantity = events[i].quantity;
		ev->edge = (AlarmEvent_Edge)events[i].edge;
		ev->type = (AlarmEvent_Type)events[i].type;
		ev->rel_s = events[i].rel_s;
		ev->has_value = events[i].has_value;
		ev->value = events[i].value;
	}
	report.events_count = (pb_size_t)n;

	/* fPort 3 carries the same 1-byte APP_PROTO_VERSION prefix as fPort 2
	 * (telemetry) and fPort 85 (responses) so every protobuf uplink frame is
	 * uniformly versioned (#165). */
	if (out_cap < 1) {
		return -EMSGSIZE;
	}
	out[0] = APP_PROTO_VERSION;

	pb_ostream_t ostream = pb_ostream_from_buffer(out + 1, out_cap - 1);
	if (!pb_encode(&ostream, AlarmReport_fields, &report)) {
		LOG_ERR_CALL_FAILED_STR("pb_encode", PB_GET_ERROR(&ostream));
		return -EMSGSIZE;
	}

	*out_len = ostream.bytes_written + 1;
	return 0;
}
