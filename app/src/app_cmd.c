/*
 * Copyright (c) 2026 HARDWARIO a.s.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_cmd.h"
#include "app_alarm.h"
#include "app_alarm_rules.h"
#include "app_battery.h"
#include "app_buzzer.h"
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
#include "app_w1_slots.h"
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
	if (!app_nfc_mailbox_available()) {
		status |= APP_DEVICE_STATUS_MAILBOX_DOWN;
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
	/* Radio: OFF means the operator deliberately silenced it; otherwise, in
	 * LoRaWAN mode, flag a link that is not alive (not healthy/warning = idle/
	 * joining/reconnect/disabled). P2P has no LoRaWAN link, so it sets neither. */
	if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_OFF) {
		status |= APP_DEVICE_STATUS_RADIO_OFF;
	} else if (g_app_config.radio_mode == APP_CONFIG_RADIO_MODE_LORAWAN &&
		   info->lrw_state != APP_LRW_STATE_HEALTHY &&
		   info->lrw_state != APP_LRW_STATE_WARNING) {
		status |= APP_DEVICE_STATUS_RADIO_LINK_DOWN;
	}
	if (app_nfc_claim_state_get() == APP_NFC_CLAIM_ACTIVE) {
		status |= APP_DEVICE_STATUS_CLAIM_ACTIVE;
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
	/* Cap set by the caller (fill_info's max_alarms) when the encode didn't fit
	 * the transport's byte budget with every active alarm included — see the
	 * retry loop in app_cmd_build_info()/app_cmd_handle(). SIZE_MAX = no cap. */
	size_t max_alarms = (size_t)(uintptr_t)*arg;

	struct app_alarm_active list[ACTIVE_ALARM_SNAPSHOT_MAX];
	size_t n = app_alarm_active_snapshot(list, ARRAY_SIZE(list));
	if (n > max_alarms) {
		n = max_alarms;
	}

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
 * NFC-only block below. max_alarms caps Response_AlarmStatus entries encoded
 * into active_alarms (field 15) — pass SIZE_MAX for "no cap"; a caller that hit
 * -EMSGSIZE at the current DR budget retries with a smaller value so the alarm
 * list is what gets trimmed, not the rest of Info (see app_cmd_build_info() /
 * app_cmd_handle()). */
static void fill_info(enum app_cmd_transport tp, Response_Info *info, size_t max_alarms)
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
	info->active_alarms.arg = (void *)(uintptr_t)max_alarms;
	if (i.has_unix_time) {
		info->unix_time = i.unix_time;
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

		/* claim_token (#170): emit only once commissioned (any non-zero byte).
		 * The all-zero "unset" state is omitted so an uncommissioned device's
		 * Info stays compact. fixed_length bytes -> plain 16-byte array, no
		 * .size.
		 *
		 * NFC-only: at 18 B on the wire this is the single largest
		 * Info field, and it pushed the response past the EU868 DR0 application
		 * payload budget (56 B vs 50 B). Before #425 an Info could not be paged, so
		 * an over-budget Info was dropped whole — leaving a downlink get_info, and
		 * the device-info-on-join uplink, silently unanswered at DR0. Restricting
		 * the token to NFC keeps the LoRaWAN Info small at every DR (over NFC it
		 * is one more Info page unit, #425/#414).
		 *
		 * Trade-off (deliberate): a backend can no longer learn claim_token over
		 * the air; claiming becomes an NFC-only flow. This reverses the LoRaWAN
		 * half of 636dd5d (#170) — if over-the-air claiming is still required,
		 * page the Info response instead of trimming it. */
		for (size_t j = 0; j < sizeof(i.claim_token); j++) {
			if (i.claim_token[j] != 0) {
				info->has_claim_token = true;
				memcpy(info->claim_token, i.claim_token, sizeof(info->claim_token));
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

/* #425 universal paging: stamp page `index` of `count` on a Response — the one
 * place that writes the envelope. Omitted for a single-frame answer (count <=
 * 1), so an unpaged answer is unchanged. */
static void set_page(Response *resp, uint32_t index, uint32_t count)
{
	if (count > 1) {
		resp->page_index = index;
		resp->page_count = count;
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

	/* Shared with app_config.c's reset ops (#340 M27) — a rollback here
	 * blindly doing `*app_config() = snapshot` must not race a concurrent
	 * device_reset/factory_reset/vendor_reset, or either operation's result
	 * can silently clobber the other's. app_alarm_rules_reload_from_config()
	 * (called within this critical section) takes its own inner lock
	 * (app_alarm_rules.c's m_lock) and never calls back into app_cmd.c or
	 * app_config.c — no deadlock cycle. */
	app_config_lock();

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
			goto out;
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

out:
	app_config_unlock();
}

/* app_cmd_handle_get_param is defined after DUMP_FIELDS (it pages like
 * get_config); see below. */

static void app_cmd_handle_get_info(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				    enum app_cmd_action *action)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);

	resp->which_body = Response_info_tag;
	fill_info(tp, &resp->body.info, SIZE_MAX);
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
	{DUMP_SECTION_LORAWAN, 12, 18, true},    {DUMP_SECTION_LORAWAN, 16, 3, false},
	{DUMP_SECTION_LORAWAN, 13, 3, false},    {DUMP_SECTION_LORAWAN, 14, 3, false},
	{DUMP_SECTION_APPLICATION, 1, 2, false}, {DUMP_SECTION_APPLICATION, 2, 3, false},
	{DUMP_SECTION_APPLICATION, 3, 4, false}, {DUMP_SECTION_APPLICATION, 4, 2, false},
	{DUMP_SECTION_APPLICATION, 5, 6, false}, {DUMP_SECTION_APPLICATION, 6, 3, false},
	{DUMP_SECTION_APPLICATION, 7, 2, false}, {DUMP_SECTION_SENSORS, 1, 2, false},
	{DUMP_SECTION_SENSORS, 2, 2, false},     {DUMP_SECTION_SENSORS, 3, 2, false},
	{DUMP_SECTION_SENSORS, 4, 2, false},     {DUMP_SECTION_SENSORS, 5, 2, false},
	{DUMP_SECTION_SENSORS, 6, 2, false},     {DUMP_SECTION_SENSORS, 7, 2, false},
	{DUMP_SECTION_SENSORS, 19, 3, false},    {DUMP_SECTION_SENSORS, 8, 2, false},
	{DUMP_SECTION_SENSORS, 9, 2, false},     {DUMP_SECTION_SENSORS, 10, 2, false},
	{DUMP_SECTION_SENSORS, 11, 10, false},   {DUMP_SECTION_SENSORS, 12, 10, false},
	{DUMP_SECTION_SENSORS, 13, 10, false},   {DUMP_SECTION_SENSORS, 14, 10, false},
	{DUMP_SECTION_SENSORS, 15, 2, false},    {DUMP_SECTION_SENSORS, 16, 3, false},
	{DUMP_SECTION_SENSORS, 17, 3, false},    {DUMP_SECTION_SENSORS, 18, 3, false},
	{DUMP_SECTION_ALARMS, 1, 3, false},      {DUMP_SECTION_ALARMS, 3, 19, false},
	{DUMP_SECTION_ALARMS, 4, 19, false},     {DUMP_SECTION_ALARMS, 5, 19, false},
	{DUMP_SECTION_ALARMS, 6, 19, false},     {DUMP_SECTION_ALARMS, 7, 19, false},
	{DUMP_SECTION_ALARMS, 8, 19, false},     {DUMP_SECTION_ALARMS, 9, 19, false},
	{DUMP_SECTION_ALARMS, 10, 19, false},    {DUMP_SECTION_ALARMS, 11, 19, false},
	{DUMP_SECTION_ALARMS, 12, 19, false},    {DUMP_SECTION_ALARMS, 13, 19, false},
	{DUMP_SECTION_ALARMS, 14, 19, false},    {DUMP_SECTION_ALARMS, 15, 19, false},
	{DUMP_SECTION_ALARMS, 16, 20, false},    {DUMP_SECTION_ALARMS, 17, 20, false},
	{DUMP_SECTION_ALARMS, 18, 20, false},    {DUMP_SECTION_ALARMS, 20, 3, false},
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

/* Over NFC the response travels in the ST25DV Fast-Transfer-Mode mailbox, a
 * 256 B frame: 1 B channel prefix + 8 B header (serial, nonce) + ciphertext +
 * 16 B CCM tag -> at most 231 B of plaintext, i.e. version byte + Response.
 * A ConfigDump page therefore gets roughly 231 - 1 (version) - ~14 B
 * (Response/ConfigDump wrappers, page_index, page_count) ~= 216 B of field
 * payload. The budget is in DUMP_FIELDS.size units, which over-estimate native
 * byte fields ~2x (so real bytes <= budget), hence 200 keeps every page inside
 * one mailbox frame with margin; a whole config snapshot is ~3-4 pages read in
 * one RF session (#313). LoRaWAN keeps the small DR0 budget. */
#define DUMP_PAGE_BUDGET_NFC 200

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
	set_page(resp, page, page_count);

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
	set_page(resp, page, page_count);

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

static bool buffer_is_zero(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i]) {
			return false;
		}
	}

	return true;
}

/* #299: rotate secret_key over the already-encrypted channel (the caller already
 * authenticated with the CURRENT key — decrypt() runs before app_cmd_handle() is
 * ever reached). The new key is applied to staging immediately but persisted by
 * the deferred action below (same stack-cost reason as reset_counters above);
 * never echoed back — secret_key stays proto_field:false on every read path.
 * #322: that deferred action also reboots, which is what actually makes the new
 * key live — the NFC channel authenticates from g_app_config (the boot-time
 * copy), so a persist alone would leave the device answering to the OLD key
 * until some later, unrelated reboot. */
static void app_cmd_handle_set_secret_key(enum app_cmd_transport tp, const Command *cmd,
					  Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	const Command_SetSecretKey *ssk = &cmd->body.set_secret_key;

	if (!ssk->has_key) {
		make_error(resp, Response_Error_Code_BAD_REQUEST, "missing key");
		return;
	}

	/* #322: an all-zero key is the "unprovisioned" sentinel that makes
	 * key_is_provisioned() (app_nfc.c) refuse the encrypted channel outright —
	 * accepting one here would lock the caller out of the very channel it just
	 * used, with no way back short of the vendor_token channel or a J-Link. Same
	 * bar app_settings_vendor_reset() already enforces on its replacement key. */
	if (buffer_is_zero(ssk->key, sizeof(ssk->key))) {
		make_error(resp, Response_Error_Code_BAD_REQUEST, "zero key");
		return;
	}

	memcpy(app_config()->secret_key, ssk->key, sizeof(app_config()->secret_key));

	*action = APP_CMD_ACTION_SECRET_KEY_SAVE;
	resp->which_body = Response_ack_tag;
}

/* #308/#415: explicit end of the claim window. app_nfc_claim_done() does the
 * same narrow settings_save_one() persist app_nfc.c already does synchronously —
 * no deferred action, no reboot. Idempotent (a no-op if already done), so it is
 * always safe to send. With #415 this is the ONLY way the window closes (the
 * implicit close on any decrypted command is gone), so the app must send it
 * after storing the claimed keys. */
static void app_cmd_handle_claim_done(enum app_cmd_transport tp, const Command *cmd, Response *resp,
				      enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);

	app_nfc_claim_done();
	resp->which_body = Response_ack_tag;
}

/* #351/#415: re-open the claim window (symmetric counterpart to claim_done's
 * close). Always deferred via APP_CMD_ACTION_CLAIM_ACTIVE_SAVE — same
 * restart-style pattern as reboot/device_reset/set_secret_key: the Ack is
 * written and delivered to the phone first (app_nfc_take_cmd_action() only
 * releases the action once that round-trip completes, #242), and only then does
 * main.c flip the latch + reboot. This used to short-circuit to a synchronous
 * app_nfc_claim_active() (no reboot) when no new_claim_token was given, on the
 * reasoning that nothing in g_app_config was changing so there was nothing to
 * wait on — but that made the two branches behave differently for no
 * functional reason. Deferring both the same way costs one reboot in the
 * no-new-token case and buys consistent, predictable timing instead: the
 * phone can always assume "ack read -> reboot happens" regardless of which
 * branch it took, mirroring the non-new-token branch's rebuild of
 * app_config()->claim_token being a same-value no-op (h_commit just re-syncs
 * the value that's already live), so a plain re-open still leaves
 * claim_token unchanged. */
static void app_cmd_handle_claim_active(enum app_cmd_transport tp, const Command *cmd,
					Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	const Command_ClaimActive *rearm = &cmd->body.claim_active;

	if (rearm->has_new_claim_token &&
	    !buffer_is_zero(rearm->new_claim_token, sizeof(rearm->new_claim_token))) {
		memcpy(app_config()->claim_token, rearm->new_claim_token,
		       sizeof(app_config()->claim_token));
	}
	*action = APP_CMD_ACTION_CLAIM_ACTIVE_SAVE;

	resp->which_body = Response_ack_tag;
}

/* #415: read the claim identity {serial_number, claim_token} over the
 * unauthenticated plain_text transport (also nfc / shell). Returns
 * Response.claim_info while the claim window is active; Error NOT_READY
 * "claimed" once it is done, or "no claim token" when none was provisioned.
 * Read-only, no side effects — the identity-class-only rule a plain_text command
 * must obey. Discloses exactly what the plaintext hio.stck:clm record does
 * today, but only on a powered unit while the window is open. */
static void app_cmd_handle_get_claim_info(enum app_cmd_transport tp, const Command *cmd,
					  Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);

	if (app_nfc_claim_state_get() == APP_NFC_CLAIM_DONE) {
		make_error(resp, Response_Error_Code_NOT_READY, "claimed");
		return;
	}
	if (buffer_is_zero(g_app_config.claim_token, sizeof(g_app_config.claim_token))) {
		make_error(resp, Response_Error_Code_NOT_READY, "no claim token");
		return;
	}

	resp->which_body = Response_claim_info_tag;
	resp->body.claim_info.serial_number = g_app_config.serial_number;
	BUILD_ASSERT(sizeof(resp->body.claim_info.claim_token) == sizeof(g_app_config.claim_token),
		     "ClaimInfo.claim_token size mismatch");
	memcpy(resp->body.claim_info.claim_token, g_app_config.claim_token,
	       sizeof(g_app_config.claim_token));
}

/* #415/#313: identity bootstrap over the mailbox. Everything a phone needs after
 * a tap, before its first encrypted command, and none of it secret: the serial
 * (to pick the cached secret_key), the nonce high-water (to send nonce+1 through
 * the anti-replay window), the config version (is its cached config stale) and
 * the FW version. Replaces the plaintext hio.stck:inf record. Read-only. Always
 * answers (unlike get_claim_info it is not gated on the claim window). Identity
 * only — no device_status: alarm / battery / radio / claim state stay owner-only
 * (Info.device_status over the encrypted get_info), so an unauthenticated tap
 * cannot tell e.g. that a security sensor's radio is off. */
static void app_cmd_handle_get_basic_info(enum app_cmd_transport tp, const Command *cmd,
					  Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);

	resp->which_body = Response_basic_info_tag;
	Response_BasicInfo *bi = &resp->body.basic_info;
	bi->serial_number = g_app_config.serial_number;
	/* Live high-water == what decrypt() checks against (what the old inf record
	 * carried, now served here). */
	bi->nonce_counter = app_config()->nonce_counter;
	bi->config_version = g_app_config.config_version;
	bi->fw_major = APP_VERSION_MAJOR;
	bi->fw_minor = APP_VERSION_MINOR;
	bi->fw_patch = APP_VERSION_PATCH;
}

/* #338: remote-triggered buzzer melody (NFC/LRW). kind selects one of the
 * fixed severity melodies (the buzzer is DC self-oscillating only — no custom
 * tones); kind 0 (or any id >= 16) is the STOP request — the remote
 * counterpart of `ats buzzer off`, silencing a beep/melody mid-playback and
 * cancelling a pending repeat cycle. repeat_s (0-999s), if non-zero, keeps
 * re-playing the melody that many seconds after each playback finishes, until
 * stopped. Fire-and-forget: the Ack confirms the request was accepted, not
 * that it audibly played (a newer request supersedes an older queued one). */
static void app_cmd_handle_buzzer_play(enum app_cmd_transport tp, const Command *cmd,
				       Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);
#if defined(CONFIG_APP_BUZZER)
	const Command_BuzzerPlay *play = &cmd->body.buzzer_play;

	if (play->repeat_s > 999) {
		make_error(resp, Response_Error_Code_BAD_REQUEST, "repeat_s out of range");
		return;
	}

	/* kind is a raw id (#338) — this dispatch layer never validates or
	 * interprets it; app_buzzer_play_repeating() (app_buzzer.c MELODY_TABLE)
	 * is the sole authority on which ids exist (0 / >=16 = stop, 1-3 =
	 * melodies, 4-15 reserved), so a new melody never needs a change here. */
	int ret = app_buzzer_play_repeating(play->kind, (uint16_t)play->repeat_s);
	if (ret == -ENOENT) {
		make_error(resp, Response_Error_Code_BAD_REQUEST, "unknown melody kind");
		return;
	}
	if (ret) {
		make_error(resp, Response_Error_Code_NOT_READY, "buzzer not active");
		return;
	}

	resp->which_body = Response_ack_tag;
#else
	make_error(resp, Response_Error_Code_NOT_SUPPORTED, "buzzer not built into this FW");
#endif /* defined(CONFIG_APP_BUZZER) */
}

/* #316: replacement secret_key staged by the vendor_reset handler below, consumed
 * once via app_cmd_take_pending_vendor_secret_key() when the deferred
 * APP_CMD_ACTION_VENDOR_RESET runs (main.c / nfc-check apply sites). Lives here
 * (not app_nfc.c) because vendor_reset is now a generic Command: the handler runs
 * on dispatch and stashes the key synchronously; the action carrier conveys only
 * the enum. Single writer (the handler) + copied out immediately by the consumer,
 * and the NFC action-staging (M-5) rejects a second pending action, so the buffer
 * stays valid until taken. */
static uint8_t m_pending_vendor_secret_key[16];

/* #316: vendor_reset over the vendor channel (NFC hio.stck:vnd, authenticated by
 * vendor_token — decrypt() runs before app_cmd_handle() is reached). Body reuses
 * the SetSecretKey shape: the mandatory replacement secret_key, because
 * app_settings_vendor_reset() zeroes the old key (secret_key is NOT in the
 * vendor_reset persistent tier). The destructive wipe + key install + cold reboot
 * is the deferred APP_CMD_ACTION_VENDOR_RESET, run after the Ack is on the tag.
 *
 * Gate (Option A, #316): refuse when vendor_reset_allow is false — the same policy
 * app_settings_vendor_reset() enforces, pre-checked here so the phone gets an
 * immediate NOT_READY instead of an Ack followed by a silent no-op. Recovery from
 * allow==false is a vendor-channel set_param(vendor_reset_allow=true) first (its
 * write is gated only by transport, never by the flag's current value), then this. */
static void app_cmd_handle_vendor_reset(enum app_cmd_transport tp, const Command *cmd,
					Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	const Command_SetSecretKey *vr = &cmd->body.vendor_reset;

	if (!vr->has_key) {
		make_error(resp, Response_Error_Code_BAD_REQUEST, "missing key");
		return;
	}

	/* #385: same bar as app_cmd_handle_set_secret_key() above — reject a
	 * zero key synchronously instead of Ack'ing and letting
	 * app_settings_vendor_reset()'s own key_is_set() check silently no-op the
	 * deferred action later (the caller would see a success Ack for a reset
	 * that never actually happened). */
	if (buffer_is_zero(vr->key, sizeof(vr->key))) {
		make_error(resp, Response_Error_Code_BAD_REQUEST, "zero key");
		return;
	}

	if (!app_config()->vendor_reset_allow) {
		make_error(resp, Response_Error_Code_NOT_READY, "vendor_reset disabled");
		return;
	}

	memcpy(m_pending_vendor_secret_key, vr->key, sizeof(m_pending_vendor_secret_key));

	*action = APP_CMD_ACTION_VENDOR_RESET;
	resp->which_body = Response_ack_tag;
}

const uint8_t *app_cmd_take_pending_vendor_secret_key(void)
{
	return m_pending_vendor_secret_key;
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
	 * uplink. Only when nothing replays do we send an Error so the host still
	 * gets a definitive answer: BUDGET_TOO_SMALL when records exist but not one
	 * fits the current DR (#409 3f, retry at a higher DR), else
	 * HISTORY_UNAVAILABLE. */
	int ret = app_lrw_history_replay_start(from, to, cmd->seq);
	if (ret == -EMSGSIZE) {
		make_error(resp, Response_Error_Code_BUDGET_TOO_SMALL, NULL);
	} else if (ret != 0) {
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
	/* NFC history is cursor-paged (next_ord / has_more below): the phone drives
	 * it by record ordinal, so no page_index/page_count here (#425). */
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
		fill_info(tp, &resp->body.info, SIZE_MAX);
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

/* #395: with CONFIG_APP_CALIBRATION=n the flag's only consumers (main.c's
 * detect_mode() and app_calibration_init(), which also clears it) are compiled
 * out — ACKing here would persist calibration=true into NVS where it lurks
 * until some later full-featured FW boots straight into calibration mode.
 * Reject up front instead. */
static void app_cmd_handle_enter_calibration(enum app_cmd_transport tp, const Command *cmd,
					     Response *resp, enum app_cmd_action *action)
{
	ARG_UNUSED(tp);
	ARG_UNUSED(cmd);
	ARG_UNUSED(action);
#if defined(CONFIG_APP_CALIBRATION)
	*action = APP_CMD_ACTION_ENTER_CALIBRATION;
	resp->which_body = Response_ack_tag;
#else
	make_error(resp, Response_Error_Code_NOT_SUPPORTED, "calibration not built into this FW");
#endif /* defined(CONFIG_APP_CALIBRATION) */
}

// BEGIN GENERATED DISPATCH
static void app_cmd_dispatch(enum app_cmd_transport tp, const Command *cmd, Response *resp,
			     enum app_cmd_action *action)
{
	resp->seq = cmd->seq;

	switch (cmd->which_body) {
	case Command_set_param_tag:
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_set_param(tp, cmd, resp, action);
		break;
	case Command_get_param_tag:
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_get_param(tp, cmd, resp, action);
		break;
	case Command_get_info_tag:
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_get_info(tp, cmd, resp, action);
		break;
	case Command_get_config_tag:
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_get_config(tp, cmd, resp, action);
		break;
	case Command_settings_save_tag:
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		*action = APP_CMD_ACTION_SETTINGS_SAVE;
		resp->which_body = Response_ack_tag;
		break;
	case Command_reboot_tag:
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		*action = APP_CMD_ACTION_REBOOT;
		resp->which_body = Response_ack_tag;
		break;
	case Command_device_reset_tag:
		/* transports: [nfc, shell] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_NFC && tp != APP_CMD_TRANSPORT_SHELL_DEBUG) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
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
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
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
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_w1_scan(tp, cmd, resp, action);
		break;
	case Command_lrw_reset_tag:
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_lrw_reset(tp, cmd, resp, action);
		break;
	case Command_lrw_join_tag:
		/* transports: [lrw, nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG && tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_lrw_join(tp, cmd, resp, action);
		break;
	case Command_enter_calibration_tag:
		/* transports: [lrw, nfc] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_enter_calibration(tp, cmd, resp, action);
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
		/* transports: [nfc, shell] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_NFC && tp != APP_CMD_TRANSPORT_SHELL_DEBUG) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		*action = APP_CMD_ACTION_FACTORY_RESET;
		resp->which_body = Response_ack_tag;
		break;
	case Command_set_secret_key_tag:
		/* transports: [nfc, shell, vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_NFC && tp != APP_CMD_TRANSPORT_SHELL_DEBUG &&
		    tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_set_secret_key(tp, cmd, resp, action);
		break;
	case Command_claim_done_tag:
		/* transports: [nfc, shell] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_NFC && tp != APP_CMD_TRANSPORT_SHELL_DEBUG) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_claim_done(tp, cmd, resp, action);
		break;
	case Command_vendor_reset_tag:
		/* transports: [vendor] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_VENDOR) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_vendor_reset(tp, cmd, resp, action);
		break;
	case Command_claim_active_tag:
		/* transports: [nfc, shell] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_NFC && tp != APP_CMD_TRANSPORT_SHELL_DEBUG) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_claim_active(tp, cmd, resp, action);
		break;
	case Command_buzzer_play_tag:
		/* transports: [lrw, nfc] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_LRW && tp != APP_CMD_TRANSPORT_NFC) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_buzzer_play(tp, cmd, resp, action);
		break;
	case Command_get_claim_info_tag:
		/* transports: [plain_text, nfc, shell] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_PLAIN_TEXT && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_get_claim_info(tp, cmd, resp, action);
		break;
	case Command_get_basic_info_tag:
		/* transports: [plain_text, nfc, shell] — reject on any other transport */
		if (tp != APP_CMD_TRANSPORT_PLAIN_TEXT && tp != APP_CMD_TRANSPORT_NFC &&
		    tp != APP_CMD_TRANSPORT_SHELL_DEBUG) {
			make_error(resp, Response_Error_Code_NOT_READY, "transport not allowed");
			break;
		}
		app_cmd_handle_get_basic_info(tp, cmd, resp, action);
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

/* Whether `resp` encodes (version byte + Response) into `cap` bytes — a nanopb
 * sizing pass, so a page layout needs no scratch buffer of the frame's size. */
static bool response_fits(const Response *resp, size_t cap)
{
	size_t size = 0;

	return pb_get_encoded_size(&size, Response_fields, resp) && size + 1 <= cap;
}

/* #425 universal paging over a radio. An answer that does not fit the DR budget
 * is split into pages — each a complete Response with the same seq and
 * Response.page_index/page_count — and the device sends every page by itself.
 * One stream at a time; a new paged answer replaces a running one. Only touched
 * from the LoRaWAN command path (m_work_q).
 *   REQUEST  - GetConfig/GetParam: the raw request is re-dispatched with
 *              page = next, so every page comes from the same handler/layout.
 *   SETTINGS - the autonomous settings-info: re-laid out for the budget captured
 *              at the start (the content is static config).
 *   W1SCAN   - the scan result is kept, so later pages do not rescan the bus.
 *   INFO     - a snapshot of Info (scalars + active alarms), so all pages
 *              describe the same moment. */
#define PAGE_STREAM_REQ_MAX 64
/* Upper bound used for page_index/page_count while laying pages out: keeps both
 * varints at one byte, so the real values never make a page grow. */
#define PAGE_COUNT_BOUND    127

enum page_stream_kind {
	PAGE_STREAM_NONE = 0,
	PAGE_STREAM_REQUEST,
	PAGE_STREAM_SETTINGS,
	PAGE_STREAM_W1SCAN,
	PAGE_STREAM_INFO,
};

/* Info snapshot for paging: the LoRaWAN view of the scalars plus the active
 * alarms as (source, quantity, type) triples. */
struct info_snap {
	Response_Info info;
	uint8_t alarm[ACTIVE_ALARM_SNAPSHOT_MAX][3];
	uint8_t n_alarms;
	uint32_t seq; /* echoed on every page; part of the page size */
};

static struct {
	enum page_stream_kind kind;
	uint32_t next;
	uint32_t count;
	uint32_t seq;
	size_t cap; /* budget the layout was made for */
	union {
		struct {
			uint8_t buf[PAGE_STREAM_REQ_MAX];
			size_t len;
		} req;
		struct {
			Response_W1Scan scan;
			uint8_t per_page;
		} w1;
		struct info_snap info;
	} u;
} m_page_stream;

void app_cmd_stream_cancel(void)
{
	m_page_stream.kind = PAGE_STREAM_NONE;
}

bool app_cmd_stream_active(void)
{
	return m_page_stream.kind != PAGE_STREAM_NONE;
}

static void page_stream_start(enum page_stream_kind kind, uint32_t seq, size_t cap, uint32_t count)
{
	m_page_stream.kind = kind;
	m_page_stream.seq = seq;
	m_page_stream.cap = cap;
	m_page_stream.next = 1;
	m_page_stream.count = count;
}

/* After a LoRaWAN GetConfig/GetParam: arm the stream when pages remain. */
static bool page_stream_arm(const uint8_t *in, size_t in_len, const Response *resp)
{
	if (resp->which_body != Response_config_dump_tag ||
	    in_len > sizeof(m_page_stream.u.req.buf) || resp->page_index + 1 >= resp->page_count) {
		return false;
	}
	memcpy(m_page_stream.u.req.buf, in, in_len);
	m_page_stream.u.req.len = in_len;
	page_stream_start(PAGE_STREAM_REQUEST, resp->seq, 0, resp->page_count);
	m_page_stream.next = resp->page_index + 1;
	return true;
}

/* ---- settings-info pages (#412 content) --------------------------------- */

/* application: interval_sample, interval_report, history_enable */
static const uint32_t cs_app_ids[] = {2, 3, 4};
/* sensors: cap_hall_left..cap_accelerometer (all nine cap_* flags) */
static const uint32_t cs_sensor_ids[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
#if defined(APP_CMD_HAVE_W1)
#define CS_ITEMS (ARRAY_SIZE(cs_app_ids) + ARRAY_SIZE(cs_sensor_ids) + 1)
#else
#define CS_ITEMS (ARRAY_SIZE(cs_app_ids) + ARRAY_SIZE(cs_sensor_ids))
#endif

/* Fill a settings-info ConfigDump with the items in `mask` (bit i = item i:
 * application ids, then sensor ids, then the w1_slot_type block). */
static void config_status_fill(Response *resp, uint32_t mask, uint32_t page, uint32_t count)
{
	*resp = (Response)Response_init_zero;
	resp->seq = 0;
	resp->which_body = Response_config_dump_tag;
	set_page(resp, page, count);

	Response_ConfigDump *cd = &resp->body.config_dump;
	uint32_t ids[ARRAY_SIZE(cs_sensor_ids)];
	size_t n = 0;

	for (size_t i = 0; i < ARRAY_SIZE(cs_app_ids); i++) {
		if (mask & BIT(i)) {
			ids[n++] = cs_app_ids[i];
		}
	}
	if (n > 0) {
		cd->has_application = true;
		app_config_fill_application(&cd->application, ids, n);
	}

	n = 0;
	for (size_t i = 0; i < ARRAY_SIZE(cs_sensor_ids); i++) {
		if (mask & BIT(ARRAY_SIZE(cs_app_ids) + i)) {
			ids[n++] = cs_sensor_ids[i];
		}
	}
	if (n > 0) {
		cd->has_sensors = true;
		app_config_fill_sensors(&cd->sensors, ids, n);
	}

#if defined(APP_CMD_HAVE_W1)
	if (mask & BIT(CS_ITEMS - 1)) {
		/* Detected 1-Wire slot type per slot (runtime state; see app_w1_slot_type
		 * in app_w1_slots.h — the single source of truth). Wire values are pinned
		 * here so reordering the enum can never silently change the on-air
		 * meaning. */
		BUILD_ASSERT(APP_W1_SLOT_EMPTY == 0 && APP_W1_SLOT_DALLAS == 1 &&
				     APP_W1_SLOT_MACHINE_PROBE == 2,
			     "w1_slot_type wire values must stay 0/empty 1/dallas 2/machine-probe");
		BUILD_ASSERT(APP_W1_SLOT_COUNT <= ARRAY_SIZE(cd->w1_slot_type),
			     "w1_slot_type array too small for APP_W1_SLOT_COUNT");
		cd->w1_slot_type_count = APP_W1_SLOT_COUNT;
		for (int s = 0; s < APP_W1_SLOT_COUNT; s++) {
			cd->w1_slot_type[s] = (uint32_t)app_w1_slot_get_type(s);
		}
	}
#endif
}

/* Greedy layout: pack items into as few pages as fit `cap` (measured by really
 * encoding each candidate page). An item that does not fit even alone is left
 * out (physical floor, #425); -EMSGSIZE only when no item fits at all. On
 * success mask[p] holds the items of page p and *count the pages. */
static int config_status_layout(size_t cap, uint32_t mask[CS_ITEMS], uint32_t *count)
{
	uint8_t tmp[128];
	size_t len;
	Response r;
	uint32_t cur = 0;

	cap = MIN(cap, sizeof(tmp));
	memset(mask, 0, sizeof(uint32_t) * CS_ITEMS);

	for (size_t i = 0; i < CS_ITEMS; i++) {
		config_status_fill(&r, mask[cur] | BIT(i), PAGE_COUNT_BOUND, PAGE_COUNT_BOUND);
		if (encode_response(&r, tmp, cap, &len) == 0) {
			mask[cur] |= BIT(i);
			continue;
		}
		config_status_fill(&r, BIT(i), PAGE_COUNT_BOUND, PAGE_COUNT_BOUND);
		if (encode_response(&r, tmp, cap, &len) != 0) {
			continue; /* too big even alone: left out */
		}
		cur++;
		mask[cur] = BIT(i);
	}
	if (mask[cur] == 0) {
		return -EMSGSIZE; /* nothing fits */
	}
	*count = cur + 1;
	return 0;
}

static int config_status_page(size_t layout_cap, uint32_t page, uint8_t *out, size_t out_cap,
			      size_t *out_len, uint32_t *count)
{
	uint32_t mask[CS_ITEMS];
	Response r;
	int ret = config_status_layout(layout_cap, mask, count);

	if (ret) {
		return ret;
	}
	if (page >= *count) {
		return -ENODATA;
	}
	config_status_fill(&r, mask[page], page, *count);
	return encode_response(&r, out, out_cap, out_len);
}

/* ---- W1Scan pages -------------------------------------------------------- */

/* Split a LoRaWAN W1Scan answer that does not fit `cap`: keep the largest
 * number of ROMs per page that fits, trim `resp` to page 0 and keep the full
 * result for app_cmd_stream_next(). Returns true when a stream was armed. If
 * not even one ROM fits, `resp` is left alone (the caller's too-large fallback
 * answers BUDGET_TOO_SMALL). */
static bool w1_scan_arm_pages(Response *resp, size_t cap)
{
	uint8_t tmp[128];
	size_t len;
	pb_size_t n = resp->body.w1_scan.rom_count;
	pb_size_t k;

	cap = MIN(cap, sizeof(tmp));
	for (k = n; k > 0; k--) {
		Response r = *resp;

		r.body.w1_scan.rom_count = k;
		if (k < n) {
			set_page(&r, PAGE_COUNT_BOUND, PAGE_COUNT_BOUND);
		}
		if (encode_response(&r, tmp, cap, &len) == 0) {
			break;
		}
	}
	if (k == n || k == 0) {
		return false; /* fits whole, or nothing fits */
	}

	m_page_stream.u.w1.scan = resp->body.w1_scan;
	m_page_stream.u.w1.per_page = (uint8_t)k;
	page_stream_start(PAGE_STREAM_W1SCAN, resp->seq, cap, (n + k - 1) / k);

	resp->body.w1_scan.rom_count = k;
	set_page(resp, 0, m_page_stream.count);
	return true;
}

static int w1_scan_page(uint32_t page, uint8_t *out, size_t out_cap, size_t *out_len)
{
	Response r = Response_init_zero;
	const Response_W1Scan *all = &m_page_stream.u.w1.scan;
	size_t first = (size_t)page * m_page_stream.u.w1.per_page;

	r.seq = m_page_stream.seq;
	r.which_body = Response_w1_scan_tag;
	set_page(&r, page, m_page_stream.count);
	for (size_t i = first; i < all->rom_count && i < first + m_page_stream.u.w1.per_page; i++) {
		r.body.w1_scan.rom[r.body.w1_scan.rom_count++] = all->rom[i];
	}
	return encode_response(&r, out, out_cap, out_len);
}

#ifdef CONFIG_ZTEST
/* Test hook: the W1Scan split without a 1-Wire bus. Builds the answer for `n`
 * ROMs, applies the LoRaWAN split for `cap` and encodes page 0. */
int test_w1_scan_page0(const uint8_t roms[][8], size_t n, uint32_t seq, uint8_t *out, size_t cap,
		       size_t *out_len, bool *streamed)
{
	Response r = Response_init_zero;

	app_cmd_stream_cancel();
	r.seq = seq;
	r.which_body = Response_w1_scan_tag;
	for (size_t i = 0; i < n && i < ARRAY_SIZE(r.body.w1_scan.rom); i++) {
		r.body.w1_scan.rom[i].size = 8;
		memcpy(r.body.w1_scan.rom[i].bytes, roms[i], 8);
		r.body.w1_scan.rom_count++;
	}
	*streamed = w1_scan_arm_pages(&r, cap);
	return encode_response(&r, out, cap, out_len);
}
#endif

/* ---- Info pages ---------------------------------------------------------- */

/* Info units, in page order: the scalar fields one by one, then each active
 * alarm. Splitting per field is what lets page 0 carry the firmware version
 * even at the 11 B budget tier. */
enum {
	INFO_U_FW_MAJOR,
	INFO_U_FW_MINOR,
	INFO_U_FW_PATCH,
	INFO_U_BUILD_TYPE,
	INFO_U_DEBUG,
	INFO_U_SERIAL,
	INFO_U_UPTIME,
	INFO_U_UNIX_TIME,
	INFO_U_BATTERY,
	INFO_U_RESET_CAUSE,
	INFO_U_DEVICE_STATUS,
	/* NFC-only fields: never set in a LoRaWAN snapshot, so empty (skipped) there. */
	INFO_U_LRW_STATE,
	INFO_U_CLAIM_TOKEN,
	INFO_U_DEV_EUI,
	INFO_U_SCALARS,
};

struct alarm_range {
	const struct info_snap *snap;
	uint8_t start;
	uint8_t end;
};

static bool encode_alarm_range(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
	const struct alarm_range *r = *arg;

	for (uint8_t i = r->start; i < r->end; i++) {
		Response_AlarmStatus e = Response_AlarmStatus_init_zero;

		e.source = r->snap->alarm[i][0];
		e.quantity = r->snap->alarm[i][1];
		e.type = r->snap->alarm[i][2];
		if (!pb_encode_tag_for_field(stream, field) ||
		    !pb_encode_submessage(stream, Response_AlarmStatus_fields, &e)) {
			return false;
		}
	}
	return true;
}

/* Build an Info page with the scalar units in `mask` and alarms [a0, a1). */
static void info_page_fill(Response *resp, const struct info_snap *snap, uint32_t mask,
			   struct alarm_range *rng, uint32_t page, uint32_t count)
{
	const Response_Info *all = &snap->info;
	Response_Info *pi = &resp->body.info;

	*resp = (Response)Response_init_zero;
	resp->seq = snap->seq;
	resp->which_body = Response_info_tag;
	set_page(resp, page, count);

	pi->fw_major = (mask & BIT(INFO_U_FW_MAJOR)) ? all->fw_major : 0;
	pi->fw_minor = (mask & BIT(INFO_U_FW_MINOR)) ? all->fw_minor : 0;
	pi->fw_patch = (mask & BIT(INFO_U_FW_PATCH)) ? all->fw_patch : 0;
	pi->build_type = (mask & BIT(INFO_U_BUILD_TYPE)) ? all->build_type : 0;
	pi->debug = (mask & BIT(INFO_U_DEBUG)) ? all->debug : false;
	pi->serial_number = (mask & BIT(INFO_U_SERIAL)) ? all->serial_number : 0;
	pi->uptime_s = (mask & BIT(INFO_U_UPTIME)) ? all->uptime_s : 0;
	pi->unix_time = (mask & BIT(INFO_U_UNIX_TIME)) ? all->unix_time : 0;
	pi->battery = (mask & BIT(INFO_U_BATTERY)) ? all->battery : 0;
	pi->reset_cause = (mask & BIT(INFO_U_RESET_CAUSE)) ? all->reset_cause : 0;
	pi->device_status = (mask & BIT(INFO_U_DEVICE_STATUS)) ? all->device_status : 0;
	if (mask & BIT(INFO_U_LRW_STATE)) {
		pi->has_lrw_state = all->has_lrw_state;
		pi->lrw_state = all->lrw_state;
	}
	if (mask & BIT(INFO_U_CLAIM_TOKEN)) {
		pi->has_claim_token = all->has_claim_token;
		memcpy(pi->claim_token, all->claim_token, sizeof(pi->claim_token));
	}
	if (mask & BIT(INFO_U_DEV_EUI)) {
		pi->has_dev_eui = all->has_dev_eui;
		memcpy(pi->dev_eui, all->dev_eui, sizeof(pi->dev_eui));
	}
	if (rng->end > rng->start) {
		pi->active_alarms.funcs.encode = encode_alarm_range;
		pi->active_alarms.arg = rng;
	}
}

/* A scalar unit whose value is 0 is omitted on the wire anyway; skipping it keeps
 * a page from carrying nothing. */
static bool info_unit_empty(const Response_Info *in, size_t u)
{
	switch (u) {
	case INFO_U_FW_MAJOR:
		return in->fw_major == 0;
	case INFO_U_FW_MINOR:
		return in->fw_minor == 0;
	case INFO_U_FW_PATCH:
		return in->fw_patch == 0;
	case INFO_U_BUILD_TYPE:
		return in->build_type == 0;
	case INFO_U_DEBUG:
		return !in->debug;
	case INFO_U_SERIAL:
		return in->serial_number == 0;
	case INFO_U_UPTIME:
		return in->uptime_s == 0;
	case INFO_U_UNIX_TIME:
		return in->unix_time == 0;
	case INFO_U_BATTERY:
		return in->battery == 0;
	case INFO_U_RESET_CAUSE:
		return in->reset_cause == 0;
	case INFO_U_DEVICE_STATUS:
		return in->device_status == 0;
	case INFO_U_LRW_STATE:
		return !in->has_lrw_state;
	case INFO_U_CLAIM_TOKEN:
		return !in->has_claim_token;
	case INFO_U_DEV_EUI:
		return !in->has_dev_eui;
	default:
		return false;
	}
}

/* Greedy layout of the Info units for `cap`; fills in the composition of page
 * `want` (if it exists) and the page count. A unit that does not fit even alone
 * (at the 11 B tier: serial, unix time, an alarm entry) is left out — physical
 * floor, #425; -EMSGSIZE only when no unit fits at all. */
static int info_layout(const struct info_snap *snap, size_t cap, uint32_t want, uint32_t *mask_out,
		       struct alarm_range *rng_out, uint32_t *count)
{
	Response r;
	uint32_t cur = 0, mask = 0;
	struct alarm_range rng = {.snap = snap, .start = 0, .end = 0};
	size_t units = INFO_U_SCALARS + snap->n_alarms;

	for (size_t u = 0; u < units; u++) {
		bool is_alarm = u >= INFO_U_SCALARS;
		uint8_t a = (uint8_t)(u - INFO_U_SCALARS);

		if (!is_alarm && info_unit_empty(&snap->info, u)) {
			continue;
		}
		uint32_t tmask = is_alarm ? mask : (mask | BIT(u));
		struct alarm_range trng = rng;

		if (is_alarm) {
			/* Alarms on one page must be contiguous: a skipped one closes the
			 * run, so a new run always starts a new page. */
			if (trng.end == trng.start || trng.end != a) {
				trng.start = a;
			}
			trng.end = a + 1;
		}
		bool contiguous = !is_alarm || rng.end == rng.start || rng.end == a;

		if (contiguous) {
			info_page_fill(&r, snap, tmask, &trng, PAGE_COUNT_BOUND, PAGE_COUNT_BOUND);
			if (response_fits(&r, cap)) {
				mask = tmask;
				rng = trng;
				continue;
			}
		}

		/* Does it fit alone? If not, leave it out. */
		uint32_t amask = is_alarm ? 0 : BIT(u);
		struct alarm_range arng = {.snap = snap, .start = 0, .end = 0};

		if (is_alarm) {
			arng.start = a;
			arng.end = a + 1;
		}
		info_page_fill(&r, snap, amask, &arng, PAGE_COUNT_BOUND, PAGE_COUNT_BOUND);
		if (!response_fits(&r, cap)) {
			continue;
		}
		if (mask != 0 || rng.end != rng.start) {
			if (cur == want) {
				*mask_out = mask;
				*rng_out = rng;
			}
			cur++;
		}
		mask = amask;
		rng = arng;
	}
	if (mask == 0 && rng.end == rng.start) {
		return -EMSGSIZE; /* nothing fits */
	}
	if (cur == want) {
		*mask_out = mask;
		*rng_out = rng;
	}
	*count = cur + 1;
	return 0;
}

static int info_page(uint32_t page, uint8_t *out, size_t out_cap, size_t *out_len)
{
	const struct info_snap *snap = &m_page_stream.u.info;
	uint32_t mask = 0, count;
	struct alarm_range rng = {.snap = snap};
	Response r;
	int ret = info_layout(snap, m_page_stream.cap, page, &mask, &rng, &count);

	if (ret) {
		return ret;
	}
	if (page >= count) {
		return -ENODATA;
	}
	info_page_fill(&r, snap, mask, &rng, page, count);
	return encode_response(&r, out, out_cap, out_len);
}

/* Page a LoRaWAN Info that does not fit `cap`: snapshot it, encode page 0 into
 * `out` and, when more pages follow, arm the stream (*streamed = true). */
static int info_paged(uint32_t seq, uint8_t *out, size_t cap, size_t *out_len, bool *streamed)
{
	struct info_snap *snap = &m_page_stream.u.info;
	struct app_alarm_active list[ACTIVE_ALARM_SNAPSHOT_MAX];
	size_t n = app_alarm_active_snapshot(list, ARRAY_SIZE(list));
	uint32_t count;

	app_cmd_stream_cancel();
	memset(snap, 0, sizeof(*snap));
	fill_info(APP_CMD_TRANSPORT_LRW, &snap->info, SIZE_MAX);
	snap->info.active_alarms.funcs.encode = NULL;
	for (size_t i = 0; i < n; i++) {
		snap->alarm[i][0] = (uint8_t)list[i].source;
		snap->alarm[i][1] = (uint8_t)list[i].quantity;
		snap->alarm[i][2] = (uint8_t)list[i].type;
	}
	snap->n_alarms = (uint8_t)n;
	snap->seq = seq;

	m_page_stream.seq = seq;
	m_page_stream.cap = cap;

	uint32_t mask = 0;
	struct alarm_range rng = {.snap = snap};
	Response r;
	int ret = info_layout(snap, cap, 0, &mask, &rng, &count);

	if (ret) {
		return ret;
	}
	info_page_fill(&r, snap, mask, &rng, 0, count);
	ret = encode_response(&r, out, cap, out_len);
	if (ret) {
		return ret;
	}
	*streamed = count > 1;
	if (*streamed) {
		page_stream_start(PAGE_STREAM_INFO, seq, cap, count);
	}
	return 0;
}

/* ---- stream driver -------------------------------------------------------- */

static int request_page(uint32_t page, uint8_t *out, size_t out_cap, size_t *out_len)
{
	Command cmd = Command_init_zero;
	Response resp = Response_init_zero;
	enum app_cmd_action act = APP_CMD_ACTION_NONE;
	pb_istream_t istream =
		pb_istream_from_buffer(m_page_stream.u.req.buf, m_page_stream.u.req.len);

	if (!pb_decode(&istream, Command_fields, &cmd)) {
		return -EINVAL;
	}
	if (cmd.which_body == Command_get_config_tag) {
		cmd.body.get_config.has_page = true;
		cmd.body.get_config.page = page;
	} else if (cmd.which_body == Command_get_param_tag) {
		cmd.body.get_param.has_page = true;
		cmd.body.get_param.page = page;
	} else {
		return -EINVAL;
	}
	app_cmd_dispatch(APP_CMD_TRANSPORT_LRW, &cmd, &resp, &act);
	return encode_response(&resp, out, out_cap, out_len);
}

int app_cmd_stream_next(uint8_t *out, size_t out_cap, size_t *out_len)
{
	uint32_t count;
	int ret;

	if (!out || !out_len) {
		return -EINVAL;
	}
	if (m_page_stream.kind == PAGE_STREAM_NONE || m_page_stream.next >= m_page_stream.count) {
		m_page_stream.kind = PAGE_STREAM_NONE;
		return -ENODATA;
	}

	switch (m_page_stream.kind) {
	case PAGE_STREAM_REQUEST:
		ret = request_page(m_page_stream.next, out, out_cap, out_len);
		break;
	case PAGE_STREAM_SETTINGS:
		ret = config_status_page(m_page_stream.cap, m_page_stream.next, out, out_cap,
					 out_len, &count);
		break;
	case PAGE_STREAM_W1SCAN:
		ret = w1_scan_page(m_page_stream.next, out, out_cap, out_len);
		break;
	case PAGE_STREAM_INFO:
		ret = info_page(m_page_stream.next, out, out_cap, out_len);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (ret) {
		m_page_stream.kind = PAGE_STREAM_NONE;
		return ret;
	}
	if (++m_page_stream.next >= m_page_stream.count) {
		m_page_stream.kind = PAGE_STREAM_NONE;
	}
	return 0;
}

/* ---- host-driven pages (NFC / vendor / shell) ----------------------------- */

/* Over the phone channels the host asks for every page itself (GetInfo.page /
 * W1Scan.page); nothing is streamed and nothing is kept between requests. Each
 * page is laid out afresh from a new snapshot, so the pages of one read may
 * differ by the few tenths of a second between the requests (accepted). The
 * layout rules are the radio ones: greedy, a unit that does not fit alone is
 * left out, and every page is a complete, independently decodable Response. */
static bool host_paged(enum app_cmd_transport tp)
{
	return tp == APP_CMD_TRANSPORT_NFC || tp == APP_CMD_TRANSPORT_VENDOR ||
	       tp == APP_CMD_TRANSPORT_SHELL_DEBUG;
}

/* The requested page does not exist: OUT_OF_RANGE, fault_field 1 (= page), as
 * GetConfig answers. */
static void page_out_of_range(Response *resp, uint32_t seq)
{
	*resp = (Response)Response_init_zero;
	resp->seq = seq;
	make_error(resp, Response_Error_Code_OUT_OF_RANGE, "page");
	resp->body.error.fault_field = 1;
}

/* Page `page` of the Info for `cap`; `r` is scratch (the caller's Response). */
static int info_host_page(enum app_cmd_transport tp, uint32_t seq, uint32_t page, Response *r,
			  uint8_t *out, size_t cap, size_t *out_len)
{
	struct info_snap snap;
	struct app_alarm_active list[ACTIVE_ALARM_SNAPSHOT_MAX];
	size_t n = app_alarm_active_snapshot(list, ARRAY_SIZE(list));

	memset(&snap, 0, sizeof(snap));
	fill_info(tp, &snap.info, SIZE_MAX);
	snap.info.active_alarms.funcs.encode = NULL;
	for (size_t i = 0; i < n; i++) {
		snap.alarm[i][0] = (uint8_t)list[i].source;
		snap.alarm[i][1] = (uint8_t)list[i].quantity;
		snap.alarm[i][2] = (uint8_t)list[i].type;
	}
	snap.n_alarms = (uint8_t)n;
	snap.seq = seq;

	uint32_t mask = 0, count = 0;
	struct alarm_range rng = {.snap = &snap};
	int ret = info_layout(&snap, cap, page, &mask, &rng, &count);

	if (ret) {
		return ret;
	}
	if (page >= count) {
		page_out_of_range(r, seq);
	} else {
		info_page_fill(r, &snap, mask, &rng, page, count);
	}
	return encode_response(r, out, cap, out_len);
}

/* Page `page` of the W1Scan result in `r` (split by ROM) for `cap`. */
static int w1_scan_host_page(Response *r, uint32_t page, uint8_t *out, size_t cap, size_t *out_len)
{
	const Response_W1Scan all = r->body.w1_scan;
	pb_size_t n = all.rom_count;
	pb_size_t k = n;

	/* Most ROMs per page that still fit with (worst-case) page fields. */
	for (; k > 0; k--) {
		r->body.w1_scan.rom_count = k;
		r->page_index = r->page_count = (k < n) ? PAGE_COUNT_BOUND : 0;
		if (response_fits(r, cap)) {
			break;
		}
	}
	if (n > 0 && k == 0) {
		return -EMSGSIZE; /* not even one ROM fits */
	}

	uint32_t count = (n == 0) ? 1 : (n + k - 1) / k;

	r->page_index = r->page_count = 0;
	if (page >= count) {
		page_out_of_range(r, r->seq);
		return encode_response(r, out, cap, out_len);
	}
	r->body.w1_scan.rom_count = 0;
	for (size_t i = (size_t)page * k; i < n && i < ((size_t)page + 1) * k; i++) {
		r->body.w1_scan.rom[r->body.w1_scan.rom_count++] = all.rom[i];
	}
	set_page(r, page, count);
	return encode_response(r, out, cap, out_len);
}

#ifdef CONFIG_ZTEST
/* Test hook: the host-driven W1Scan pages without a 1-Wire bus. */
int test_w1_scan_host_page(const uint8_t roms[][8], size_t n, uint32_t seq, uint32_t page,
			   uint8_t *out, size_t cap, size_t *out_len)
{
	Response r = Response_init_zero;

	r.seq = seq;
	r.which_body = Response_w1_scan_tag;
	for (size_t i = 0; i < n && i < ARRAY_SIZE(r.body.w1_scan.rom); i++) {
		r.body.w1_scan.rom[i].size = 8;
		memcpy(r.body.w1_scan.rom[i].bytes, roms[i], 8);
		r.body.w1_scan.rom_count++;
	}
	return w1_scan_host_page(&r, page, out, cap, out_len);
}
#endif

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

		/* #409 3d/3e: over LoRaWAN a multi-page GetConfig/GetParam streams
		 * every remaining page by itself (the host sends one request). NFC keeps
		 * its host-driven paging (big pages, read in one RF session). */
		if (transport == APP_CMD_TRANSPORT_LRW &&
		    (cmd.which_body == Command_get_config_tag ||
		     cmd.which_body == Command_get_param_tag)) {
			app_cmd_stream_cancel();
			if (page_stream_arm(in, in_len, &resp)) {
				act = APP_CMD_ACTION_PAGE_STREAM;
			}
		} else if (transport == APP_CMD_TRANSPORT_LRW &&
			   resp.which_body == Response_w1_scan_tag) {
			/* #425: a scan that does not fit is paged by ROM. */
			app_cmd_stream_cancel();
			if (w1_scan_arm_pages(&resp, out_cap)) {
				act = APP_CMD_ACTION_PAGE_STREAM;
			}
		}
	}

	/* #409 3a: over LoRaWAN an Error carries code + fault_field only. The detail
	 * string (up to 32 B) made even an Error too big for the 11 B budget tier
	 * (US915 DR0, AU915/AS923 DR2), so a failed command went unanswered. NFC
	 * keeps the human-readable detail. */
	if (transport == APP_CMD_TRANSPORT_LRW && resp.which_body == Response_error_tag) {
		resp.body.error.detail[0] = '\0';
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

	/* Host-driven pages (NFC / vendor / shell): GetInfo.page / W1Scan.page picks
	 * the page; an Info / W1Scan that does not fit the frame answers page 0 of N
	 * instead of being trimmed. */
	uint32_t host_page = 0;

	if (cmd.which_body == Command_get_info_tag && cmd.body.get_info.has_page) {
		host_page = cmd.body.get_info.page;
	} else if (cmd.which_body == Command_w1_scan_tag && cmd.body.w1_scan.has_page) {
		host_page = cmd.body.w1_scan.page;
	}
	const bool host = host_paged(transport) && (resp.which_body == Response_info_tag ||
						    resp.which_body == Response_w1_scan_tag);
	int ret = -EMSGSIZE;

	if (!host || host_page == 0) {
		ret = encode_response(&resp, out, out_cap, out_len);
	}
	if (host && ret == -EMSGSIZE) {
		ret = (resp.which_body == Response_info_tag)
			      ? info_host_page(transport, resp.seq, host_page, &resp, out, out_cap,
					       out_len)
			      : w1_scan_host_page(&resp, host_page, out, out_cap, out_len);
	}

	if (ret == -EMSGSIZE && resp.which_body == Response_info_tag &&
	    transport == APP_CMD_TRANSPORT_LRW) {
		/* #425: a GetInfo that does not fit the payload budget is paged —
		 * fields and active alarms spread over self-contained Info pages —
		 * instead of trimmed. */
		bool streamed = false;

		ret = info_paged(resp.seq, out, out_cap, out_len, &streamed);
		if (ret == 0 && streamed) {
			act = APP_CMD_ACTION_PAGE_STREAM;
		}
	}

	/* Last resort for an Info that still overflows (not even one page fits):
	 * drop active alarms one at a time and re-encode before giving up — the rest
	 * of Info is worth far more than the alarm list. */
	for (size_t max_alarms = ACTIVE_ALARM_SNAPSHOT_MAX;
	     ret == -EMSGSIZE && resp.which_body == Response_info_tag && max_alarms-- > 0;) {
		resp.body.info.active_alarms.arg = (void *)(uintptr_t)max_alarms;
		ret = encode_response(&resp, out, out_cap, out_len);
	}

	if (ret == -EMSGSIZE) {
		/* The composed response doesn't fit the transport buffer. Don't fail
		 * silently (#93.3) — replace it with a compact Error carrying the same
		 * seq so the host learns the request couldn't be answered (e.g. an
		 * over-broad GetParam/GetConfig page). */
		LOG_WRN("Response too large for buffer; sending Error instead");
		Response err = Response_init_zero;
		err.seq = resp.seq;
		/* #409: over LoRaWAN the only reason is the DR payload budget, so say
		 * so — the host should retry once ADR raises the DR. */
		if (transport == APP_CMD_TRANSPORT_LRW) {
			make_error(&err, Response_Error_Code_BUDGET_TOO_SMALL, NULL);
		} else {
			make_error(&err, Response_Error_Code_UNKNOWN, "response too large");
		}
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

int app_cmd_build_budget_error(uint32_t seq, uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (!out || !out_len) {
		return -EINVAL;
	}

	Response resp = Response_init_zero;
	resp.seq = seq;
	make_error(&resp, Response_Error_Code_BUDGET_TOO_SMALL, NULL);
	return encode_response(&resp, out, out_cap, out_len);
}

int app_cmd_build_info(uint8_t *out, size_t out_cap, size_t *out_len, bool *more)
{
	if (!out || !out_len || !more) {
		return -EINVAL;
	}
	*more = false;

	Response resp = Response_init_zero;
	resp.seq = 0;
	resp.which_body = Response_info_tag;
	/* Autonomous GetInfo on join goes out over LoRaWAN, so dev_eui is omitted. */
	fill_info(APP_CMD_TRANSPORT_LRW, &resp.body.info, SIZE_MAX);

	/* out_cap is the current payload budget (see queue_info_uplink()). If the
	 * full Info does not fit, page it (#425): page 0 goes out here, the rest via
	 * app_cmd_stream_next(). */
	int ret = encode_response(&resp, out, out_cap, out_len);

	if (ret == -EMSGSIZE) {
		ret = info_paged(0, out, out_cap, out_len, more);
	}
	return ret;
}

int app_cmd_build_config_status(uint8_t *out, size_t out_cap, size_t *out_len, bool *more)
{
	uint32_t count;

	if (!out || !out_len || !more) {
		return -EINVAL;
	}
	*more = false;

	/* Autonomous boot settings-info uplink (#412): ConfigDump page(s) with a
	 * fixed selection of the key operating settings, so the LNS learns the
	 * device's effective configuration on join without polling. Same content as
	 * a GetConfig reply, plus the runtime-only w1_slot_type; the persisted ROM
	 * serials are left out (GetParam sensors 11..14). #425: one page when it fits
	 * out_cap (EU868 DR0 and up), else as many pages as the budget needs — page
	 * 0 here, the rest via app_cmd_stream_next(). */
	int ret = config_status_page(out_cap, 0, out, out_cap, out_len, &count);

	if (ret) {
		return ret;
	}
	if (count > 1) {
		page_stream_start(PAGE_STREAM_SETTINGS, 0, out_cap, count);
		*more = true;
	}
	return 0;
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
	set_page(&resp, frame_index, frame_count);
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
	set_page(&resp, frame_index, frame_count);
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
			       uint32_t page_index, uint32_t page_count, uint8_t *out,
			       size_t out_cap, size_t *out_len)
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
	if (page_count > 1) { /* #425: same paging as Response */
		report.page_index = page_index;
		report.page_count = page_count;
	}

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
