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
  5: "alarm_temperature_enabled", 6: "alarm_temperature_lo", 7: "alarm_temperature_hi", 8: "alarm_temperature_hst",
  9: "alarm_humidity_enabled", 10: "alarm_humidity_lo", 11: "alarm_humidity_hi", 12: "alarm_humidity_hst",
  13: "alarm_pressure_enabled", 14: "alarm_pressure_lo", 15: "alarm_pressure_hi", 16: "alarm_pressure_hst",
  17: "alarm_t1_temperature_enabled", 18: "alarm_t1_temperature_lo", 19: "alarm_t1_temperature_hi", 20: "alarm_t1_temperature_hst",
  21: "alarm_t2_temperature_enabled", 22: "alarm_t2_temperature_lo", 23: "alarm_t2_temperature_hi", 24: "alarm_t2_temperature_hst",
  25: "hall_left_counter", 26: "hall_left_notify_act", 27: "hall_left_notify_deact",
  28: "hall_right_counter", 29: "hall_right_notify_act", 30: "hall_right_notify_deact",
  31: "input_a_counter", 32: "input_a_notify_act", 33: "input_a_notify_deact",
  34: "input_b_counter", 35: "input_b_notify_act", 36: "input_b_notify_deact",
  37: "corr_temperature", 38: "corr_t1_temperature", 39: "corr_t2_temperature",
  40: "cap_hall_left", 41: "cap_hall_right", 42: "cap_input_a", 43: "cap_input_b",
  44: "cap_light_sensor", 45: "cap_barometer", 46: "cap_pir_detector", 47: "cap_1w_thermometer", 48: "cap_1w_machine_probe"
};
var _LRW_NAMES = { 1: "region", 2: "network", 3: "adr", 4: "activation", 12: "sub_band" };
var _LRW_HEX = { 5: "deveui", 6: "joineui", 9: "devaddr" };

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
      if (_APP_NAMES[f]) o[_APP_NAMES[f]] = v.value;
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

// fPort 2: flat protobuf Telemetry (periodic / event-triggered report). Keys
// match the legacy fPort-1 bitmap decoder so dashboards stay stable. Unknown
// fields are skipped (forward-compatible with newer firmware).
function decodeTelemetry(bytes) {
  var d = {};
  var pos = 0;
  while (pos < bytes.length) {
    var tag = _pbReadVarint(bytes, pos); pos = tag.next;
    var field = tag.value >>> 3;
    var wire = tag.value & 0x7;
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
      case 2:  if (v.value & (1 << 0)) d.boot = true; break;   // system_flags
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
      // 1-wire ext
      case 10: d.ext_temperature_1 = _pbZigzag(v.value) / 100; break;
      case 11: d.ext_temperature_2 = _pbZigzag(v.value) / 100; break;
      // machine probe 1
      case 12: d.machine_probe_temperature_1 = _pbZigzag(v.value) / 100; break;
      case 13: d.machine_probe_humidity_1 = v.value / 2; break;
      case 14: if (v.value & (1 << 0)) d.machine_probe_tilt_alert_1 = true; break;
      // machine probe 2
      case 15: d.machine_probe_temperature_2 = _pbZigzag(v.value) / 100; break;
      case 16: d.machine_probe_humidity_2 = v.value / 2; break;
      case 17: if (v.value & (1 << 0)) d.machine_probe_tilt_alert_2 = true; break;
      // hall left
      case 18: d.hall_left_count = v.value; break;
      case 19:
        f = v.value;
        if (f & (1 << 0)) d.hall_left_notify_act = true;
        if (f & (1 << 1)) d.hall_left_notify_deact = true;
        if (f & (1 << 2)) d.hall_left_is_active = true;
        break;
      // hall right
      case 20: d.hall_right_count = v.value; break;
      case 21:
        f = v.value;
        if (f & (1 << 0)) d.hall_right_notify_act = true;
        if (f & (1 << 1)) d.hall_right_notify_deact = true;
        if (f & (1 << 2)) d.hall_right_is_active = true;
        break;
      // input A
      case 22: d.input_a_count = v.value; break;
      case 23:
        f = v.value;
        if (f & (1 << 0)) d.input_a_notify_act = true;
        if (f & (1 << 1)) d.input_a_notify_deact = true;
        if (f & (1 << 2)) d.input_a_is_active = true;
        break;
      // input B
      case 24: d.input_b_count = v.value; break;
      case 25:
        f = v.value;
        if (f & (1 << 0)) d.input_b_notify_act = true;
        if (f & (1 << 1)) d.input_b_notify_deact = true;
        if (f & (1 << 2)) d.input_b_is_active = true;
        break;
      default: break; /* unknown field: ignore (forward-compatible) */
    }
  }
  return d;
}

function decodeUplink(input) {

  // fPort 85: DownlinkResponse (Ack / Info / Error from command dispatcher).
  if (input.fPort === 85) {
    return {
      data: decodeDownlinkResponse(input.bytes),
      warnings: [],
      errors: []
    };
  }

  // fPort 2: protobuf Telemetry (new format). fPort 1 stays the legacy bitmap.
  if (input.fPort === 2) {
    return {
      data: decodeTelemetry(input.bytes),
      warnings: [],
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

if (false) {

  // console.log("Decoded data:", JSON.stringify(decodeUplink({
  //   bytes: [0x78, 0x1a, 0x00, 0x01, 0xa5, 0x08, 0xd7, 0x88, 0x08, 0xed, 0x75],
  // }), null, 2));
  console.log("Decoded data:", JSON.stringify(decodeUplink({
    bytes: Buffer.from("7a01a109fa580258", "hex"),
  }), null, 2));
}
