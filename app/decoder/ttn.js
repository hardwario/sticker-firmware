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

var _BUILD_TYPES = ["main", "dev", "custom"];

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
    switch (field) {
      case 1:  d.voltage = v.value / 50; break;
      case 2:  d.temperature = _pbZigzag(v.value) / 100; break;
      case 3:  d.humidity = v.value / 2; break;
      case 4:  d.illuminance = v.value * 2; break;
      case 5:  d.pressure = v.value / 1000; break;
      case 6:  d.altitude = _pbZigzag(v.value) / 10; break;
      case 7:  d.ext_temperature_1 = _pbZigzag(v.value) / 100; break;
      case 8:  d.ext_temperature_2 = _pbZigzag(v.value) / 100; break;
      case 9:  d.machine_probe_temperature_1 = _pbZigzag(v.value) / 100; break;
      case 10: d.machine_probe_temperature_2 = _pbZigzag(v.value) / 100; break;
      case 11: d.machine_probe_humidity_1 = v.value / 2; break;
      case 12: d.machine_probe_humidity_2 = v.value / 2; break;
      case 13: d.orientation = v.value; break;
      case 14: d.motion_count = v.value; break;
      case 15: d.hall_left_count = v.value; break;
      case 16: d.hall_right_count = v.value; break;
      case 17: d.input_a_count = v.value; break;
      case 18: d.input_b_count = v.value; break;
      case 19:
        var f = v.value;
        d.boot = !!(f & (1 << 0));
        d.machine_probe_tilt_alert_1 = !!(f & (1 << 1));
        d.machine_probe_tilt_alert_2 = !!(f & (1 << 2));
        d.hall_left_notify_act = !!(f & (1 << 3));
        d.hall_left_notify_deact = !!(f & (1 << 4));
        d.hall_left_is_active = !!(f & (1 << 5));
        d.hall_right_notify_act = !!(f & (1 << 6));
        d.hall_right_notify_deact = !!(f & (1 << 7));
        d.hall_right_is_active = !!(f & (1 << 8));
        d.input_a_notify_act = !!(f & (1 << 9));
        d.input_a_notify_deact = !!(f & (1 << 10));
        d.input_a_is_active = !!(f & (1 << 11));
        d.input_b_notify_act = !!(f & (1 << 12));
        d.input_b_notify_deact = !!(f & (1 << 13));
        d.input_b_is_active = !!(f & (1 << 14));
        break;
      default: break; /* unknown field: ignore */
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
