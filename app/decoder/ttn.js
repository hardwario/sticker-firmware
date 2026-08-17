// Strict ES5 only in this whole file — the ChirpStack v3 JS engine (otto)
// rejects any ES6 syntax at parse time, even in code that never executes.

// ChirpStack v3 entry point — delegates to the TTS v3 / ChirpStack v4 codec below.
function Decode(fPort, bytes, variables) {
  return decodeUplink({ fPort: fPort, bytes: bytes }).data;
}

// Minimal protobuf wire-format reader for DownlinkResponse on fPort 85.
// Avoids pulling in protobufjs (TTN/ChirpStack JS sandboxes typically
// block require()). Only handles varint (wire 0) and length-delimited
// (wire 2) — enough for DownlinkResponse{ seq, Info{...}, Error{...} }.
function _pbReadVarint(bytes, offset) {
  var result = 0;
  var shift = 0;
  var pos = offset;
  while (pos < bytes.length) {
    var b = bytes[pos++];
    result |= (b & 0x7f) << shift;
    if ((b & 0x80) === 0) {
      return { value: result >>> 0, next: pos };
    }
    shift += 7;
    if (shift >= 32) {
      return { value: result >>> 0, next: pos };
    }
  }
  return { value: result >>> 0, next: pos };
}

function _pbBytesToAscii(bytes) {
  var s = "";
  for (var i = 0; i < bytes.length; i++) {
    s += String.fromCharCode(bytes[i]);
  }
  return s;
}

function _pbBytesToHex(bytes) {
  var s = "";
  for (var i = 0; i < bytes.length; i++) {
    var h = bytes[i].toString(16);
    s += h.length === 1 ? "0" + h : h;
  }
  return s;
}

// Decode a little-endian IEEE-754 float32 (protobuf wire type 5).
function _pbReadFloat(bytes, offset) {
  var b0 = bytes[offset], b1 = bytes[offset + 1], b2 = bytes[offset + 2], b3 = bytes[offset + 3];
  var bits = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
  var sign = bits < 0 ? -1 : 1;
  var exp = (bits >>> 23) & 0xff;
  var mant = bits & 0x7fffff;
  var val;
  if (exp === 0) {
    val = mant * Math.pow(2, -149);
  } else if (exp === 0xff) {
    val = mant ? NaN : Infinity;
  } else {
    val = (mant + 0x800000) * Math.pow(2, exp - 150);
  }
  return { value: sign * val, next: offset + 4 };
}

var _BUILD_TYPES = ["main", "dev", "custom"];
var _LRW_STATES = ["idle", "joining", "healthy", "warning", "reconnect", "disabled"];

// device_status (Info field 14) bit -> name. Keep in sync with APP_DEVICE_STATUS_*.
var _DEVICE_STATUS = [
  [1 << 0, "alarm_any"],
  [1 << 1, "alarm_threshold"],
  [1 << 2, "alarm_state"],
  [1 << 3, "alarm_rate"],
  [1 << 4, "alarm_no_data"],
  [1 << 5, "alarm_low_battery"],
  [1 << 8, "nfc_down"],
  [1 << 9, "history_down"],
  [1 << 10, "i2c_wedged"],
  [1 << 11, "time_unsynced"],
  [1 << 12, "lrw_disabled"],
];

// reset_cause (Info field 11) bit -> name. Zephyr hwinfo RESET_* bitmask of the
// last boot, so the backend can see WHY the device restarted (e.g. watchdog vs
// brownout vs power-on) from the on-join GetInfo. Keep in sync with app_ats.c.
var _RESET_CAUSES = [
  [1 << 0, "pin"],
  [1 << 1, "software"],
  [1 << 2, "brownout"],
  [1 << 3, "power_on"],
  [1 << 4, "watchdog"],
  [1 << 6, "security"],
  [1 << 7, "low_power_wake"],
];

// Config submessage maps: proto field tag -> name. The flat C struct is split
// across submessages lorawan/application/sensors/alarms; device identity stays
// at the AppConfigMessage root (not addressable via SetParam/GetConfig).
// proto_ids are contiguous 1..N per submessage (aligned in #166).
var _APP_NAMES = {
  1: "calibration", 2: "interval_sample", 3: "interval_report",
  4: "history_enable", 5: "history_sensors", 6: "battery_level",
  7: "vendor_reset_allow"
};
var _APP_ENUMS = {};
var _APP_FLOAT = {};

var _SEN_NAMES = {
  1: "cap_hall_left", 2: "cap_hall_right", 3: "cap_input_a", 4: "cap_input_b",
  5: "cap_light_sensor", 6: "cap_barometer", 7: "cap_pir_detector",
  8: "cap_w1_sensors", 9: "cap_accelerometer", 10: "accel_motion_sensitivity",
  15: "hall_left_counter", 16: "hall_right_counter",
  17: "input_a_counter", 18: "input_b_counter"
};
var _SEN_ENUMS = { 10: ["off", "low", "medium", "high"] };
var _SEN_FLOAT = {};
var _SEN_HEX = { 11: "sensor1_rom", 12: "sensor2_rom", 13: "sensor3_rom", 14: "sensor4_rom" };
var _SEN_HEX_ENC = { sensor1_rom: 11, sensor2_rom: 12, sensor3_rom: 13, sensor4_rom: 14 };

var _ALM_NAMES = {
  1: "alarm_limit"
};
var _ALM_ENUMS = {};
var _ALM_FLOAT = {};
// Dynamic alarm rule slots alarm_0..alarm_15 = proto fields 3..18, each a packed
// 17-byte rule carried as native bytes (presented/authored as a 34-char hex string).
var _ALM_HEX = {};
var _ALM_HEX_ENC = {};
(function () {
  for (var i = 0; i < 16; i++) { _ALM_HEX[3 + i] = "alarm_" + i; _ALM_HEX_ENC["alarm_" + i] = 3 + i; }
})();

// Names drop the `lrw_` prefix the YAML carries (region <- lrw_region, ...).
var _LRW_NAMES = {
  1: "region", 2: "sub_band", 3: "network", 4: "adr", 5: "activation",
  13: "link_check_interval", 14: "link_check_fail_rejoin", 15: "radio_mode"
};
var _LRW_HEX = { 6: "deveui", 7: "joineui", 10: "devaddr" };

// Reverse maps (name -> tag) for encoding SetParam. The LoRaWAN hex set adds the
// secret keys (nwkkey/appkey/nwkskey/appskey) which the decoder deliberately
// hides but which a downlink may legitimately set.
var _LRW_HEX_ENC = { deveui: 6, joineui: 7, nwkkey: 8, appkey: 9, devaddr: 10, nwkskey: 11, appskey: 12 };
var _LRW_ENUM = {
  region: { EU868: 0, US915: 1, AU915: 2 },
  network: { PUBLIC: 0, PRIVATE: 1 },
  activation: { OTAA: 0, ABP: 1 },
  radio_mode: { OFF: 0, LORAWAN: 1, P2P: 2 }
};
function _invert(map) {
  var out = {};
  for (var k in map) { if (map.hasOwnProperty(k)) out[map[k]] = +k; }
  return out;
}
var _APP_TAGS = _invert(_APP_NAMES);
var _SEN_TAGS = _invert(_SEN_NAMES);
var _ALM_TAGS = _invert(_ALM_NAMES);
var _LRW_TAGS = _invert(_LRW_NAMES);

// proto field tag -> command name in the DownlinkCommand body oneof.
// BEGIN GENERATED COMMANDS
var _CMD_NAMES = {
  2: "set_param",
  3: "get_param",
  4: "get_info",
  5: "get_config",
  6: "settings_save",
  7: "reboot",
  8: "device_reset",
  9: "force_send",
  10: "reset_counters",
  11: "req_history",
  12: "clock_sync",
  13: "req_history_page",
  14: "w1_scan",
  16: "lrw_reset",
  17: "lrw_join",
  18: "enter_calibration",
  21: "sample",
  23: "factory_reset",
  24: "set_secret_key",
  25: "clm_ack",
  26: "vendor_reset",
  27: "clm_rearm",
  28: "buzzer_play",
};
// END GENERATED COMMANDS
var _CMD_TAGS = _invert(_CMD_NAMES);

function _decodeLorawan(bytes, start, end) {
  var o = {}, pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var f = tag.value >>> 3, w = tag.value & 0x7;
    if (w === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (_LRW_NAMES[f]) o[_LRW_NAMES[f]] = v.value;
    } else if (w === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      if (_LRW_HEX[f]) o[_LRW_HEX[f]] = _pbBytesToHex(bytes.slice(pos, pos + len.value));
      pos += len.value;
    } else { break; }
  }
  return o;
}

// Generic config-submessage decoder: varint (with optional symbolic enum),
// float (wire 5), and bytes -> hex (HEX map). Used for application/sensors/alarms.
function _decodeCfgGroup(bytes, start, end, NAMES, ENUMS, HEX) {
  var o = {}, pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var f = tag.value >>> 3, w = tag.value & 0x7;
    if (w === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (NAMES[f]) {
        var ev = ENUMS && ENUMS[f] && ENUMS[f][v.value] !== undefined;
        o[NAMES[f]] = ev ? ENUMS[f][v.value] : v.value;
      }
    } else if (w === 5) {
      var fl = _pbReadFloat(bytes, pos); pos = fl.next;
      if (NAMES[f]) o[NAMES[f]] = fl.value;
    } else if (w === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      if (HEX && HEX[f]) o[HEX[f]] = _pbBytesToHex(bytes.slice(pos, pos + len.value));
      pos += len.value;
    } else { break; }
  }
  return o;
}

function _decodeApplication(b, s, e) { return _decodeCfgGroup(b, s, e, _APP_NAMES, _APP_ENUMS, null); }
function _decodeSensors(b, s, e) { return _decodeCfgGroup(b, s, e, _SEN_NAMES, _SEN_ENUMS, _SEN_HEX); }
function _decodeAlarms(b, s, e) { return _decodeCfgGroup(b, s, e, _ALM_NAMES, _ALM_ENUMS, _ALM_HEX); }

function _decodeConfigDump(bytes, start, end) {
  var cd = {}, pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var f = tag.value >>> 3, w = tag.value & 0x7;
    if (w === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (f === 1) cd.page_index = v.value;
      else if (f === 2) cd.page_count = v.value;
    } else if (w === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      var e2 = pos + len.value;
      if (f === 3) cd.lorawan = _decodeLorawan(bytes, pos, e2);
      else if (f === 4) cd.application = _decodeApplication(bytes, pos, e2);
      else if (f === 5) cd.sensors = _decodeSensors(bytes, pos, e2);
      else if (f === 6) cd.alarms = _decodeAlarms(bytes, pos, e2);
      pos = e2;
    } else { break; }
  }
  return cd;
}

function _decodeInfo(bytes, start, end) {
  var info = { fw_major: 0, fw_minor: 0, fw_patch: 0, build_type: 0, debug: false, device_status: 0, active_alarms: [] };
  var pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (field === 1) info.fw_major = v.value;
      else if (field === 2) info.fw_minor = v.value;
      else if (field === 3) info.fw_patch = v.value;
      else if (field === 4) info.build_type = v.value;
      else if (field === 5) info.serial_number = v.value;
      else if (field === 6) info.uptime_s = v.value;
      else if (field === 7) info.unix_time = v.value;
      else if (field === 8) info.debug = v.value !== 0;
      else if (field === 10) info.battery = v.value; // supply voltage in mV (0/absent = unavailable)
      else if (field === 11) info.reset_cause = v.value; // hwinfo reset-cause bitmask of last boot (#88)
      else if (field === 12) info.lrw_state = v.value; // LoRaWAN network state; emitted over NFC only, absent from LoRaWAN uplinks
      else if (field === 14) info.device_status = v.value; // aggregated device status bitmask
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      // field 9 = claim_token (#170): 128-bit device claim token, presented as
      // hex. Omitted by the device until commissioned.
      if (field === 9) info.claim_token = _pbBytesToHex(bytes.slice(pos, pos + len.value));
      // field 13 = dev_eui: 8-byte DevEUI as hex. Emitted over NFC only, so it
      // is absent from LoRaWAN uplinks.
      else if (field === 13) info.dev_eui = _pbBytesToHex(bytes.slice(pos, pos + len.value));
      // field 15 = repeated AlarmStatus active_alarms (#288): one entry per alarm
      // latched active when Info was built. Same enums as the fPort 3 AlarmReport.
      else if (field === 15) info.active_alarms.push(_decodeAlarmStatus(bytes, pos, pos + len.value));
      // else: skip unknown length-delimited fields (forward compatibility).
      pos += len.value;
    } else {
      break;
    }
  }
  info.fw_version = info.fw_major + "." + info.fw_minor + "." + info.fw_patch;
  info.build_type_name = _BUILD_TYPES[info.build_type] || "unknown";
  // lrw_state is NFC-only, so it is absent from LoRaWAN uplinks — only name it
  // when the field was actually present (leave both undefined otherwise).
  if (info.lrw_state !== undefined) {
    info.lrw_state_name = _LRW_STATES[info.lrw_state] || "unknown";
  }
  info.device_status_flags = _DEVICE_STATUS
    .filter(function (f) { return (info.device_status & f[0]) !== 0; })
    .map(function (f) { return f[1]; });
  // Decode the reset-cause bitmask into names so the backend sees why the device
  // rebooted (present on every join GetInfo). 0 = unknown; omitted if absent.
  if (info.reset_cause !== undefined) {
    info.reset_cause_flags = _RESET_CAUSES
      .filter(function (f) { return (info.reset_cause & f[0]) !== 0; })
      .map(function (f) { return f[1]; });
  }
  return info;
}

function _decodeError(bytes, start, end) {
  var err = {};
  var pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (field === 1) err.code = v.value;
      else if (field === 2) {
        // fault_field is encoded group*100 + tag (#196): split into a readable
        // group (1=lorawan 2=application 3=sensors 4=alarms; 0=not group-scoped)
        // and the proto tag within that group.
        err.fault_field = v.value % 100;
        err.fault_group = Math.floor(v.value / 100);
      }
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      if (field === 3) err.detail = _pbBytesToAscii(bytes.slice(pos, pos + len.value));
      pos += len.value;
    } else {
      break;
    }
  }
  return err;
}

// W1Scan response (fPort 85): repeated `rom` (field 1), each 8 bytes
// (family + 6-byte serial + CRC). Returned as hex strings; the host picks one
// and teaches a slot via SetParam sensorN_rom.
function _decodeW1Scan(bytes, start, end) {
  var roms = [];
  var pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      if (field === 1) roms.push(_pbBytesToHex(bytes.slice(pos, pos + len.value)));
      pos += len.value;
    } else {
      break;
    }
  }
  return { rom: roms };
}

// app_history_sensor enum order → name + encoding (mirrors app_history.c).
var _HIST_SENSORS = [
  { name: "temperature", enc: "temp" },
  { name: "humidity", enc: "hum" },
  { name: "s1_temperature", enc: "temp" },
  { name: "s1_humidity", enc: "hum" },
  { name: "s2_temperature", enc: "temp" },
  { name: "s2_humidity", enc: "hum" },
  { name: "s3_temperature", enc: "temp" },
  { name: "s3_humidity", enc: "hum" },
  { name: "s4_temperature", enc: "temp" },
  { name: "s4_humidity", enc: "hum" },
  { name: "hall_left_count", enc: "count" },
  { name: "hall_right_count", enc: "count" },
  { name: "input_a_count", enc: "count" },
  { name: "input_b_count", enc: "count" },
  { name: "motion_count", enc: "count" },
  // #311: barometer/light/accelerometer — already sampled + in telemetry,
  // newly recordable in history. accel_motion_count is the LIS2DH any-motion
  // event counter, distinct from motion_count above (PIR person-detection).
  { name: "pressure", enc: "pressure" },
  { name: "illuminance", enc: "lux" },
  { name: "orientation", enc: "orient" },
  { name: "accel_motion_count", enc: "count" }
];
var _HIST_TEMP_SENTINEL = 0x7fff;
var _HIST_HUM_SENTINEL = 0xff;
var _HIST_PRESSURE_SENTINEL = 0xffff;
var _HIST_LUX_SENTINEL = 0xffff;
var _HIST_ORIENT_SENTINEL = 0xff;

// HistoryFrame carries a shared `present` mask + `interval_s` once; samples is a
// sequence of fixed-size, values-only records (no per-record time or mask):
//   [per present sensor: scaled value]   int16 LE ×100 (temp), uint8 ×2 (hum),
//                                         uint32 LE (counter), uint16 LE ×10
//                                         (pressure, hPa), uint16 LE /2 (lux),
//                                         uint8 raw (orientation); sentinel = null.
// Record j's time is t0_unix + j*interval_s (records are periodic). One
// ReqHistory yields N such frames (frame_index 0..frame_count-1); the consumer
// concatenates their records to reconstruct the requested window.
function _decodeHistorySamples(bytes, t0, present, interval, synced) {
  var out = [];
  var recSize = 0;
  for (var s = 0; s < _HIST_SENSORS.length; s++) {
    if (!(present & (1 << s))) continue;
    var e = _HIST_SENSORS[s].enc;
    recSize += (e === "temp" || e === "pressure" || e === "lux") ? 2
      : (e === "hum" || e === "orient") ? 1 : 4;
  }
  if (recSize === 0) return out;

  var p = 0, j = 0;
  while (p + recSize <= bytes.length) {
    // time=null when the clock had not synced at capture (L-1/L-3): t0 is only
    // uptime-relative, so an absolute timestamp would be a bogus ~1970 date.
    var rec = { time: synced ? ((t0 + j * interval) >>> 0) : null };
    for (var k = 0; k < _HIST_SENSORS.length; k++) {
      if (!(present & (1 << k))) continue;
      var d = _HIST_SENSORS[k];
      if (d.enc === "temp") {
        var raw = bytes[p] | (bytes[p + 1] << 8); p += 2;
        rec[d.name] = raw === _HIST_TEMP_SENTINEL ? null
          : (raw > 0x7fff ? raw - 0x10000 : raw) / 100;
      } else if (d.enc === "hum") {
        var hv = bytes[p]; p += 1;
        rec[d.name] = hv === _HIST_HUM_SENTINEL ? null : hv / 2;
      } else if (d.enc === "pressure") {
        var pv = bytes[p] | (bytes[p + 1] << 8); p += 2;
        rec[d.name] = pv === _HIST_PRESSURE_SENTINEL ? null : pv / 10;
      } else if (d.enc === "lux") {
        var lv = bytes[p] | (bytes[p + 1] << 8); p += 2;
        rec[d.name] = lv === _HIST_LUX_SENTINEL ? null : lv * 2;
      } else if (d.enc === "orient") {
        var ov = bytes[p]; p += 1;
        rec[d.name] = ov === _HIST_ORIENT_SENTINEL ? null : ov;
      } else { // count
        rec[d.name] = (bytes[p] | (bytes[p + 1] << 8) | (bytes[p + 2] << 16) |
                       (bytes[p + 3] << 24)) >>> 0; p += 4;
      }
    }
    out.push(rec);
    j++;
  }
  return out;
}

function _decodeHistoryFrame(bytes, start, end) {
  // frame_index/frame_count default to 0 — proto3 omits a zero frame_index, so
  // frame 0 of a replay carries no field 1; the consumer still needs index 0.
  var hf = { frame_index: 0, frame_count: 0, records: [] };
  var t0 = 0, present = 0, interval = 0;
  // time_synced (field 7) absent = old FW = treat as synced (emit timestamps).
  var synced = true;
  // next_ord (field 8) / has_more (field 9): NFC paged read (#260, req_history_page)
  // only. Absent over the LoRaWAN device-driven replay; surfaced here so the same
  // decoder is a complete reference for the NFC (Manager-App) paging cursor.
  var samples = null;
  var pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var f = tag.value >>> 3, w = tag.value & 0x7;
    if (w === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (f === 1) hf.frame_index = v.value;
      else if (f === 2) hf.frame_count = v.value;
      else if (f === 3) t0 = v.value;
      else if (f === 5) present = v.value;
      else if (f === 6) interval = v.value;
      else if (f === 7) synced = v.value !== 0;
      else if (f === 8) hf.next_ord = v.value;
      else if (f === 9) hf.has_more = v.value !== 0;
    } else if (w === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      if (f === 4) samples = bytes.slice(pos, pos + len.value);
      pos += len.value;
    } else { break; }
  }
  hf.t0_unix = t0;
  hf.present = present;
  hf.interval_s = interval;
  hf.time_synced = synced;
  if (samples) hf.records = _decodeHistorySamples(samples, t0, present, interval, synced);
  return hf;
}

function decodeDownlinkResponse(bytes) {
  var resp = {};
  var pos = 0;
  while (pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (field === 1) resp.seq = v.value;
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      var end = pos + len.value;
      if (field === 2) resp.ack = {};
      else if (field === 3) resp.info = _decodeInfo(bytes, pos, end);
      else if (field === 4) resp.config_dump = _decodeConfigDump(bytes, pos, end);
      else if (field === 5) resp.history_frame = _decodeHistoryFrame(bytes, pos, end);
      else if (field === 6) resp.error = _decodeError(bytes, pos, end);
      else if (field === 7) resp.w1_scan = _decodeW1Scan(bytes, pos, end);
      pos = end;
    } else {
      break;
    }
  }
  return resp;
}

// Zig-zag decode for protobuf sint32.
function _pbZigzag(n) {
  return (n >>> 1) ^ -(n & 1);
}

// enum app_w1_slot_type → label (mirrors app_w1_slots.h).
var _W1_SLOT_TYPES = { 1: "dallas", 2: "machine-probe" };

// One SensorReading submessage (Telemetry field 27): slot=1 (1-based, matches
// sensorN config / `w1 list`), type=2,
// temperature=3 (sint32 ×100), humidity=4 (uint ×2), flags=5 (bit0 tilt),
// illuminance=6 (uint lux), magnetic_field=7 (sint µT → mT /1000), accel
// x/y/z=8/9/10 (sint m/s² ×100). Machine-probe carries 6-10; a sub-sensor that
// did not respond is absent. Absent quantities stay undefined. `bytes[start..end)`
// is the submessage body.
function _decodeSensorReading(bytes, start, end) {
  var sr = {};
  var pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (wire !== 0) { // forward-compat: skip unknown non-varint
      if (wire === 2) { var l = _pbReadVarint(bytes, pos); pos = l.next + l.value; continue; }
      if (wire === 5) { pos += 4; continue; }
      if (wire === 1) { pos += 8; continue; }
      break;
    }
    var v = _pbReadVarint(bytes, pos); pos = v.next;
    switch (field) {
      case 1: sr.slot = v.value; break;
      case 2: sr.type = v.value; sr.type_name = _W1_SLOT_TYPES[v.value] || "unknown"; break;
      case 3: { var _t = _pbZigzag(v.value); sr.temperature = (_t === _TM_S32_NA) ? null : _t / 100; break; }
      case 4: sr.humidity = v.value / 2; break;
      case 5: sr.tilt_alert = (v.value & (1 << 0)) !== 0; break;
      case 6: sr.illuminance = v.value; break;
      case 7: sr.magnetic_field = _pbZigzag(v.value) / 1000; break; // mT
      case 8: sr.accel_x = _pbZigzag(v.value) / 100; break;         // m/s²
      case 9: sr.accel_y = _pbZigzag(v.value) / 100; break;
      case 10: sr.accel_z = _pbZigzag(v.value) / 100; break;
      default: break;
    }
  }
  return sr;
}

// fPort 2: flat protobuf Telemetry (periodic / event-triggered report). Keys
// match the legacy fPort-1 bitmap decoder so dashboards stay stable. Unknown
// fields are skipped (forward-compatible with newer firmware). Per #80 the
// firmware always sends the system group and every enabled-sensor group whole,
// so boolean fields (boot, *_is_active, *_notify_*, tilt) are surfaced as
// explicit true/false rather than set-only-when-true.
// "Value not available" sentinels (mirror app_compose.c): an enabled analog
// sensor is always on the wire so the configured-sensor list is stable; a NaN
// reading arrives as the sentinel and decodes to null.
var _TM_S32_NA = -2147483648; // INT32_MIN (sint32: temperature, altitude)
var _TM_U32_NA = 4294967295;  // UINT32_MAX (uint32: humidity, pressure, illuminance)

function decodeTelemetry(bytes) {
  var d = {};
  var pos = 0;
  while (pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    // 1-wire ROM-bound slots: repeated SensorReading (field 27, length-delimited).
    if (field === 27 && wire === 2) {
      var sl = _pbReadVarint(bytes, pos); pos = sl.next;
      if (!d.w1_sensors) { d.w1_sensors = []; }
      d.w1_sensors.push(_decodeSensorReading(bytes, pos, pos + sl.value));
      pos += sl.value;
      continue;
    }
    if (wire !== 0) {
      // Forward-compat: skip an unknown non-varint field.
      if (wire === 2) { var l = _pbReadVarint(bytes, pos); pos = l.next + l.value; continue; }
      if (wire === 5) { pos += 4; continue; }
      if (wire === 1) { pos += 8; continue; }
      break;
    }
    var v = _pbReadVarint(bytes, pos); pos = v.next;
    var f;
    switch (field) {
      // system
      case 1:  d.voltage = (v.value === 0) ? null : v.value / 50; break; // 0 = pre-sample sentinel (L-51)
      case 2:  d.boot = (v.value & (1 << 0)) !== 0; break;     // system_flags (always sent)
      // internal (SHT4x) — sentinel → null (sensor enabled but no valid sample)
      case 3:  { var _t = _pbZigzag(v.value); d.temperature = (_t === _TM_S32_NA) ? null : _t / 100; break; }
      case 4:  d.humidity = (v.value === _TM_U32_NA) ? null : v.value / 2; break;
      // barometer
      case 5:  d.pressure = (v.value === _TM_U32_NA) ? null : v.value / 10; break; // hPa x10
      case 6:  { var _a = _pbZigzag(v.value); d.altitude = (_a === _TM_S32_NA) ? null : _a / 10; break; }
      // light
      case 7:  d.illuminance = (v.value === _TM_U32_NA) ? null : v.value * 2; break;
      // accel
      case 8:  d.orientation = v.value; break;
      // pir
      case 9:  d.motion_count = v.value; break;
      // 1-wire slots are repeated SensorReading (field 27, handled above).
      // The flat fields 10-17 below are retired in current firmware but kept
      // decodable so one formatter serves a mixed fleet (older firmware still
      // emits them); new firmware never sends both.
      case 10: d.ext_temperature_1 = _pbZigzag(v.value) / 100; break;
      case 11: d.ext_temperature_2 = _pbZigzag(v.value) / 100; break;
      case 12: d.machine_probe_temperature_1 = _pbZigzag(v.value) / 100; break;
      case 13: d.machine_probe_humidity_1 = v.value / 2; break;
      case 14: d.machine_probe_tilt_alert_1 = (v.value & (1 << 0)) !== 0; break;
      case 15: d.machine_probe_temperature_2 = _pbZigzag(v.value) / 100; break;
      case 16: d.machine_probe_humidity_2 = v.value / 2; break;
      case 17: d.machine_probe_tilt_alert_2 = (v.value & (1 << 0)) !== 0; break;
      // hall left (flag bits 0/1 notify retired with dynamic-alarms; active = bit 2)
      case 18: d.hall_left_count = v.value; break;
      case 19:
        d.hall_left_is_active = (v.value & (1 << 2)) !== 0;
        break;
      // hall right
      case 20: d.hall_right_count = v.value; break;
      case 21:
        d.hall_right_is_active = (v.value & (1 << 2)) !== 0;
        break;
      // input A
      case 22: d.input_a_count = v.value; break;
      case 23:
        d.input_a_is_active = (v.value & (1 << 2)) !== 0;
        break;
      // input B
      case 24: d.input_b_count = v.value; break;
      case 25:
        d.input_b_is_active = (v.value & (1 << 2)) !== 0;
        break;
      case 26: d.accel_motion_count = v.value; break;
      default: break; /* unknown field: ignore (forward-compatible) */
    }
  }
  return d;
}

// ---------------------------------------------------------------------------
// Downlink encoding: build a DownlinkCommand (protobuf) on fPort 85 from a
// human-friendly JSON object. Mirrors the dispatcher in app_cmd.c.
//
// Examples (the object passed as input.data):
//   { "command": "get_info", "seq": 1 }
//   { "command": "force_send" }
//   { "command": "clock_sync" }             // empty: network re-sync (LRW or NFC)
//   { "command": "clock_sync", "clock_sync": { "unix_time": 1780000000 } }  // one-shot bootstrap
//   { "command": "reboot" }                 // also settings_save / device_reset
//                                            // (factory_reset/set_secret_key are nfc/shell only —
//                                            // rejected over LoRaWAN, so no downlink for them here)
//   { "command": "reset_counters", "hall_left": true, "input_a": true }
//   { "command": "get_config", "page": 0 }
//   { "command": "req_history", "from_unix": 1780000000, "to_unix": 1780003600 }
//   { "command": "req_history_page", "req_history_page": { "start_ord": 440 } }
//                                            // nfc-only transport — rejected over LoRaWAN too
//   { "command": "get_param", "lorawan_field": [3], "application_field": [4, 7] }
//   { "command": "set_param", "seq": 5,
//     "lorawan": { "adr": true },
//     "application": { "interval_report": 120, "temperature_alarm_hi": 50.0 } }
// ---------------------------------------------------------------------------

function _encVarint(value) {
  var out = [];
  var v = value >>> 0;
  // Handle values above 2^31 (e.g. unix time) via Math, not bit ops.
  if (value > 0xffffffff || value < 0) {
    v = Math.floor(value);
    while (v > 127) { out.push((v % 128) + 128); v = Math.floor(v / 128); }
    out.push(v);
    return out;
  }
  while (v > 127) { out.push((v & 0x7f) | 0x80); v >>>= 7; }
  out.push(v);
  return out;
}

function _encTag(field, wire) {
  return _encVarint((field << 3) | wire);
}

// Manual IEEE-754 float32 little-endian encoder (no DataView — sandbox-safe).
function _encFloat(value) {
  if (value === 0) {
    var negZero = (1 / value) === -Infinity;
    return [0, 0, 0, negZero ? 0x80 : 0];
  }
  var sign = 0;
  if (value < 0) { sign = 1; value = -value; }
  var exp = Math.floor(Math.log(value) / Math.LN2);
  var mant = value / Math.pow(2, exp);
  if (mant < 1) { exp--; mant = value / Math.pow(2, exp); }
  if (mant >= 2) { exp++; mant = value / Math.pow(2, exp); }
  var e = exp + 127, m;
  if (e <= 0) {
    m = Math.round(value / Math.pow(2, -126) * Math.pow(2, 23));
    e = 0;
  } else if (e >= 255) {
    e = 255; m = 0;
  } else {
    m = Math.round((mant - 1) * Math.pow(2, 23));
    if (m === 0x800000) { m = 0; e++; }
  }
  var bits = ((sign << 31) | (e << 23) | m) >>> 0;
  return [bits & 0xff, (bits >>> 8) & 0xff, (bits >>> 16) & 0xff, (bits >>> 24) & 0xff];
}

function _encLenDelim(field, payload) {
  return _encTag(field, 2).concat(_encVarint(payload.length)).concat(payload);
}

function _hexToBytes(hex) {
  var out = [];
  for (var i = 0; i + 1 < hex.length; i += 2) {
    out.push(parseInt(hex.substr(i, 2), 16));
  }
  return out;
}

function _encLorawan(lrw) {
  var out = [];
  for (var name in lrw) {
    if (!lrw.hasOwnProperty(name)) continue;
    var val = lrw[name];
    if (_LRW_HEX_ENC[name] !== undefined) {
      out = out.concat(_encLenDelim(_LRW_HEX_ENC[name], _hexToBytes(val)));
    } else if (_LRW_TAGS[name] !== undefined) {
      var num = val;
      if (typeof val === "string" && _LRW_ENUM[name]) num = _LRW_ENUM[name][val];
      if (typeof val === "boolean") num = val ? 1 : 0;
      out = out.concat(_encTag(_LRW_TAGS[name], 0)).concat(_encVarint(num));
    } else {
      // Unknown field: fail loudly instead of emitting an empty submessage the
      // device silently acks — the operator must know the SetParam did nothing.
      throw new Error("unknown lorawan field: " + name);
    }
  }
  return out;
}

// Generic config-submessage encoder. HEXENC names encode as length-delimited
// hex; FLOAT tags as wire-type 5; everything else varint (bool -> 0/1, symbolic
// enum -> index). Used for application/sensors/alarms.
function _encCfgGroup(obj, TAGS, FLOAT, ENUMS, HEXENC) {
  var out = [];
  for (var name in obj) {
    if (!obj.hasOwnProperty(name)) continue;
    var val = obj[name];
    if (HEXENC && HEXENC[name] !== undefined) {
      out = out.concat(_encLenDelim(HEXENC[name], _hexToBytes(val)));
      continue;
    }
    var tag = TAGS[name];
    if (tag === undefined) {
      // Unknown field: fail loudly instead of emitting an empty submessage the
      // device silently acks — the operator must know the SetParam did nothing.
      throw new Error("unknown config field: " + name);
    }
    if (FLOAT && FLOAT[tag]) {
      out = out.concat(_encTag(tag, 5)).concat(_encFloat(val));
    } else {
      var num = (typeof val === "boolean") ? (val ? 1 : 0) : val;
      if (typeof val === "string" && ENUMS && ENUMS[tag]) {
        var ix = ENUMS[tag].indexOf(val);
        if (ix >= 0) num = ix;
      }
      out = out.concat(_encTag(tag, 0)).concat(_encVarint(num));
    }
  }
  return out;
}

function _encApplication(a) { return _encCfgGroup(a, _APP_TAGS, _APP_FLOAT, _APP_ENUMS, null); }
function _encSensors(s) { return _encCfgGroup(s, _SEN_TAGS, _SEN_FLOAT, _SEN_ENUMS, _SEN_HEX_ENC); }
function _encAlarms(a) { return _encCfgGroup(a, _ALM_TAGS, _ALM_FLOAT, _ALM_ENUMS, _ALM_HEX_ENC); }

function encodeDownlinkCommand(cmd) {
  var out = [];
  if (cmd.seq) out = out.concat(_encTag(1, 0)).concat(_encVarint(cmd.seq));

  var name = cmd.command;
  var tag = _CMD_TAGS[name];
  if (tag === undefined) {
    return { bytes: null, error: "unknown command: " + name };
  }

  // Symmetric with decodeDownlink: the command body lives under cmd[name] —
  // the same nested-oneof shape decode emits. Empty-body commands have no
  // sub-object.
  var b = cmd[name] || {};
  var body = [];
  try {
  if (name === "set_param") {
    if (b.lorawan) body = body.concat(_encLenDelim(1, _encLorawan(b.lorawan)));
    if (b.application) body = body.concat(_encLenDelim(2, _encApplication(b.application)));
    if (b.sensors) body = body.concat(_encLenDelim(4, _encSensors(b.sensors)));
    if (b.alarms) body = body.concat(_encLenDelim(5, _encAlarms(b.alarms)));
    // save (field 3): persist + reboot after applying; set on the LAST message
    // of a multi-downlink batch only.
    if (b.save) body = body.concat(_encTag(3, 0)).concat(_encVarint(1));
  } else if (name === "get_param") {
    // proto3 repeated scalars are packed (length-delimited) by default.
    var _packField = function (arr, tag) {
      if (!arr || !arr.length) return;
      var pl = [];
      for (var i = 0; i < arr.length; i++) pl = pl.concat(_encVarint(arr[i]));
      body = body.concat(_encLenDelim(tag, pl));
    };
    _packField(b.lorawan_field, 1);
    _packField(b.application_field, 2);
    _packField(b.sensors_field, 3);
    _packField(b.alarms_field, 4);
    // page (field 5, #93.3): request page N of an over-budget field list.
    if (b.page) body = body.concat(_encTag(5, 0)).concat(_encVarint(b.page));
  } else if (name === "get_config") {
    if (b.page) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.page));
  } else if (name === "reset_counters") {
    if (b.hall_left) body = body.concat(_encTag(1, 0)).concat(_encVarint(1));
    if (b.hall_right) body = body.concat(_encTag(2, 0)).concat(_encVarint(1));
    if (b.input_a) body = body.concat(_encTag(3, 0)).concat(_encVarint(1));
    if (b.input_b) body = body.concat(_encTag(4, 0)).concat(_encVarint(1));
  } else if (name === "req_history") {
    if (b.from_unix) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.from_unix));
    if (b.to_unix) body = body.concat(_encTag(2, 0)).concat(_encVarint(b.to_unix));
  } else if (name === "clock_sync") {
    // Empty body (LRW): re-sync the RTC from the network (DeviceTimeReq). With
    // unix_time set: one-shot RTC bootstrap (normally an NFC-only use case, but
    // encodable here too since the field is transport-agnostic).
    if (b.unix_time) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.unix_time));
  } else if (name === "req_history_page") {
    // #260 — NFC-only transport (rejected with NOT_READY over LoRaWAN), encoded
    // here so the same builder can produce a hand-injected NFC payload.
    if (b.from_unix) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.from_unix));
    if (b.to_unix) body = body.concat(_encTag(2, 0)).concat(_encVarint(b.to_unix));
    if (b.start_ord) body = body.concat(_encTag(3, 0)).concat(_encVarint(b.start_ord));
  } else if (name === "buzzer_play") {
    if (b.kind) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.kind));
    if (b.repeat_s) body = body.concat(_encTag(2, 0)).concat(_encVarint(b.repeat_s));
  }
  } catch (e) {
    return { bytes: null, error: e.message };
  }
  // get_info / settings_save / reboot / device_reset / force_send / w1_scan:
  // empty body. (factory_reset and set_secret_key are nfc/shell only — never a
  // LoRaWAN downlink — so they have no encoder branch here; sending either by
  // tag/id still round-trips through the command map above for decoding an
  // NFC-side exchange, just not as a buildable downlink.)

  out = out.concat(_encLenDelim(tag, body));
  return { bytes: out, error: null };
}

function encodeDownlink(input) {
  var cmd = input.data || {};
  var r = encodeDownlinkCommand(cmd);
  if (r.error) {
    return { bytes: [], fPort: 85, warnings: [], errors: [r.error] };
  }
  return { bytes: r.bytes, fPort: 85, warnings: [], errors: [] };
}

// Decode a DownlinkCommand (so the LNS shows queued/sent downlinks readably).
function decodeDownlinkCommand(bytes) {
  var cmd = {};
  var pos = 0;
  while (pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3, wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (field === 1) cmd.seq = v.value;
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      var end = pos + len.value;
      cmd.command = _CMD_NAMES[field] || ("field_" + field);
      if (field === 2) { // set_param
        var sp = {}, p = pos;
        while (p < end && p < bytes.length) {
          var t = _pbReadVarint(bytes, p); p = t.next;
          var f2 = t.value >>> 3, w2 = t.value & 0x7;
          if (w2 === 2) {
            var l2 = _pbReadVarint(bytes, p); p = l2.next;
            if (f2 === 1) sp.lorawan = _decodeLorawan(bytes, p, p + l2.value);
            else if (f2 === 2) sp.application = _decodeApplication(bytes, p, p + l2.value);
            else if (f2 === 4) sp.sensors = _decodeSensors(bytes, p, p + l2.value);
            else if (f2 === 5) sp.alarms = _decodeAlarms(bytes, p, p + l2.value);
            p += l2.value;
          } else if (w2 === 0) {
            var sv = _pbReadVarint(bytes, p); p = sv.next;
            if (f2 === 3) sp.save = sv.value !== 0; // persist + reboot after apply
          } else { break; }
        }
        cmd.set_param = sp;
      } else if (field === 3) { // get_param (repeated uint32, packed or not)
        var gp = { lorawan_field: [], application_field: [], sensors_field: [], alarms_field: [] },
            q = pos;
        while (q < end && q < bytes.length) {
          var t3 = _pbReadVarint(bytes, q); q = t3.next;
          var f3 = t3.value >>> 3, w3 = t3.value & 0x7;
          var dst = (f3 === 1) ? gp.lorawan_field : (f3 === 2) ? gp.application_field
                  : (f3 === 3) ? gp.sensors_field : (f3 === 4) ? gp.alarms_field : null;
          if (w3 === 0) {
            var v3 = _pbReadVarint(bytes, q); q = v3.next;
            if (f3 === 5) gp.page = v3.value; // #93.3 pagination cursor
            else if (dst) dst.push(v3.value);
          } else if (w3 === 2) {
            var pl3 = _pbReadVarint(bytes, q); q = pl3.next;
            var e3 = q + pl3.value;
            while (q < e3 && q < bytes.length) { var pv = _pbReadVarint(bytes, q); q = pv.next; if (dst) dst.push(pv.value); }
          } else { break; }
        }
        ["lorawan_field", "application_field", "sensors_field", "alarms_field"].forEach(
          function (k) { if (gp[k].length === 0) delete gp[k]; });
        cmd.get_param = gp;
      } else if (field === 5) { // get_config
        var gc = {}, r2 = pos;
        if (r2 < end) {
          var t5 = _pbReadVarint(bytes, r2); r2 = t5.next;
          if ((t5.value >>> 3) === 1 && (t5.value & 0x7) === 0) {
            var v5 = _pbReadVarint(bytes, r2); gc.page = v5.value;
          }
        }
        cmd.get_config = gc;
      } else if (field === 10) { // reset_counters
        var rc = {}, s = pos;
        while (s < end && s < bytes.length) {
          var t10 = _pbReadVarint(bytes, s); s = t10.next;
          var f10 = t10.value >>> 3, w10 = t10.value & 0x7;
          if (w10 === 0) {
            var v10 = _pbReadVarint(bytes, s); s = v10.next;
            if (f10 === 1) rc.hall_left = v10.value !== 0;
            else if (f10 === 2) rc.hall_right = v10.value !== 0;
            else if (f10 === 3) rc.input_a = v10.value !== 0;
            else if (f10 === 4) rc.input_b = v10.value !== 0;
          } else { break; }
        }
        cmd.reset_counters = rc;
      } else if (field === 11) { // req_history
        var rh = {}, u = pos;
        while (u < end && u < bytes.length) {
          var t11 = _pbReadVarint(bytes, u); u = t11.next;
          var f11 = t11.value >>> 3, w11 = t11.value & 0x7;
          if (w11 === 0) {
            var v11 = _pbReadVarint(bytes, u); u = v11.next;
            if (f11 === 1) rh.from_unix = v11.value;
            else if (f11 === 2) rh.to_unix = v11.value;
          } else { break; }
        }
        cmd.req_history = rh;
      } else if (field === 12) { // clock_sync (empty = network re-sync; unix_time = NFC bootstrap)
        var cs = {}, cp = pos;
        while (cp < end && cp < bytes.length) {
          var t12 = _pbReadVarint(bytes, cp); cp = t12.next;
          var f12 = t12.value >>> 3, w12 = t12.value & 0x7;
          if (w12 === 0) {
            var v12 = _pbReadVarint(bytes, cp); cp = v12.next;
            if (f12 === 1) cs.unix_time = v12.value;
          } else { break; }
        }
        cmd.clock_sync = cs;
      } else if (field === 13) { // req_history_page (#260, NFC-only transport)
        var rp = {}, pp = pos;
        while (pp < end && pp < bytes.length) {
          var t13 = _pbReadVarint(bytes, pp); pp = t13.next;
          var f13 = t13.value >>> 3, w13 = t13.value & 0x7;
          if (w13 === 0) {
            var v13 = _pbReadVarint(bytes, pp); pp = v13.next;
            if (f13 === 1) rp.from_unix = v13.value;
            else if (f13 === 2) rp.to_unix = v13.value;
            else if (f13 === 3) rp.start_ord = v13.value;
          } else { break; }
        }
        cmd.req_history_page = rp;
      }
      pos = end;
    } else {
      break;
    }
  }
  return cmd;
}

function decodeDownlink(input) {
  if (input.fPort === 85) {
    return { data: decodeDownlinkCommand(input.bytes), warnings: [], errors: [] };
  }
  return { data: { bytes_hex: _pbBytesToHex(input.bytes) }, warnings: [], errors: [] };
}

// fPort 3: alarm-detail batch, protobuf AlarmReport. Top-level: base_time(1),
// total(2), repeated AlarmEvent events(3). AlarmEvent (dynamic alarm rule):
// source(1), edge(2), rel_s(4) varints + optional sint32 value(5) + quantity(6) +
// slot(7) + type(9). source = enum app_alarm_source, quantity = enum
// app_alarm_quantity; type says WHAT fired (low/high/trigger/no_data) and edge
// the rising/falling transition — orthogonal (#212). value is scaled per quantity
// (temp/hum ×100, pressure ×10, magnetic_field µT, digital 0/1, counter) and
// absent for some edges. Per-event time = base_time + rel_s. `total` may exceed
// events present (dropped to fit the data rate).
var _ALARM_SOURCES = ["onboard", "s1", "s2", "s3", "s4", "hall-left", "hall-right",
  "input-a", "input-b", "pir", "accel", "battery"];
var _ALARM_QUANTITIES = ["temperature", "humidity", "pressure", "illuminance",
  "magnetic-field", "tilt", "state", "count", "voltage"];
var _ALARM_EDGES = ["activate", "deactivate"];
var _ALARM_TYPES = ["none", "low", "high", "trigger", "no_data"];

function _alarmUnscale(quantity, raw) {
  switch (quantity) {
    case 0: case 1: return raw / 100;   // temperature, humidity
    case 2: return raw / 10;            // pressure (hPa×10)
    case 4: return raw / 1000;          // magnetic-field (µT -> mT)
    case 8: return raw / 100;           // voltage (V×100)
    default: return raw;                // illuminance / state / count
  }
}

// Response.Info.active_alarms entry (#288): a live alarm snapshot trimmed to
// source(1)/quantity(2)/type(3) — no slot/value/edge/time (that is the fPort 3
// AlarmEvent). Reuses the fPort 3 enum name maps for a consistent JSON shape.
function _decodeAlarmStatus(bytes, start, end) {
  var a = { source: 0, quantity: 0, type: 0 };
  var p = start;
  while (p < end && p < bytes.length) {
    var t = _pbReadVarint(bytes, p); p = t.next;
    var field = t.value >>> 3, wire = t.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, p); p = v.next;
      if (field === 1) a.source = v.value;
      else if (field === 2) a.quantity = v.value;
      else if (field === 3) a.type = v.value;
    } else if (wire === 2) {
      var l = _pbReadVarint(bytes, p); p = l.next + l.value;
    } else { break; }
  }
  return {
    source: _ALARM_SOURCES[a.source] || ("src" + a.source),
    quantity: _ALARM_QUANTITIES[a.quantity] || ("q" + a.quantity),
    type: _ALARM_TYPES[a.type] || "none",
  };
}

function _decodeAlarmEvent(bytes, start, end) {
  var ev = { slot: 0, source: 0, quantity: 0, edge: 0, type: 0, rel_s: 0, value: null };
  var p = start;
  while (p < end && p < bytes.length) {
    var t = _pbReadVarint(bytes, p); p = t.next;
    var field = t.value >>> 3, wire = t.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, p); p = v.next;
      if (field === 1) ev.source = v.value;
      else if (field === 2) ev.edge = v.value;
      else if (field === 4) ev.rel_s = v.value;
      else if (field === 5) ev.value = _pbZigzag(v.value);
      else if (field === 6) ev.quantity = v.value;
      else if (field === 7) ev.slot = v.value;
      else if (field === 9) ev.type = v.value;
    } else if (wire === 2) {
      var l = _pbReadVarint(bytes, p); p = l.next + l.value;
    } else { break; }
  }
  return ev;
}

function decodeAlarmBatch(bytes) {
  // time_synced (field 4) absent = old FW = treat as synced (emit timestamps).
  var out = { base_time: 0, total: 0, time_synced: true, alarms: [] };
  var rels = [];
  var pos = 0;
  while (pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3, wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (field === 1) out.base_time = v.value >>> 0;
      else if (field === 2) out.total = v.value;
      else if (field === 4) out.time_synced = v.value !== 0;
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      var endE = pos + len.value;
      if (field === 3) {
        var ev = _decodeAlarmEvent(bytes, pos, endE);
        out.alarms.push({
          slot: ev.slot,
          source: _ALARM_SOURCES[ev.source] || ("src" + ev.source),
          quantity: _ALARM_QUANTITIES[ev.quantity] || ("q" + ev.quantity),
          event: _ALARM_EDGES[ev.edge] || "activate",
          type: _ALARM_TYPES[ev.type] || "none",
          value: ev.value === null ? null : _alarmUnscale(ev.quantity, ev.value),
          time: 0,
        });
        rels.push(ev.rel_s);
      }
      pos = endE;
    } else { break; }
  }
  // base_time (field 1) precedes events (field 3) on the wire, but resolve the
  // per-event times after the loop so order can't bite us. time=null when the
  // clock had not synced at window start (L-3/L-4): base_time is uptime-relative.
  for (var i = 0; i < out.alarms.length; i++) {
    out.alarms[i].time = out.time_synced ? ((out.base_time + rels[i]) >>> 0) : null;
  }
  out.truncated = out.alarms.length < out.total; // some alarms dropped to fit the DR
  return out;
}

// 1-byte format version prefixed to application protobuf payloads (fPort 2
// telemetry, fPort 3 alarm report, fPort 85 response). Mirrors APP_PROTO_VERSION
// in app_cmd.h.
var _PROTO_VERSION = 0x01;

// Strip + validate the version prefix at byte[0]. Returns the protobuf bytes
// (byte 1..end) and pushes a warning on an unexpected version (the remainder is
// still decoded best-effort). fPort 1 (legacy bitmap) and fPort 10 (calibration)
// are unversioned.
function _stripProtoVersion(bytes, warnings) {
  if (!bytes || bytes.length < 1) return bytes;
  if (bytes[0] !== _PROTO_VERSION) {
    warnings.push("unknown payload version 0x" + bytes[0].toString(16));
  }
  return bytes.slice(1);
}

function decodeUplink(input) {

  // fPort 85: DownlinkResponse (Ack / Info / Error from command dispatcher).
  if (input.fPort === 85) {
    var w85 = [];
    var b85 = _stripProtoVersion(input.bytes, w85);
    return {
      data: decodeDownlinkResponse(b85),
      warnings: w85,
      errors: []
    };
  }

  // fPort 3: alarm-detail batch (#27), version-prefixed like fPort 2/85 (#165).
  if (input.fPort === 3) {
    var w3 = [];
    var b3 = _stripProtoVersion(input.bytes, w3);
    return { data: decodeAlarmBatch(b3), warnings: w3, errors: [] };
  }

  // fPort 2: protobuf Telemetry (new format). fPort 1 stays the legacy bitmap.
  if (input.fPort === 2) {
    var w2 = [];
    var b2 = _stripProtoVersion(input.bytes, w2);
    return {
      data: decodeTelemetry(b2),
      warnings: w2,
      errors: []
    };
  }

  function toSignedInt16(value) {
    return value > 0x7fff ? value - 0x10000 : value;
  }

  var data = {};
  var bytes = input.bytes;
  var index = 0;
  var isExtendedPacket = false;
  var header = 0;

  if (bytes.length >= 4) {
    header = (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
    if (header & (1 << 20)) {
      isExtendedPacket = true;
      index = 4;
    }
  }

  if (!isExtendedPacket) {
    header = (bytes[0] << 8) | bytes[1];
    index = 2;
  }

  var orientation = header & 0xf;

  if (isExtendedPacket ? (header & (1 << 31)) : (header & (1 << 15))) {
    data.boot = true;
  } else {
    data.boot = false;
  }

  if (isExtendedPacket ? (header & (1 << 30)) : (header & (1 << 14))) {
    data.orientation = orientation === 0xf ? null : orientation;
  } else {
    data.orientation = null;
  }

  if (isExtendedPacket ? (header & (1 << 29)) : (header & (1 << 13))) {
    var voltage = bytes[index++];
    data.voltage = voltage === 0xff ? null : voltage / 50;
  } else {
    data.voltage = null;
  }

  if (isExtendedPacket ? (header & (1 << 28)) : (header & (1 << 12))) {
    var temperature = (bytes[index++] << 8) | bytes[index++];
    data.temperature = temperature === 0x7fff ? null : toSignedInt16(temperature) / 100;
  } else {
    data.temperature = null;
  }

  if (isExtendedPacket ? (header & (1 << 27)) : (header & (1 << 11))) {
    var humidity = bytes[index++];
    data.humidity = humidity === 0xff ? null : humidity / 2;
  } else {
    data.humidity = null;
  }

  if (isExtendedPacket ? (header & (1 << 26)) : (header & (1 << 10))) {
    var illuminance = (bytes[index++] << 8) | bytes[index++];
    data.illuminance = illuminance === 0xffff ? null : illuminance * 2;
  } else {
    data.illuminance = null;
  }

  if (isExtendedPacket ? (header & (1 << 25)) : (header & (1 << 9))) {
    var ext_temperature_1 = (bytes[index++] << 8) | bytes[index++];
    data.ext_temperature_1 = ext_temperature_1 === 0x7fff ? null : toSignedInt16(ext_temperature_1) / 100;
  } else {
    data.ext_temperature_1 = null;
  }

  if (isExtendedPacket ? (header & (1 << 24)) : (header & (1 << 8))) {
    var ext_temperature_2 = (bytes[index++] << 8) | bytes[index++];
    data.ext_temperature_2 = ext_temperature_2 === 0x7fff ? null : toSignedInt16(ext_temperature_2) / 100;
  } else {
    data.ext_temperature_2 = null;
  }

  if (isExtendedPacket ? (header & (1 << 23)) : (header & (1 << 7))) {
    var motion_count = (bytes[index++] << 24) | (bytes[index++] << 16) | (bytes[index++] << 8) | bytes[index++];
    data.motion_count = motion_count === 0xffffffff ? null : motion_count;
  } else {
    data.motion_count = null;
  }

  if (isExtendedPacket ? (header & (1 << 22)) : (header & (1 << 6))) {
    var altitude = (bytes[index++] << 8) | bytes[index++];
    data.altitude = altitude === 0x7fff ? null : toSignedInt16(altitude) / 10;
  } else {
    data.altitude = null;
  }

  if (isExtendedPacket ? (header & (1 << 21)) : (header & (1 << 5))) {
    var pressure = (bytes[index++] << 24) | (bytes[index++] << 16) | (bytes[index++] << 8) | bytes[index++];
    data.pressure = pressure === 0xffffffff ? null : pressure;
  } else {
    data.pressure = null;
  }

  if (isExtendedPacket) {
    if (header & (1 << 19)) {
      var machine_probe_temperature_1 = (bytes[index++] << 8) | bytes[index++];
      data.machine_probe_temperature_1 =
        machine_probe_temperature_1 === 0x7fff
          ? null
          : toSignedInt16(machine_probe_temperature_1) / 100;
    } else {
      data.machine_probe_temperature_1 = null;
    }

    if (header & (1 << 18)) {
      var machine_probe_temperature_2 = (bytes[index++] << 8) | bytes[index++];
      data.machine_probe_temperature_2 =
        machine_probe_temperature_2 === 0x7fff
          ? null
          : toSignedInt16(machine_probe_temperature_2) / 100;
    } else {
      data.machine_probe_temperature_2 = null;
    }

    if (header & (1 << 17)) {
      var machine_probe_humidity_1 = bytes[index++];
      data.machine_probe_humidity_1 = machine_probe_humidity_1 === 0xff ? null : machine_probe_humidity_1 / 2;
    } else {
      data.machine_probe_humidity_1 = null;
    }

    if (header & (1 << 16)) {
      var machine_probe_humidity_2 = bytes[index++];
      data.machine_probe_humidity_2 = machine_probe_humidity_2 === 0xff ? null : machine_probe_humidity_2 / 2;
    } else {
      data.machine_probe_humidity_2 = null;
    }

    if (header & (1 << 15)) {
      data.machine_probe_tilt_alert_1 = true;
    } else {
      data.machine_probe_tilt_alert_1 = false;
    }
    if (header & (1 << 14)) {
      data.machine_probe_tilt_alert_2 = true;
    } else {
      data.machine_probe_tilt_alert_2 = false;
    }

    if (header & (1 << 13)) {
      var hall_left_count = (bytes[index++] << 24) | (bytes[index++] << 16) | (bytes[index++] << 8) | bytes[index++];
      data.hall_left_count = hall_left_count;
    } else {
      data.hall_left_count = null;
    }

    if (header & (1 << 12)) {
      var hall_right_count = (bytes[index++] << 24) | (bytes[index++] << 16) | (bytes[index++] << 8) | bytes[index++];
      data.hall_right_count = hall_right_count;
    } else {
      data.hall_right_count = null;
    }

    data.hall_left_notify_act = (header & (1 << 11)) ? true : false;
    data.hall_left_notify_deact = (header & (1 << 10)) ? true : false;
    data.hall_right_notify_act = (header & (1 << 9)) ? true : false;
    data.hall_right_notify_deact = (header & (1 << 8)) ? true : false;

    data.hall_left_is_active = (header & (1 << 7)) ? true : false;
    data.hall_right_is_active = (header & (1 << 6)) ? true : false;

    if (header & (1 << 5)) {
      var input_a_count = (bytes[index++] << 24) | (bytes[index++] << 16) | (bytes[index++] << 8) | bytes[index++];
      data.input_a_count = input_a_count;
      var status_a = bytes[index++];
      data.input_a_notify_act = (status_a & (1 << 3)) ? true : false;
      data.input_a_notify_deact = (status_a & (1 << 2)) ? true : false;
      data.input_a_is_active = (status_a & (1 << 0)) ? true : false;
    } else {
      data.input_a_count = null;
      data.input_a_notify_act = false;
      data.input_a_notify_deact = false;
      data.input_a_is_active = false;
    }

    if (header & (1 << 4)) {
      var input_b_count = (bytes[index++] << 24) | (bytes[index++] << 16) | (bytes[index++] << 8) | bytes[index++];
      data.input_b_count = input_b_count;
      var status_b = bytes[index++];
      data.input_b_notify_act = (status_b & (1 << 3)) ? true : false;
      data.input_b_notify_deact = (status_b & (1 << 2)) ? true : false;
      data.input_b_is_active = (status_b & (1 << 0)) ? true : false;
    } else {
      data.input_b_count = null;
      data.input_b_notify_act = false;
      data.input_b_notify_deact = false;
      data.input_b_is_active = false;
    }
  }

  return {
    data: data,
    warnings: [],
    errors: []
  };
}

// Make the codec importable from Node (tests) without affecting the
// TTN/ChirpStack sandbox, which has no `module`. No ES6 shorthand properties
// here — see the ES5-only note at the top of the file.
if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    decodeUplink: decodeUplink,
    encodeDownlink: encodeDownlink,
    decodeDownlink: decodeDownlink,
    Decode: Decode,
    decodeTelemetry: decodeTelemetry,
    decodeDownlinkResponse: decodeDownlinkResponse,
    decodeAlarmBatch: decodeAlarmBatch
  };
}
