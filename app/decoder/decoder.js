// STICKER LoRaWAN payload codec (TTN / TTS / ChirpStack). Docs: app/decoder/README.md.
// Upload decoder.min.js (npm run build); the regions between BEGIN/END GENERATED
// are written by west configen / west sensorgen.
function Decode(fPort, bytes, variables) {
  return decodeUplink({ fPort: fPort, bytes: bytes }).data;
}
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
var _DEVICE_STATUS = [
  [1 << 0, "alarm_any"],
  [1 << 1, "alarm_threshold"],
  [1 << 2, "alarm_state"],
  [1 << 3, "alarm_rate"],
  [1 << 4, "alarm_no_data"],
  [1 << 5, "alarm_low_battery"],
  [1 << 6, "alarm_sensor_mismatch"],
  [1 << 8, "radio_off"],
  [1 << 9, "lrw_disabled"],
  [1 << 10, "radio_link_down"],
  [1 << 12, "nfc_down"],
  [1 << 14, "i2c_wedged"],
  [1 << 15, "history_down"],
  [1 << 16, "time_unsynced"],
  [1 << 17, "claim_active"],
];
var _RESET_CAUSES = [
  [1 << 0, "pin"],
  [1 << 1, "software"],
  [1 << 2, "brownout"],
  [1 << 3, "power_on"],
  [1 << 4, "watchdog"],
  [1 << 6, "security"],
  [1 << 7, "low_power_wake"],
];
var _FAULT_REASONS = {
  0: "cpu_exception", 1: "spurious_irq", 2: "stack_check_fail", 3: "kernel_oops",
  4: "kernel_panic",
  16: "mem_fault", 17: "mem_stacking", 18: "mem_unstacking", 19: "mem_data_access",
  20: "mem_instruction_access", 21: "mem_fp_lazy_state",
  22: "bus_fault", 23: "bus_stacking", 24: "bus_unstacking", 25: "bus_precise_data",
  26: "bus_imprecise_data", 27: "bus_instruction", 28: "bus_fp_lazy_state",
  29: "usage_fault", 30: "div_0", 31: "unaligned_access", 32: "stack_overflow",
  33: "no_coprocessor", 34: "illegal_exc_return", 35: "illegal_epsr",
  36: "undefined_instruction"
};
var _APP_NAMES = {
  1: "calibration", 2: "interval_sample", 3: "interval_report",
  4: "history_enable", 6: "battery_level",
  7: "vendor_reset_allow", 8: "interval_announce"
};
var _APP_HEX = { 9: "history_channels" };
var _APP_HEX_ENC = { history_channels: 9 };
var _APP_ENUMS = {};
var _APP_FLOAT = {};
var _SEN_NAMES = {
  1: "cap_hall_left", 2: "cap_hall_right",
  5: "cap_light_sensor", 6: "cap_barometer",
  8: "cap_w1_sensors", 9: "cap_accelerometer", 10: "accel_motion_sensitivity",
  15: "hall_left_counter", 16: "hall_right_counter",
  17: "input_a_counter", 18: "input_b_counter", 22: "cap_sht",
  23: "sensor1_type", 24: "sensor2_type", 25: "sensor3_type", 26: "sensor4_type",
  27: "cap_gpio_mode", 28: "cap_gpio_input_a", 29: "cap_gpio_input_b"
};
var _SEN_ENUMS = {
  10: ["off", "low", "medium", "high"],
  27: ["none", "inputs", "pir", "buzzer"],
  28: ["none", "digital", "analog", "pulse"],
  29: ["none", "digital", "analog", "pulse"]
};
var _SEN_FLOAT = {};
var _SEN_HEX = { 11: "sensor1_rom", 12: "sensor2_rom", 13: "sensor3_rom", 14: "sensor4_rom" };
var _SEN_HEX_ENC = { sensor1_rom: 11, sensor2_rom: 12, sensor3_rom: 13, sensor4_rom: 14 };
var _ALM_NAMES = {
  1: "alarm_limit", 20: "alarm_buzzer_mode", 21: "radio_alarm_ack",
  22: "event_log_enable", 23: "alarm_send_detail", 24: "alarm_send_report", 27: "log_split", 28: "event_format",
  25: "event_flush_count", 26: "event_flush_interval"
};
var _ALM_ENUMS = {
  20: ["off", "once", "slow", "normal", "fast", "continuous", "reserved6", "reserved7"],
  28: ["full", "compact", "raw"]
};
var _ALM_FLOAT = {};
var _ALM_HEX = {};
var _ALM_HEX_ENC = {};
(function () {
  for (var i = 0; i < 16; i++) { _ALM_HEX[3 + i] = "alarm_" + i; _ALM_HEX_ENC["alarm_" + i] = 3 + i; }
})();
var _LRW_NAMES = {
  1: "region", 2: "sub_band", 3: "network", 4: "adr", 5: "activation",
  13: "radio_link_check_interval", 14: "radio_link_check_fail_rejoin", 15: "radio_mode",
  16: "datarate"
};
var _LRW_HEX = { 6: "deveui", 7: "joineui", 10: "devaddr" };
var _P2P_NAMES = { 1: "frequency", 2: "spreading_factor", 3: "tx_power", 4: "modulation" };
var _P2P_ENUMS = { 4: ["lora", "fsk"] };
var _P2P_FLOAT = {};
var _LRW_HEX_ENC = { deveui: 6, joineui: 7, nwkkey: 8, appkey: 9, devaddr: 10, nwkskey: 11, appskey: 12 };
var _LRW_ENUM = {
  region: { EU868: 0, US915: 1, AU915: 2, AS923: 3 },
  network: { PUBLIC: 0, PRIVATE: 1 },
  activation: { OTAA: 0, ABP: 1 },
  radio_mode: { OFF: 0, LORAWAN: 1, P2P: 2 },
  datarate: { AUTO: 0, DR0: 1, DR1: 2, DR2: 3, DR3: 4, DR4: 5, DR5: 6, DR6: 7, DR7: 8 }
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
var _LRW_TAG_ALIASES = { link_check_interval: 13, link_check_fail_rejoin: 14 };
for (var _a in _LRW_TAG_ALIASES) {
  if (_LRW_TAG_ALIASES.hasOwnProperty(_a)) _LRW_TAGS[_a] = _LRW_TAG_ALIASES[_a];
}
var _P2P_TAGS = _invert(_P2P_NAMES);
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
  25: "claim_done",
  26: "vendor_reset",
  27: "claim_active",
  28: "buzzer_play",
  29: "get_claim_info",
  30: "get_basic_info",
  31: "get_settings",
  32: "get_radio_state",
  33: "req_events",
  34: "req_events_page",
};
// END GENERATED COMMANDS
var _CMD_TAGS = _invert(_CMD_NAMES);
// BEGIN GENERATED SENSOR_TYPES
// Sensor type registry, generated by `west sensorgen` from app_sensor_types.yaml
// (#430). type id -> { name, ch: [channel] }; a channel is
// { n: name, u: unit, k: kind, s: wire scale, h: [history enc, scale] | null,
//   p: part (1-Wire chip) }.
// Slot 0 is always type 1 (motherboard); slots 1..4 carry their type on the wire.
var _SENSOR_TYPES = {
  1: {
    name: "motherboard",
    ch: [
      { n: "temperature", u: "degC", k: "threshold", s: 100, h: ["i16", 100] },
      { n: "humidity", u: "%RH", k: "threshold", s: 2, h: ["u8", 2] },
      { n: "pressure", u: "hPa", k: "threshold", s: 10, h: ["u16", 10] },
      { n: "illuminance", u: "lx", k: "threshold", s: 1, h: ["u16", 0.5] },
      { n: "temperature-baro", u: "degC", k: "threshold", s: 100, h: ["i16", 100] },
      { n: "hall-left-state", u: "bool", k: "state", s: 1, h: ["u8", 1] },
      { n: "hall-left-count", u: "pulses", k: "rate", s: 1, h: ["u32", 1] },
      { n: "hall-right-state", u: "bool", k: "state", s: 1, h: ["u8", 1] },
      { n: "hall-right-count", u: "pulses", k: "rate", s: 1, h: ["u32", 1] },
      { n: "input-a-state", u: "bool", k: "state", s: 1, h: ["u8", 1] },
      { n: "input-a-count", u: "pulses", k: "rate", s: 1, h: ["u32", 1] },
      { n: "input-a-voltage", u: "V", k: "threshold", s: 1000, h: ["u16", 1000] },
      { n: "input-b-state", u: "bool", k: "state", s: 1, h: ["u8", 1] },
      { n: "input-b-count", u: "pulses", k: "rate", s: 1, h: ["u32", 1] },
      { n: "input-b-voltage", u: "V", k: "threshold", s: 1000, h: ["u16", 1000] },
      { n: "pir-motion", u: "bool", k: "state", s: 1, h: ["u8", 1] },
      { n: "pir-count", u: "events", k: "rate", s: 1, h: ["u32", 1] },
      { n: "accel-motion", u: "bool", k: "state", s: 1, h: ["u8", 1] },
      { n: "accel-count", u: "events", k: "rate", s: 1, h: ["u32", 1] },
      { n: "accel-orientation", u: "enum", k: "none", s: 1, h: ["u8", 1] },
      { n: "battery-voltage", u: "V", k: "threshold", s: 1000, h: ["u16", 1000] },
      { n: "altitude", u: "m", k: "none", s: 10, h: ["i16", 1] }
    ]
  },
  2: {
    name: "dallas",
    ch: [
      { n: "temperature", u: "degC", k: "threshold", s: 100, h: ["i16", 100], p: "ds18b20" }
    ]
  },
  3: {
    name: "machine-probe",
    ch: [
      { n: "temperature", u: "degC", k: "threshold", s: 100, h: ["i16", 100], p: "sht" },
      { n: "humidity", u: "%RH", k: "threshold", s: 2, h: ["u8", 2], p: "sht" },
      { n: "temperature-aux", u: "degC", k: "threshold", s: 100, h: ["i16", 100], p: "tmp112" },
      { n: "illuminance", u: "lx", k: "threshold", s: 1, h: ["u16", 0.5], p: "opt3001" },
      { n: "magnetic-field", u: "mT", k: "threshold", s: 1000, h: ["i16", 100], p: "si7210" },
      { n: "tilt", u: "bool", k: "state", s: 1, h: ["u8", 1], p: "lis2dh12" },
      { n: "accel-x", u: "m/s2", k: "threshold", s: 100, h: ["i16", 100], p: "lis2dh12" },
      { n: "accel-y", u: "m/s2", k: "threshold", s: 100, h: ["i16", 100], p: "lis2dh12" },
      { n: "accel-z", u: "m/s2", k: "threshold", s: 100, h: ["i16", 100], p: "lis2dh12" }
    ]
  }
};
var _SENSOR_MB_TYPE = 1;
// History encodings: record bytes and the "absent" sentinel as stored (#485 S10).
var _HIST_ENC_SIZE = { u8: 1, i16: 2, u16: 2, i32: 4, u32: 4 };
var _HIST_ENC_SENTINEL = { u8: 0xff, i16: 0x7fff, u16: 0xffff, i32: 0x7fffffff, u32: 0xffffffff };
// END GENERATED SENSOR_TYPES
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
function _decodeApplication(b, s, e) { return _decodeCfgGroup(b, s, e, _APP_NAMES, _APP_ENUMS, _APP_HEX); }
function _decodeSensors(b, s, e) { return _decodeCfgGroup(b, s, e, _SEN_NAMES, _SEN_ENUMS, _SEN_HEX); }
function _decodeAlarms(b, s, e) { return _decodeCfgGroup(b, s, e, _ALM_NAMES, _ALM_ENUMS, _ALM_HEX); }
function _decodeP2P(b, s, e) { return _decodeCfgGroup(b, s, e, _P2P_NAMES, _P2P_ENUMS, null); }
function _decodeConfigDump(bytes, start, end) {
  var cd = {}, pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var f = tag.value >>> 3, w = tag.value & 0x7;
    if (w === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (f === 1) cd.page_index = v.value;
      else if (f === 2) cd.page_count = v.value;
      else if (f === 7) { (cd.w1_slot_type = cd.w1_slot_type || []).push(_W1_SLOT_TYPES[v.value] || ("type" + v.value)); }
    } else if (w === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      var e2 = pos + len.value;
      if (f === 3) cd.lorawan = _decodeLorawan(bytes, pos, e2);
      else if (f === 4) cd.application = _decodeApplication(bytes, pos, e2);
      else if (f === 5) cd.sensors = _decodeSensors(bytes, pos, e2);
      else if (f === 6) cd.alarms = _decodeAlarms(bytes, pos, e2);
      else if (f === 7) {
        cd.w1_slot_type = [];
        var p = pos;
        while (p < e2) {
          var t = _pbReadVarint(bytes, p); p = t.next;
          cd.w1_slot_type.push(_W1_SLOT_TYPES[t.value] || ("type" + t.value));
        }
      }
      else if (f === 8) cd.p2p = _decodeP2P(bytes, pos, e2);
      pos = e2;
    } else { break; }
  }
  return cd;
}
function _decodeInfo(bytes, start, end) {
  var info = { fw_major: 0, fw_minor: 0, fw_patch: 0, build_type: 0, debug: false, device_status: 0, active_alarms: [] };
  var seen = {};
  var pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    seen[field] = true;
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
      else if (field === 10) info.battery = v.value;
      else if (field === 11) info.reset_cause = v.value;
      else if (field === 14) info.device_status = v.value;
      else if (field === 20) info.fault_reason = v.value;
      else if (field === 21) info.fault_pc = "0x" + ("0000000" + (v.value >>> 0).toString(16)).slice(-8);
      else if (field === 22) info.fault_lr = "0x" + ("0000000" + (v.value >>> 0).toString(16)).slice(-8);
      else if (field === 19) (info.w1_slot_state = info.w1_slot_state || []).push(_W1_SLOT_STATES[v.value] || ("state" + v.value));
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      if (field === 9) info.claim_token = _pbBytesToHex(bytes.slice(pos, pos + len.value));
      else if (field === 13) info.dev_eui = _pbBytesToHex(bytes.slice(pos, pos + len.value));
      else if (field === 15) info.active_alarms.push(_decodeAlarmStatus(bytes, pos, pos + len.value));
      else if (field === 19) {
        info.w1_slot_state = [];
        for (var sp = pos; sp < pos + len.value;) {
          var st = _pbReadVarint(bytes, sp); sp = st.next;
          info.w1_slot_state.push(_W1_SLOT_STATES[st.value] || ("state" + st.value));
        }
      }
      pos += len.value;
    } else {
      break;
    }
  }
  info.fw_version = info.fw_major + "." + info.fw_minor + "." + info.fw_patch;
  info.build_type_name = _BUILD_TYPES[info.build_type] || "unknown";
  info.device_status_flags = _DEVICE_STATUS
    .filter(function (f) { return (info.device_status & f[0]) !== 0; })
    .map(function (f) { return f[1]; });
  if (info.reset_cause !== undefined) {
    info.reset_cause_flags = _RESET_CAUSES
      .filter(function (f) { return (info.reset_cause & f[0]) !== 0; })
      .map(function (f) { return f[1]; });
  }
  if (info.fault_reason !== undefined) {
    info.fault_reason_name = _FAULT_REASONS[info.fault_reason] || ("reason" + info.fault_reason);
  }
  Object.defineProperty(info, "_seen", { value: seen, enumerable: false, configurable: true });
  return info;
}
var _INFO_FIELD_KEYS = {
  1: ["fw_major"], 2: ["fw_minor"], 3: ["fw_patch"], 4: ["build_type", "build_type_name"],
  5: ["serial_number"], 6: ["uptime_s"], 7: ["unix_time"], 8: ["debug"], 9: ["claim_token"],
  10: ["battery"], 11: ["reset_cause", "reset_cause_flags"],
  13: ["dev_eui"], 14: ["device_status", "device_status_flags"], 15: ["active_alarms"],
  20: ["fault_reason", "fault_reason_name"], 21: ["fault_pc"], 22: ["fault_lr"],
  19: ["w1_slot_state"]
};
function _pruneInfoPage(info) {
  var seen = info._seen || {};
  var keep = {};
  for (var f in _INFO_FIELD_KEYS) {
    if (seen[f]) _INFO_FIELD_KEYS[f].forEach(function (k) { keep[k] = true; });
  }
  if (seen[1] || seen[2] || seen[3]) keep.fw_version = true;
  var out = {};
  for (var k in info) { if (info.hasOwnProperty(k) && keep[k]) out[k] = info[k]; }
  return out;
}
var _RADIO_STATE_FIELDS = {
  1: ["state", false], 2: ["sf", false], 3: ["datarate", false], 4: ["tx_power_dbm", true],
  5: ["dl_rssi", true], 6: ["dl_snr", true], 7: ["dl_age_s", false], 8: ["dl_unix_time", false],
  9: ["ul_rssi", true], 10: ["ul_snr", true], 11: ["ul_margin", false], 12: ["ul_gw_count", false],
  13: ["dev_addr", false], 14: ["fcnt_up", false], 15: ["fail_streak", false],
  16: ["join_attempts", false], 17: ["duty_blocked_s", false], 18: ["airtime_hour_ms", false],
  19: ["uptime_s", false], 20: ["tx_count", false], 21: ["rx_count", false],
  22: ["retry_count", false], 23: ["fail_count", false], 24: ["tx_err_count", false],
  25: ["join_count", false]
};
function _decodeRadioState(bytes, start, end) {
  var rs = {};
  var pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      var f = _RADIO_STATE_FIELDS[field];
      if (f) rs[f[0]] = f[1] ? _pbZigzag(v.value) : v.value;
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next + len.value;
    } else {
      break;
    }
  }
  if (rs.state !== undefined) rs.state_name = _LRW_STATES[rs.state] || "unknown";
  if (rs.dev_addr !== undefined) rs.dev_addr_hex = ("0000000" + rs.dev_addr.toString(16)).slice(-8);
  return rs;
}
function _decodeError(bytes, start, end) {
  var err = { code: 0 };
  var pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (field === 1) err.code = v.value;
      else if (field === 2) {
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
      } else {
        rec[d.name] = (bytes[p] | (bytes[p + 1] << 8) | (bytes[p + 2] << 16) |
                       (bytes[p + 3] << 24)) >>> 0; p += 4;
      }
    }
    out.push(rec);
    j++;
  }
  return out;
}
function _historyColumns(channels, w1Types) {
  var cols = [];
  for (var i = 0; i < channels.length; i++) {
    var slot = channels[i] >> 5, ch = channels[i] & 0x1f;
    var type = slot === 0 ? _SENSOR_MB_TYPE : (w1Types ? w1Types[slot - 1] : 0);
    var t = _SENSOR_TYPES[type];
    var c = t ? t.ch[ch] : undefined;
    if (!c || !c.h || !_HIST_ENC_SIZE[c.h[0]]) return null;
    var key = c.n.replace(/-/g, "_");
    cols.push({ key: slot === 0 ? key : "s" + slot + "_" + key, enc: c.h[0],
                scale: c.h[1], count: c.k === "rate" });
  }
  return cols;
}
function _decodeHistoryColumns(bytes, t0, cols, interval, synced) {
  var out = [];
  var recSize = 0;
  for (var i = 0; i < cols.length; i++) recSize += _HIST_ENC_SIZE[cols[i].enc];
  if (recSize === 0) return out;
  var p = 0, j = 0;
  while (p + recSize <= bytes.length) {
    var rec = { time: synced ? ((t0 + j * interval) >>> 0) : null };
    for (var k = 0; k < cols.length; k++) {
      var c = cols[k], n = _HIST_ENC_SIZE[c.enc], raw;
      if (n === 1) raw = bytes[p];
      else if (n === 2) raw = bytes[p] | (bytes[p + 1] << 8);
      else raw = (bytes[p] | (bytes[p + 1] << 8) | (bytes[p + 2] << 16) |
                  (bytes[p + 3] << 24)) >>> 0;
      p += n;
      if (raw === _HIST_ENC_SENTINEL[c.enc]) { rec[c.key] = null; continue; }
      if (c.enc === "i16" && raw > 0x7fff) raw -= 0x10000;
      else if (c.enc === "i32") raw = raw | 0;
      rec[c.key] = c.count ? raw : raw / c.scale;
    }
    out.push(rec);
    j++;
  }
  return out;
}
function _decodeHistoryFrame(bytes, start, end) {
  var hf = { records: [] };
  var t0 = 0, present = 0, interval = 0;
  var synced = true;
  var samples = null, channels = null, w1Types = null;
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
      else if (f === 10) channels = bytes.slice(pos, pos + len.value);
      else if (f === 11) w1Types = bytes.slice(pos, pos + len.value);
      pos += len.value;
    } else { break; }
  }
  if (hf.frame_count !== undefined && hf.frame_index === undefined) hf.frame_index = 0;
  else if (hf.frame_count === undefined) delete hf.frame_index;
  hf.t0_unix = t0;
  hf.present = present;
  hf.interval_s = interval;
  hf.time_synced = synced;
  if (channels) {
    var cols = _historyColumns(channels, w1Types);
    hf.channels = cols ? cols.map(function (c) { return c.key; }) : null;
    if (w1Types) hf.w1_types = Array.prototype.slice.call(w1Types);
    if (samples && cols) hf.records = _decodeHistoryColumns(samples, t0, cols, interval, synced);
  } else if (samples) {
    hf.records = _decodeHistorySamples(samples, t0, present, interval, synced);
  }
  return hf;
}
function _decodeEventPage(bytes) {
  var d = decodeAlarmBatch(bytes);
  d.events = d.alarms;
  delete d.alarms;
  delete d.truncated;
  return d;
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
      else if (field === 12) resp.page_index = v.value;
      else if (field === 13) resp.page_count = v.value;
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      var end = pos + len.value;
      if (field === 2) resp.ack = {};
      else if (field === 3) resp.info = _decodeInfo(bytes, pos, end);
      else if (field === 4) resp.config_dump = _decodeConfigDump(bytes, pos, end);
      else if (field === 5) resp.history_frame = _decodeHistoryFrame(bytes, pos, end);
      else if (field === 6) resp.error = _decodeError(bytes, pos, end);
      else if (field === 7) resp.w1_scan = _decodeW1Scan(bytes, pos, end);
      else if (field === 14) resp.radio_state = _decodeRadioState(bytes, pos, end);
      else if (field === 15) resp.event_page = _decodeEventPage(bytes.slice(pos, end));
      pos = end;
    } else {
      break;
    }
  }
  _applyPages(resp);
  return resp;
}
function _applyPages(resp) {
  if (resp.page_count === undefined) {
    if (resp.config_dump && resp.config_dump.page_count > 1) {
      resp.page_index = resp.config_dump.page_index || 0;
      resp.page_count = resp.config_dump.page_count;
    } else if (resp.history_frame && resp.history_frame.frame_count > 1) {
      resp.page_index = resp.history_frame.frame_index || 0;
      resp.page_count = resp.history_frame.frame_count;
    }
  }
  if (resp.page_count > 1) {
    if (resp.page_index === undefined) resp.page_index = 0;
    resp.pages = (resp.page_index + 1) + "/" + resp.page_count;
    if (resp.info) resp.info = _pruneInfoPage(resp.info);
  }
  if (resp.info) delete resp.info._seen;
}
function _pbZigzag(n) {
  return (n >>> 1) ^ -(n & 1);
}
var _W1_SLOT_TYPES = (function () {
  var m = { 0: "empty" };
  for (var id in _SENSOR_TYPES) { if (Number(id) !== 1) m[id] = _SENSOR_TYPES[id].name; }
  return m;
})();
var _W1_SLOT_STATES = ["none", "ok", "absent", "replaced", "mismatch"];
function _decodeSensorReading(bytes, start, end) {
  var sr = {};
  var valid = 0;
  var raw = [];
  var pos = start;
  while (pos < end && pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (field === 12 && wire === 2) {
      var pl = _pbReadVarint(bytes, pos); pos = pl.next;
      var pend = pos + pl.value;
      while (pos < pend) { var pv = _pbReadVarint(bytes, pos); pos = pv.next; raw.push(_pbZigzag(pv.value)); }
      continue;
    }
    if (wire !== 0) {
      if (wire === 2) { var l = _pbReadVarint(bytes, pos); pos = l.next + l.value; continue; }
      if (wire === 5) { pos += 4; continue; }
      if (wire === 1) { pos += 8; continue; }
      break;
    }
    var v = _pbReadVarint(bytes, pos); pos = v.next;
    switch (field) {
      case 1: sr.slot = v.value; break;
      case 2: sr.type = v.value; sr.type_name = _W1_SLOT_TYPES[v.value] || "unknown"; break;
      case 11: valid = v.value; break;
      case 12: raw.push(_pbZigzag(v.value)); break;
      default: break;
    }
  }
  var t = _SENSOR_TYPES[sr.type];
  sr.values = {};
  sr.units = {};
  var k = 0;
  for (var ch = 0; ch < 32; ch++) {
    var c = t ? t.ch[ch] : undefined;
    var has = ((valid >>> ch) & 1) === 1;
    var r = has ? raw[k++] : undefined;
    if (c === undefined) {
      if (has) sr.values["ch" + ch] = r;
      continue;
    }
    if (c.r) continue;
    sr.values[c.n] = (r === undefined) ? null : r / c.s;
    sr.units[c.n] = c.u;
  }
  return sr;
}
var _TM_S32_NA = -2147483648;
var _TM_U32_NA = 4294967295;
function decodeTelemetry(bytes) {
  var d = {};
  var pos = 0;
  while (pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (field === 27 && wire === 2) {
      var sl = _pbReadVarint(bytes, pos); pos = sl.next;
      if (!d.w1_sensors) { d.w1_sensors = []; }
      d.w1_sensors.push(_decodeSensorReading(bytes, pos, pos + sl.value));
      pos += sl.value;
      continue;
    }
    if (wire !== 0) {
      if (wire === 2) { var l = _pbReadVarint(bytes, pos); pos = l.next + l.value; continue; }
      if (wire === 5) { pos += 4; continue; }
      if (wire === 1) { pos += 8; continue; }
      break;
    }
    var v = _pbReadVarint(bytes, pos); pos = v.next;
    var f;
    switch (field) {
      case 1:  d.voltage = (v.value === 0) ? null : v.value / 50; break;
      case 2:
        d.boot = (v.value & (1 << 0)) !== 0;
        d.alarm_status = (v.value >>> 1) & 0xff;
        d.alarm_status_flags = _DEVICE_STATUS
          .filter(function (f) { return f[0] < (1 << 8) && (d.alarm_status & f[0]) !== 0; })
          .map(function (f) { return f[1]; });
        break;
      case 3:  { var _t = _pbZigzag(v.value); d.temperature = (_t === _TM_S32_NA) ? null : _t / 100; break; }
      case 4:  d.humidity = (v.value === _TM_U32_NA) ? null : v.value / 2; break;
      case 5:  d.pressure = (v.value === _TM_U32_NA) ? null : v.value / 10; break;
      case 6:  { var _a = _pbZigzag(v.value); d.altitude = (_a === _TM_S32_NA) ? null : _a / 10; break; }
      case 7:  d.illuminance = (v.value === _TM_U32_NA) ? null : v.value * 2; break;
      case 8:  d.orientation = (v.value === _TM_U32_NA) ? null : v.value; break;
      case 9:  d.motion_count = v.value; break;
      case 10: d.ext_temperature_1 = _pbZigzag(v.value) / 100; break;
      case 11: d.ext_temperature_2 = _pbZigzag(v.value) / 100; break;
      case 12: d.machine_probe_temperature_1 = _pbZigzag(v.value) / 100; break;
      case 13: d.machine_probe_humidity_1 = v.value / 2; break;
      case 14: d.machine_probe_tilt_alert_1 = (v.value & (1 << 0)) !== 0; break;
      case 15: d.machine_probe_temperature_2 = _pbZigzag(v.value) / 100; break;
      case 16: d.machine_probe_humidity_2 = v.value / 2; break;
      case 17: d.machine_probe_tilt_alert_2 = (v.value & (1 << 0)) !== 0; break;
      case 18: d.hall_left_count = v.value; break;
      case 19:
        d.hall_left_is_active = (v.value & (1 << 2)) !== 0;
        break;
      case 20: d.hall_right_count = v.value; break;
      case 21:
        d.hall_right_is_active = (v.value & (1 << 2)) !== 0;
        break;
      case 22: d.input_a_count = v.value; break;
      case 23:
        d.input_a_is_active = (v.value & (1 << 2)) !== 0;
        break;
      case 24: d.input_b_count = v.value; break;
      case 25:
        d.input_b_is_active = (v.value & (1 << 2)) !== 0;
        break;
      case 26: d.accel_motion_count = v.value; break;
      case 28: d.page_index = v.value; break;
      case 29: d.page_count = v.value; break;
      case 30: d.input_a_voltage = (v.value === _TM_U32_NA) ? null : v.value / 1000; break;
      case 31: d.input_b_voltage = (v.value === _TM_U32_NA) ? null : v.value / 1000; break;
      default: break;
    }
  }
  _applyTelemetryPages(d);
  return d;
}
function _applyTelemetryPages(d) {
  if (d.page_count > 1) {
    if (d.page_index === undefined) d.page_index = 0;
    d.pages = (d.page_index + 1) + "/" + d.page_count;
  }
}
function _encVarint(value) {
  var out = [];
  var v = value >>> 0;
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
      throw new Error("unknown lorawan field: " + name);
    }
  }
  return out;
}
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
function _encApplication(a) { return _encCfgGroup(a, _APP_TAGS, _APP_FLOAT, _APP_ENUMS, _APP_HEX_ENC); }
function _encSensors(s) { return _encCfgGroup(s, _SEN_TAGS, _SEN_FLOAT, _SEN_ENUMS, _SEN_HEX_ENC); }
function _encAlarms(a) { return _encCfgGroup(a, _ALM_TAGS, _ALM_FLOAT, _ALM_ENUMS, _ALM_HEX_ENC); }
function _encP2P(p) { return _encCfgGroup(p, _P2P_TAGS, _P2P_FLOAT, _P2P_ENUMS, null); }
function encodeDownlinkCommand(cmd) {
  var out = [];
  if (cmd.seq) out = out.concat(_encTag(1, 0)).concat(_encVarint(cmd.seq));
  var name = cmd.command;
  var tag = _CMD_TAGS[name];
  if (tag === undefined) {
    return { bytes: null, error: "unknown command: " + name };
  }
  var b = cmd[name] || {};
  var body = [];
  try {
  if (name === "set_param") {
    if (b.lorawan) body = body.concat(_encLenDelim(1, _encLorawan(b.lorawan)));
    if (b.application) body = body.concat(_encLenDelim(2, _encApplication(b.application)));
    if (b.sensors) body = body.concat(_encLenDelim(4, _encSensors(b.sensors)));
    if (b.alarms) body = body.concat(_encLenDelim(5, _encAlarms(b.alarms)));
    if (b.save) body = body.concat(_encTag(3, 0)).concat(_encVarint(1));
    if (b.alarms_replace) body = body.concat(_encTag(6, 0)).concat(_encVarint(1));
    if (b.p2p) body = body.concat(_encLenDelim(7, _encP2P(b.p2p)));
  } else if (name === "get_param") {
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
    if (b.page) body = body.concat(_encTag(5, 0)).concat(_encVarint(b.page));
    _packField(b.p2p_field, 6);
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
  } else if (name === "req_events") {
    if (b.from_unix) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.from_unix));
    if (b.to_unix) body = body.concat(_encTag(2, 0)).concat(_encVarint(b.to_unix));
    if (b.ranges && b.ranges.length) {
      var rp = [];
      for (var ri = 0; ri < b.ranges.length; ri++) {
        rp = rp.concat(_encVarint(b.ranges[ri][0])).concat(_encVarint(b.ranges[ri][1]));
      }
      body = body.concat(_encTag(3, 2)).concat(_encVarint(rp.length)).concat(rp);
    }
    if (b.seqs && b.seqs.length) {
      var ss = b.seqs.slice().sort(function (x, y) { return x - y; });
      var sp = [];
      for (var si = 0; si < ss.length; si++) {
        sp = sp.concat(_encVarint(si ? ss[si] - ss[si - 1] : ss[0]));
      }
      body = body.concat(_encTag(4, 2)).concat(_encVarint(sp.length)).concat(sp);
    }
  } else if (name === "clock_sync") {
    if (b.unix_time) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.unix_time));
  } else if (name === "req_events_page") {
    if (b.start_seq) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.start_seq));
    if (b.from_unix) body = body.concat(_encTag(2, 0)).concat(_encVarint(b.from_unix));
    if (b.to_unix) body = body.concat(_encTag(3, 0)).concat(_encVarint(b.to_unix));
  } else if (name === "req_history_page") {
    if (b.from_unix) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.from_unix));
    if (b.to_unix) body = body.concat(_encTag(2, 0)).concat(_encVarint(b.to_unix));
    if (b.start_ord) body = body.concat(_encTag(3, 0)).concat(_encVarint(b.start_ord));
  } else if (name === "get_radio_state") {
    if (b.page) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.page));
  } else if (name === "buzzer_play") {
    if (b.kind) body = body.concat(_encTag(1, 0)).concat(_encVarint(b.kind));
    if (b.repeat_s) body = body.concat(_encTag(2, 0)).concat(_encVarint(b.repeat_s));
  }
  } catch (e) {
    return { bytes: null, error: e.message };
  }
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
      if (field === 2) {
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
            else if (f2 === 7) sp.p2p = _decodeP2P(bytes, p, p + l2.value);
            p += l2.value;
          } else if (w2 === 0) {
            var sv = _pbReadVarint(bytes, p); p = sv.next;
            if (f2 === 3) sp.save = sv.value !== 0;
            else if (f2 === 6) sp.alarms_replace = sv.value !== 0;
          } else { break; }
        }
        cmd.set_param = sp;
      } else if (field === 3) {
        var gp = { lorawan_field: [], application_field: [], sensors_field: [], alarms_field: [],
                   p2p_field: [] },
            q = pos;
        while (q < end && q < bytes.length) {
          var t3 = _pbReadVarint(bytes, q); q = t3.next;
          var f3 = t3.value >>> 3, w3 = t3.value & 0x7;
          var dst = (f3 === 1) ? gp.lorawan_field : (f3 === 2) ? gp.application_field
                  : (f3 === 3) ? gp.sensors_field : (f3 === 4) ? gp.alarms_field
                  : (f3 === 6) ? gp.p2p_field : null;
          if (w3 === 0) {
            var v3 = _pbReadVarint(bytes, q); q = v3.next;
            if (f3 === 5) gp.page = v3.value;
            else if (dst) dst.push(v3.value);
          } else if (w3 === 2) {
            var pl3 = _pbReadVarint(bytes, q); q = pl3.next;
            var e3 = q + pl3.value;
            while (q < e3 && q < bytes.length) { var pv = _pbReadVarint(bytes, q); q = pv.next; if (dst) dst.push(pv.value); }
          } else { break; }
        }
        ["lorawan_field", "application_field", "sensors_field", "alarms_field", "p2p_field"].forEach(
          function (k) { if (gp[k].length === 0) delete gp[k]; });
        cmd.get_param = gp;
      } else if (field === 5) {
        var gc = {}, r2 = pos;
        if (r2 < end) {
          var t5 = _pbReadVarint(bytes, r2); r2 = t5.next;
          if ((t5.value >>> 3) === 1 && (t5.value & 0x7) === 0) {
            var v5 = _pbReadVarint(bytes, r2); gc.page = v5.value;
          }
        }
        cmd.get_config = gc;
      } else if (field === 10) {
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
      } else if (field === 11) {
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
      } else if (field === 33) {
        var re = {}, ru = pos;
        while (ru < end && ru < bytes.length) {
          var t33 = _pbReadVarint(bytes, ru); ru = t33.next;
          var f33 = t33.value >>> 3, w33 = t33.value & 0x7;
          if (w33 === 0) {
            var v33 = _pbReadVarint(bytes, ru); ru = v33.next;
            if (f33 === 1) re.from_unix = v33.value;
            else if (f33 === 2) re.to_unix = v33.value;
          } else if (w33 === 2) {
            var l33 = _pbReadVarint(bytes, ru); ru = l33.next;
            var e33 = ru + l33.value, vals = [];
            while (ru < e33) { var x33 = _pbReadVarint(bytes, ru); ru = x33.next; vals.push(x33.value); }
            if (f33 === 3) {
              re.ranges = [];
              for (var q = 0; q + 1 < vals.length; q += 2) re.ranges.push([vals[q], vals[q + 1]]);
            } else if (f33 === 4) {
              re.seqs = [];
              for (var q2 = 0; q2 < vals.length; q2++) re.seqs.push(q2 ? re.seqs[q2 - 1] + vals[q2] : vals[0]);
            }
          } else { break; }
        }
        cmd.req_events = re;
      } else if (field === 12) {
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
      } else if (field === 13) {
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
      } else if (field === 34) {
        var ep = {}, eq = pos;
        while (eq < end && eq < bytes.length) {
          var t34 = _pbReadVarint(bytes, eq); eq = t34.next;
          var f34 = t34.value >>> 3, w34 = t34.value & 0x7;
          if (w34 === 0) {
            var v34 = _pbReadVarint(bytes, eq); eq = v34.next;
            if (f34 === 1) ep.start_seq = v34.value;
            else if (f34 === 2) ep.from_unix = v34.value;
            else if (f34 === 3) ep.to_unix = v34.value;
          } else { break; }
        }
        cmd.req_events_page = ep;
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
var _ALARM_SLOTS = ["mb", "s1", "s2", "s3", "s4"];
var _ALARM_EDGES = ["activate", "deactivate"];
var _ALARM_TYPES = ["none", "low", "high", "trigger", "no_data", "sensor_mismatch"];
var _ALARM_TYPE_NO_DATA = 4;
var _ALARM_TYPE_SENSOR_MISMATCH = 5;
var _ALARM_CH_DEVICE = 255;
var _ALARM_RULE_COUNT = 16;
function _alarmChannel(slot, sensorType, ch) {
  var t = _SENSOR_TYPES[slot === 0 ? _SENSOR_MB_TYPE : sensorType];
  return t ? t.ch[ch] : undefined;
}
function _alarmTarget(slot, ch, sensorType, type) {
  var c = _alarmChannel(slot, sensorType, ch);
  var o = {
    slot: _ALARM_SLOTS[slot] || ("slot" + slot),
    channel: type === _ALARM_TYPE_SENSOR_MISMATCH ? null
      : ch === _ALARM_CH_DEVICE ? "device" : (c ? c.n : "ch" + ch),
  };
  if (type === _ALARM_TYPE_NO_DATA && c && c.p) {
    o.part = c.p;
  }
  if (sensorType !== null && slot !== 0) {
    o.sensor_type = _W1_SLOT_TYPES[sensorType] || ("type" + sensorType);
  }
  return o;
}
function _decodeAlarmStatus(bytes, start, end) {
  var a = { slot: 0, channel: 0, type: 0, sensor_type: null };
  var p = start;
  while (p < end && p < bytes.length) {
    var t = _pbReadVarint(bytes, p); p = t.next;
    var field = t.value >>> 3, wire = t.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, p); p = v.next;
      if (field === 1) a.slot = v.value;
      else if (field === 3) a.type = v.value;
      else if (field === 4) a.channel = v.value;
      else if (field === 5) a.sensor_type = v.value;
    } else if (wire === 2) {
      var l = _pbReadVarint(bytes, p); p = l.next + l.value;
    } else { break; }
  }
  var o = _alarmTarget(a.slot, a.channel, a.sensor_type, a.type);
  o.type = _ALARM_TYPES[a.type] || "none";
  return o;
}
function _decodeAlarmEvent(bytes, start, end) {
  var ev = { rule: 0, slot: 0, channel: 0, edge: 0, type: 0, rel_s: 0, value: null, sensor_type: null };
  var p = start;
  while (p < end && p < bytes.length) {
    var t = _pbReadVarint(bytes, p); p = t.next;
    var field = t.value >>> 3, wire = t.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, p); p = v.next;
      if (field === 1) ev.slot = v.value;
      else if (field === 2) ev.edge = v.value;
      else if (field === 4) ev.rel_s = v.value;
      else if (field === 5) ev.value = _pbZigzag(v.value);
      else if (field === 7) ev.rule = v.value;
      else if (field === 9) ev.type = v.value;
      else if (field === 10) ev.channel = v.value;
      else if (field === 11) ev.sensor_type = v.value;
      else if (field === 12) ev.seq = v.value;
    } else if (wire === 2) {
      var l = _pbReadVarint(bytes, p); p = l.next + l.value;
    } else { break; }
  }
  return ev;
}
function decodeAlarmBatch(bytes) {
  var out = { base_time: 0, total: 0, time_synced: true, alarms: [] };
  var rels = [], seqs = [];
  var pos = 0;
  while (pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3, wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (field === 1) out.base_time = v.value >>> 0;
      else if (field === 2) out.total = v.value;
      else if (field === 4) out.time_synced = v.value !== 0;
      else if (field === 5) out.page_index = v.value;
      else if (field === 6) out.page_count = v.value;
      else if (field === 7) out.seq_first = v.value >>> 0;
      else if (field === 8) out.replay = v.value !== 0;
      else if (field === 9) out.oldest_seq = v.value >>> 0;
      else if (field === 10) out.next_seq = v.value >>> 0;
      else if (field === 11) out.has_more = v.value !== 0;
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      var endE = pos + len.value;
      if (field === 3) {
        var ev = _decodeAlarmEvent(bytes, pos, endE);
        var mismatch = ev.type === _ALARM_TYPE_SENSOR_MISMATCH;
        var c = _alarmChannel(ev.slot, ev.sensor_type, ev.channel);
        var a = _alarmTarget(ev.slot, ev.channel, ev.sensor_type, ev.type);
        a.rule = ev.rule < _ALARM_RULE_COUNT ? ev.rule : null;
        Object.defineProperty(a, "_wire_rule", { value: ev.rule, enumerable: false });
        a.event = _ALARM_EDGES[ev.edge] || "activate";
        a.type = _ALARM_TYPES[ev.type] || "none";
        a.value = ev.value === null ? null
          : (mismatch || !c ? ev.value : ev.value / c.s);
        a.time = 0;
        if (mismatch && ev.value !== null) a.detected_type = _W1_SLOT_TYPES[ev.value] || ("type" + ev.value);
        out.alarms.push(a);
        rels.push(ev.rel_s);
        seqs.push(ev.seq);
      }
      pos = endE;
    } else { break; }
  }
  for (var i = 0; i < out.alarms.length; i++) {
    out.alarms[i].time = out.time_synced ? ((out.base_time + rels[i]) >>> 0) : null;
  }
  if (out.seq_first !== undefined) {
    var sq = out.seq_first;
    for (var j = 0; j < out.alarms.length; j++) {
      if (j > 0) sq = seqs[j] !== undefined ? seqs[j] : sq + 1;
      out.alarms[j].seq = sq;
    }
  }
  if (out.page_count > 1) {
    if (out.page_index === undefined) out.page_index = 0;
    out.pages = (out.page_index + 1) + "/" + out.page_count;
  } else {
    out.truncated = out.alarms.length < out.total;
  }
  return out;
}
var _PROTO_VERSION = 0x01;
function _eventCompact(a) {
  var c = { rule: a.rule, event: a.event, time: a.time };
  if (a._wire_rule === 255) c.source = "watchdog";
  else if (a._wire_rule === 254) c.source = "low_battery";
  if (a.seq !== undefined) c.seq = a.seq;
  return c;
}
function _u32le(b, i) {
  return (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24)) >>> 0;
}
function _decodeEventsRaw(bytes, warnings) {
  var out = { format: "raw", base_time: 0, time_synced: true, events: [] };
  if (bytes.length < 10) {
    warnings.push("raw event batch too short");
    return out;
  }
  var fl = bytes[5];
  out.base_time = _u32le(bytes, 1);
  out.time_synced = (fl & 0x01) !== 0;
  out.seq_first = _u32le(bytes, 6);
  var i = 10;
  if (fl & 0x02) {
    out.replay = true;
    if (bytes.length < 14) {
      warnings.push("raw replay header too short");
      return out;
    }
    out.oldest_seq = _u32le(bytes, 10);
    i = 14;
  }
  var rec = (fl & 0x04) ? 4 : 3;
  if ((bytes.length - i) % rec !== 0) warnings.push("raw event batch has trailing bytes");
  var sq = out.seq_first;
  for (var k = 0; i + rec <= bytes.length; i += rec, k++) {
    var rel = bytes[i] | (bytes[i + 1] << 8);
    var b = bytes[i + 2];
    var r = b & 0x7f;
    if (k > 0) sq += rec === 4 ? bytes[i + 3] : 1;
    var ev = {
      seq: sq,
      rule: r < _ALARM_RULE_COUNT ? r : null,
      event: (b & 0x80) ? "deactivate" : "activate",
      time: out.time_synced ? out.base_time + rel : null,
      rel_s: rel
    };
    if (!out.time_synced) ev.uptime_s = out.base_time + rel;
    if (r === 127) ev.source = "watchdog";
    else if (r === 126) ev.source = "low_battery";
    out.events.push(ev);
  }
  out.total = out.events.length;
  return out;
}
function _stripProtoVersion(bytes, warnings) {
  if (!bytes || bytes.length < 1) return bytes;
  if (bytes[0] !== _PROTO_VERSION) {
    warnings.push("unknown payload version 0x" + bytes[0].toString(16));
  }
  return bytes.slice(1);
}
function decodeUplink(input) {
  if (input.fPort === 85) {
    var w85 = [];
    var b85 = _stripProtoVersion(input.bytes, w85);
    return {
      data: decodeDownlinkResponse(b85),
      warnings: w85,
      errors: []
    };
  }
  if (input.fPort === 3) {
    var w3 = [];
    var b3 = _stripProtoVersion(input.bytes, w3);
    return { data: decodeAlarmBatch(b3), warnings: w3, errors: [] };
  }
  if (input.fPort === 4) {
    var w4 = [];
    if (input.bytes && input.bytes.length > 0 && input.bytes[0] === 0x03) {
      return { data: _decodeEventsRaw(input.bytes, w4), warnings: w4, errors: [] };
    }
    if (input.bytes && input.bytes.length > 0 && input.bytes[0] === 0x02) {
      var dc = decodeAlarmBatch(input.bytes.slice(1));
      dc.format = "compact";
      dc.events = dc.alarms.map(_eventCompact);
      delete dc.alarms;
      delete dc.truncated;
      return { data: dc, warnings: w4, errors: [] };
    }
    var b4 = _stripProtoVersion(input.bytes, w4);
    var d4 = decodeAlarmBatch(b4);
    d4.events = d4.alarms;
    delete d4.alarms;
    delete d4.truncated;
    return { data: d4, warnings: w4, errors: [] };
  }
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
  function readOrNull(width) {
    if (index + width > bytes.length) {
      index += width;
      return null;
    }
    var v = bytes[index];
    for (var i = 1; i < width; i++) {
      v = (v << 8) | bytes[index + i];
    }
    index += width;
    return v;
  }
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
    var voltage = readOrNull(1);
    data.voltage = (voltage === null || voltage === 0xff) ? null : voltage / 50;
  } else {
    data.voltage = null;
  }
  if (isExtendedPacket ? (header & (1 << 28)) : (header & (1 << 12))) {
    var temperature = readOrNull(2);
    data.temperature = (temperature === null || temperature === 0x7fff) ? null : toSignedInt16(temperature) / 100;
  } else {
    data.temperature = null;
  }
  if (isExtendedPacket ? (header & (1 << 27)) : (header & (1 << 11))) {
    var humidity = readOrNull(1);
    data.humidity = (humidity === null || humidity === 0xff) ? null : humidity / 2;
  } else {
    data.humidity = null;
  }
  if (isExtendedPacket ? (header & (1 << 26)) : (header & (1 << 10))) {
    var illuminance = readOrNull(2);
    data.illuminance = (illuminance === null || illuminance === 0xffff) ? null : illuminance * 2;
  } else {
    data.illuminance = null;
  }
  if (isExtendedPacket ? (header & (1 << 25)) : (header & (1 << 9))) {
    var ext_temperature_1 = readOrNull(2);
    data.ext_temperature_1 = (ext_temperature_1 === null || ext_temperature_1 === 0x7fff) ? null : toSignedInt16(ext_temperature_1) / 100;
  } else {
    data.ext_temperature_1 = null;
  }
  if (isExtendedPacket ? (header & (1 << 24)) : (header & (1 << 8))) {
    var ext_temperature_2 = readOrNull(2);
    data.ext_temperature_2 = (ext_temperature_2 === null || ext_temperature_2 === 0x7fff) ? null : toSignedInt16(ext_temperature_2) / 100;
  } else {
    data.ext_temperature_2 = null;
  }
  if (isExtendedPacket ? (header & (1 << 23)) : (header & (1 << 7))) {
    var motion_count = readOrNull(4);
    data.motion_count = (motion_count === null || motion_count === 0xffffffff) ? null : motion_count;
  } else {
    data.motion_count = null;
  }
  if (isExtendedPacket ? (header & (1 << 22)) : (header & (1 << 6))) {
    var altitude = readOrNull(2);
    data.altitude = (altitude === null || altitude === 0x7fff) ? null : toSignedInt16(altitude) / 10;
  } else {
    data.altitude = null;
  }
  if (isExtendedPacket ? (header & (1 << 21)) : (header & (1 << 5))) {
    var pressure = readOrNull(4);
    data.pressure = (pressure === null || pressure === 0xffffffff) ? null : pressure;
  } else {
    data.pressure = null;
  }
  if (isExtendedPacket) {
    if (header & (1 << 19)) {
      var machine_probe_temperature_1 = readOrNull(2);
      data.machine_probe_temperature_1 =
        (machine_probe_temperature_1 === null || machine_probe_temperature_1 === 0x7fff)
          ? null
          : toSignedInt16(machine_probe_temperature_1) / 100;
    } else {
      data.machine_probe_temperature_1 = null;
    }
    if (header & (1 << 18)) {
      var machine_probe_temperature_2 = readOrNull(2);
      data.machine_probe_temperature_2 =
        (machine_probe_temperature_2 === null || machine_probe_temperature_2 === 0x7fff)
          ? null
          : toSignedInt16(machine_probe_temperature_2) / 100;
    } else {
      data.machine_probe_temperature_2 = null;
    }
    if (header & (1 << 17)) {
      var machine_probe_humidity_1 = readOrNull(1);
      data.machine_probe_humidity_1 = (machine_probe_humidity_1 === null || machine_probe_humidity_1 === 0xff) ? null : machine_probe_humidity_1 / 2;
    } else {
      data.machine_probe_humidity_1 = null;
    }
    if (header & (1 << 16)) {
      var machine_probe_humidity_2 = readOrNull(1);
      data.machine_probe_humidity_2 = (machine_probe_humidity_2 === null || machine_probe_humidity_2 === 0xff) ? null : machine_probe_humidity_2 / 2;
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
      data.hall_left_count = readOrNull(4);
    } else {
      data.hall_left_count = null;
    }
    if (header & (1 << 12)) {
      data.hall_right_count = readOrNull(4);
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
      data.input_a_count = readOrNull(4);
      var status_a = readOrNull(1);
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
      data.input_b_count = readOrNull(4);
      var status_b = readOrNull(1);
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
if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    decodeUplink: decodeUplink,
    encodeDownlink: encodeDownlink,
    decodeDownlink: decodeDownlink,
    Decode: Decode,
    decodeTelemetry: decodeTelemetry,
    decodeDownlinkResponse: decodeDownlinkResponse,
    decodeAlarmBatch: decodeAlarmBatch,
    sensorTypes: _SENSOR_TYPES
  };
}
