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

// proto field tag -> name for ConfigDump.Application (floats marked in _APP_FLOAT)
var _APP_NAMES = {
  1: "calibration", 2: "interval_sample", 4: "interval_report",
  5: "temperature_alarm_enabled", 6: "temperature_alarm_lo", 7: "temperature_alarm_hi", 8: "temperature_alarm_hst",
  9: "humidity_alarm_enabled", 10: "humidity_alarm_lo", 11: "humidity_alarm_hi", 12: "humidity_alarm_hst",
  13: "pressure_alarm_enabled", 14: "pressure_alarm_lo", 15: "pressure_alarm_hi", 16: "pressure_alarm_hst",
  17: "t1_alarm_enabled", 18: "t1_alarm_lo", 19: "t1_alarm_hi", 20: "t1_alarm_hst",
  21: "t2_alarm_enabled", 22: "t2_alarm_lo", 23: "t2_alarm_hi", 24: "t2_alarm_hst",
  25: "hall_left_counter", 26: "hall_left_notify_act", 27: "hall_left_notify_deact",
  28: "hall_right_counter", 29: "hall_right_notify_act", 30: "hall_right_notify_deact",
  31: "input_a_counter", 32: "input_a_notify_act", 33: "input_a_notify_deact",
  34: "input_b_counter", 35: "input_b_notify_act", 36: "input_b_notify_deact",
  37: "temperature_corr", 38: "t1_corr", 39: "t2_corr",
  40: "cap_hall_left", 41: "cap_hall_right", 42: "cap_input_a", 43: "cap_input_b",
  44: "cap_light_sensor", 45: "cap_barometer", 46: "cap_pir_detector", 47: "cap_1w_thermometer", 48: "cap_1w_machine_probe",
  49: "history_enable", 50: "history_sensors", 51: "alarm_limit", 52: "alarm_notif_time",
  53: "pir_notify_act", 54: "accel_motion_sensitivity", 55: "cap_accelerometer"
};

// Enum-valued Application fields: decode renders the symbolic name, encode
// accepts either the name or the raw number.
var _APP_ENUMS = {
  54: ["off", "low", "medium", "high"]
};
var _LRW_NAMES = { 1: "region", 2: "network", 3: "adr", 4: "activation", 12: "sub_band" };
var _LRW_HEX = { 5: "deveui", 6: "joineui", 9: "devaddr" };

// Application tags carrying a float32 (protobuf wire type 5): alarm thresholds
// and the correction offsets. Everything else in Application is a varint.
var _APP_FLOAT = {
  6: 1, 7: 1, 8: 1, 10: 1, 11: 1, 12: 1, 14: 1, 15: 1, 16: 1,
  18: 1, 19: 1, 20: 1, 22: 1, 23: 1, 24: 1, 37: 1, 38: 1, 39: 1
};

// Reverse maps (name -> tag) for encoding SetParam. The LoRaWAN hex set adds the
// secret keys (nwkkey/appkey/nwkskey/appskey) which the decoder deliberately
// hides but which a downlink may legitimately set.
var _LRW_HEX_ENC = { deveui: 5, joineui: 6, nwkkey: 7, appkey: 8, devaddr: 9, nwkskey: 10, appskey: 11 };
var _LRW_ENUM = {
  region: { EU868: 0, US915: 1, AU915: 2 },
  network: { PUBLIC: 0, PRIVATE: 1 },
  activation: { OTAA: 0, ABP: 1 }
};
function _invert(map) {
  var out = {};
  for (var k in map) { if (map.hasOwnProperty(k)) out[map[k]] = +k; }
  return out;
}
var _APP_TAGS = _invert(_APP_NAMES);
var _LRW_TAGS = _invert(_LRW_NAMES);

// proto field tag -> command name in the DownlinkCommand body oneof.
var _CMD_NAMES = {
  2: "set_param", 3: "get_param", 4: "get_info", 5: "get_config",
  6: "settings_save", 7: "reboot", 8: "factory_reset", 9: "force_send",
  10: "reset_counters", 11: "req_history", 12: "clock_sync"
};
var _CMD_TAGS = _invert(_CMD_NAMES);

function _decodeLorawan(bytes, start, end) {
  var o = {}, pos = start;
  while (pos < end) {
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

function _decodeApplication(bytes, start, end) {
  var o = {}, pos = start;
  while (pos < end) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var f = tag.value >>> 3, w = tag.value & 0x7;
    if (w === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (_APP_NAMES[f]) {
        var ev = _APP_ENUMS[f] && _APP_ENUMS[f][v.value] !== undefined;
        o[_APP_NAMES[f]] = ev ? _APP_ENUMS[f][v.value] : v.value;
      }
    } else if (w === 5) {
      var fl = _pbReadFloat(bytes, pos); pos = fl.next;
      if (_APP_NAMES[f]) o[_APP_NAMES[f]] = fl.value;
    } else if (w === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      pos += len.value;
    } else { break; }
  }
  return o;
}

function _decodeConfigDump(bytes, start, end) {
  var cd = {}, pos = start;
  while (pos < end) {
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
      pos = e2;
    } else { break; }
  }
  return cd;
}

function _decodeInfo(bytes, start, end) {
  var info = { fw_major: 0, fw_minor: 0, fw_patch: 0, build_type: 0, debug: false };
  var pos = start;
  while (pos < end) {
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
    } else if (wire === 2) {
      // Skip unknown length-delimited fields (forward compatibility).
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      pos += len.value;
    } else {
      break;
    }
  }
  info.fw_version = info.fw_major + "." + info.fw_minor + "." + info.fw_patch;
  info.build_type_name = _BUILD_TYPES[info.build_type] || "unknown";
  return info;
}

function _decodeError(bytes, start, end) {
  var err = {};
  var pos = start;
  while (pos < end) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (field === 1) err.code = v.value;
      else if (field === 2) err.fault_field = v.value;
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

// app_history_sensor enum order → name + encoding (mirrors app_history.c).
var _HIST_SENSORS = [
  { name: "temperature", enc: "temp" },
  { name: "humidity", enc: "hum" },
  { name: "ext_temperature_1", enc: "temp" },
  { name: "ext_temperature_2", enc: "temp" },
  { name: "machine_probe_temperature_1", enc: "temp" },
  { name: "machine_probe_temperature_2", enc: "temp" },
  { name: "machine_probe_humidity_1", enc: "hum" },
  { name: "machine_probe_humidity_2", enc: "hum" },
  { name: "hall_left_count", enc: "count" },
  { name: "hall_right_count", enc: "count" },
  { name: "input_a_count", enc: "count" },
  { name: "input_b_count", enc: "count" },
  { name: "motion_count", enc: "count" }
];
var _HIST_TEMP_SENTINEL = 0x7fff;
var _HIST_HUM_SENTINEL = 0xff;

// HistoryFrame carries a shared `present` mask + `interval_s` once; samples is a
// sequence of fixed-size, values-only records (no per-record time or mask):
//   [per present sensor: scaled value]   int16 LE ×100 (temp), uint8 ×2 (hum),
//                                         uint32 LE (counter); sentinel = null.
// Record j's time is t0_unix + j*interval_s (records are periodic). One
// ReqHistory yields N such frames (frame_index 0..frame_count-1); the consumer
// concatenates their records to reconstruct the requested window.
function _decodeHistorySamples(bytes, t0, present, interval) {
  var out = [];
  var recSize = 0;
  for (var s = 0; s < _HIST_SENSORS.length; s++) {
    if (!(present & (1 << s))) continue;
    var e = _HIST_SENSORS[s].enc;
    recSize += (e === "temp") ? 2 : (e === "hum") ? 1 : 4;
  }
  if (recSize === 0) return out;

  var p = 0, j = 0;
  while (p + recSize <= bytes.length) {
    var rec = { time: (t0 + j * interval) >>> 0 };
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
  var samples = null;
  var pos = start;
  while (pos < end) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var f = tag.value >>> 3, w = tag.value & 0x7;
    if (w === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (f === 1) hf.frame_index = v.value;
      else if (f === 2) hf.frame_count = v.value;
      else if (f === 3) t0 = v.value;
      else if (f === 5) present = v.value;
      else if (f === 6) interval = v.value;
    } else if (w === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      if (f === 4) samples = bytes.slice(pos, pos + len.value);
      pos += len.value;
    } else { break; }
  }
  hf.t0_unix = t0;
  hf.present = present;
  hf.interval_s = interval;
  if (samples) hf.records = _decodeHistorySamples(samples, t0, present, interval);
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

// One SensorReading submessage (Telemetry field 27): slot=1, type=2,
// temperature=3 (sint32 ×100), humidity=4 (uint ×2), flags=5 (bit0 tilt). Absent
// quantities stay undefined. `bytes[start..end)` is the submessage body.
function _decodeSensorReading(bytes, start, end) {
  var sr = {};
  var pos = start;
  while (pos < end) {
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
      case 3: sr.temperature = _pbZigzag(v.value) / 100; break;
      case 4: sr.humidity = v.value / 2; break;
      case 5: sr.tilt_alert = (v.value & (1 << 0)) !== 0; break;
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
      case 1:  d.voltage = v.value / 50; break;
      case 2:  d.boot = (v.value & (1 << 0)) !== 0; break;     // system_flags (always sent)
      // internal (SHT4x)
      case 3:  d.temperature = _pbZigzag(v.value) / 100; break;
      case 4:  d.humidity = v.value / 2; break;
      // barometer
      case 5:  d.pressure = v.value / 1000; break;
      case 6:  d.altitude = _pbZigzag(v.value) / 10; break;
      // light
      case 7:  d.illuminance = v.value * 2; break;
      // accel
      case 8:  d.orientation = v.value; break;
      // pir
      case 9:  d.motion_count = v.value; break;
      // 1-wire slots are repeated SensorReading (field 27, handled above);
      // legacy flat fields 10-17 retired.
      // hall left
      case 18: d.hall_left_count = v.value; break;
      case 19:
        f = v.value;
        d.hall_left_notify_act = (f & (1 << 0)) !== 0;
        d.hall_left_notify_deact = (f & (1 << 1)) !== 0;
        d.hall_left_is_active = (f & (1 << 2)) !== 0;
        break;
      // hall right
      case 20: d.hall_right_count = v.value; break;
      case 21:
        f = v.value;
        d.hall_right_notify_act = (f & (1 << 0)) !== 0;
        d.hall_right_notify_deact = (f & (1 << 1)) !== 0;
        d.hall_right_is_active = (f & (1 << 2)) !== 0;
        break;
      // input A
      case 22: d.input_a_count = v.value; break;
      case 23:
        f = v.value;
        d.input_a_notify_act = (f & (1 << 0)) !== 0;
        d.input_a_notify_deact = (f & (1 << 1)) !== 0;
        d.input_a_is_active = (f & (1 << 2)) !== 0;
        break;
      // input B
      case 24: d.input_b_count = v.value; break;
      case 25:
        f = v.value;
        d.input_b_notify_act = (f & (1 << 0)) !== 0;
        d.input_b_notify_deact = (f & (1 << 1)) !== 0;
        d.input_b_is_active = (f & (1 << 2)) !== 0;
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
//   { "command": "clock_sync" }
//   { "command": "reboot" }                 // also settings_save / factory_reset
//   { "command": "reset_counters", "hall_left": true, "input_a": true }
//   { "command": "get_config", "page": 0 }
//   { "command": "req_history", "from_unix": 1780000000, "to_unix": 1780003600 }
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
    }
  }
  return out;
}

function _encApplication(app) {
  var out = [];
  for (var name in app) {
    if (!app.hasOwnProperty(name)) continue;
    var tag = _APP_TAGS[name];
    if (tag === undefined) continue;
    var val = app[name];
    if (_APP_FLOAT[tag]) {
      out = out.concat(_encTag(tag, 5)).concat(_encFloat(val));
    } else {
      var num = (typeof val === "boolean") ? (val ? 1 : 0) : val;
      if (typeof val === "string" && _APP_ENUMS[tag]) {
        var ix = _APP_ENUMS[tag].indexOf(val);
        if (ix >= 0) num = ix;
      }
      out = out.concat(_encTag(tag, 0)).concat(_encVarint(num));
    }
  }
  return out;
}

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
  if (name === "set_param") {
    if (b.lorawan) body = body.concat(_encLenDelim(1, _encLorawan(b.lorawan)));
    if (b.application) body = body.concat(_encLenDelim(2, _encApplication(b.application)));
    // save (field 3): persist + reboot after applying; set on the LAST message
    // of a multi-downlink batch only.
    if (b.save) body = body.concat(_encTag(3, 0)).concat(_encVarint(1));
  } else if (name === "get_param") {
    // proto3 repeated scalars are packed (length-delimited) by default.
    var lf = b.lorawan_field || [];
    var af = b.application_field || [];
    if (lf.length) {
      var pl = [];
      for (var i = 0; i < lf.length; i++) pl = pl.concat(_encVarint(lf[i]));
      body = body.concat(_encLenDelim(1, pl));
    }
    if (af.length) {
      var pa = [];
      for (var j = 0; j < af.length; j++) pa = pa.concat(_encVarint(af[j]));
      body = body.concat(_encLenDelim(2, pa));
    }
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
  }
  // get_info / settings_save / reboot / factory_reset / force_send / clock_sync: empty body.

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
        while (p < end) {
          var t = _pbReadVarint(bytes, p); p = t.next;
          var f2 = t.value >>> 3, w2 = t.value & 0x7;
          if (w2 === 2) {
            var l2 = _pbReadVarint(bytes, p); p = l2.next;
            if (f2 === 1) sp.lorawan = _decodeLorawan(bytes, p, p + l2.value);
            else if (f2 === 2) sp.application = _decodeApplication(bytes, p, p + l2.value);
            p += l2.value;
          } else if (w2 === 0) {
            var sv = _pbReadVarint(bytes, p); p = sv.next;
            if (f2 === 3) sp.save = sv.value !== 0; // persist + reboot after apply
          } else { break; }
        }
        cmd.set_param = sp;
      } else if (field === 3) { // get_param (repeated uint32, packed or not)
        var gp = { lorawan_field: [], application_field: [] }, q = pos;
        while (q < end) {
          var t3 = _pbReadVarint(bytes, q); q = t3.next;
          var f3 = t3.value >>> 3, w3 = t3.value & 0x7;
          var dst = (f3 === 1) ? gp.lorawan_field : (f3 === 2) ? gp.application_field : null;
          if (w3 === 0) {
            var v3 = _pbReadVarint(bytes, q); q = v3.next;
            if (dst) dst.push(v3.value);
          } else if (w3 === 2) {
            var pl3 = _pbReadVarint(bytes, q); q = pl3.next;
            var e3 = q + pl3.value;
            while (q < e3) { var pv = _pbReadVarint(bytes, q); q = pv.next; if (dst) dst.push(pv.value); }
          } else { break; }
        }
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
        while (s < end) {
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
        while (u < end) {
          var t11 = _pbReadVarint(bytes, u); u = t11.next;
          var f11 = t11.value >>> 3, w11 = t11.value & 0x7;
          if (w11 === 0) {
            var v11 = _pbReadVarint(bytes, u); u = v11.next;
            if (f11 === 1) rh.from_unix = v11.value;
            else if (f11 === 2) rh.to_unix = v11.value;
          } else { break; }
        }
        cmd.req_history = rh;
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

// fPort 3: alarm-detail batch (#27), protobuf AlarmReport. Top-level fields:
// base_time(1, varint), total(2, varint), repeated AlarmEvent events(3, len-
// delimited). AlarmEvent: source(1), edge(2), side(3), rel_s(4) varints +
// optional sint32 value(5). source/edge/side are the AlarmEvent_Source/Edge/Side
// enums; value is scaled (temp/hum ×100, pressure ×10) and absent for discrete
// sources. Per-event time = base_time + rel_s. `total` may exceed the events
// present (some dropped to fit the data rate → truncated).
var _ALARM_SOURCES = ["hall-left", "hall-right", "pir", "input-a", "input-b",
  "temperature", "humidity", "pressure", "t1-temperature", "t2-temperature",
  "accel-motion"];
var _ALARM_EDGES = ["activate", "deactivate"];
var _ALARM_SIDES = ["none", "lo", "hi"];

function _alarmUnscale(source, raw) {
  return source === 7 ? raw / 10 : raw / 100; // 7 = pressure (hPa×10)
}

function _decodeAlarmEvent(bytes, start, end) {
  var ev = { source: 0, edge: 0, side: 0, rel_s: 0, value: null };
  var p = start;
  while (p < end) {
    var t = _pbReadVarint(bytes, p); p = t.next;
    var field = t.value >>> 3, wire = t.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, p); p = v.next;
      if (field === 1) ev.source = v.value;
      else if (field === 2) ev.edge = v.value;
      else if (field === 3) ev.side = v.value;
      else if (field === 4) ev.rel_s = v.value;
      else if (field === 5) ev.value = _pbZigzag(v.value);
    } else if (wire === 2) {
      var l = _pbReadVarint(bytes, p); p = l.next + l.value;
    } else { break; }
  }
  return ev;
}

function decodeAlarmBatch(bytes) {
  var out = { base_time: 0, total: 0, alarms: [] };
  var rels = [];
  var pos = 0;
  while (pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3, wire = tag.value & 0x7;
    if (wire === 0) {
      var v = _pbReadVarint(bytes, pos); pos = v.next;
      if (field === 1) out.base_time = v.value >>> 0;
      else if (field === 2) out.total = v.value;
    } else if (wire === 2) {
      var len = _pbReadVarint(bytes, pos); pos = len.next;
      var endE = pos + len.value;
      if (field === 3) {
        var ev = _decodeAlarmEvent(bytes, pos, endE);
        out.alarms.push({
          source: _ALARM_SOURCES[ev.source] || ("src" + ev.source),
          event: _ALARM_EDGES[ev.edge] || "activate",
          side: _ALARM_SIDES[ev.side] || "none",
          value: ev.value === null ? null : _alarmUnscale(ev.source, ev.value),
          time: 0,
        });
        rels.push(ev.rel_s);
      }
      pos = endE;
    } else { break; }
  }
  // base_time (field 1) precedes events (field 3) on the wire, but resolve the
  // per-event times after the loop so order can't bite us.
  for (var i = 0; i < out.alarms.length; i++) {
    out.alarms[i].time = (out.base_time + rels[i]) >>> 0;
  }
  out.truncated = out.alarms.length < out.total; // some alarms dropped to fit the DR
  return out;
}

// 1-byte format version prefixed to application protobuf payloads (fPort 2
// telemetry, fPort 85 response). Mirrors APP_PROTO_VERSION in app_cmd.h.
var _PROTO_VERSION = 0x01;

// Strip + validate the version prefix at byte[0]. Returns the protobuf bytes
// (byte 1..end) and pushes a warning on an unexpected version (the remainder is
// still decoded best-effort). fPort 1 (legacy bitmap) and fPort 3 are unversioned.
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

  // fPort 3: alarm-detail batch (#27).
  if (input.fPort === 3) {
    return { data: decodeAlarmBatch(input.bytes), warnings: [], errors: [] };
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
// TTN/ChirpStack sandbox, which has no `module`.
if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    decodeUplink, encodeDownlink, decodeDownlink, Decode,
    decodeTelemetry, decodeDownlinkResponse, decodeAlarmBatch,
  };
}
