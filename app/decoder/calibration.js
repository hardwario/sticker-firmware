function decodeUplink(input) {
  var bytes = input.bytes;

  if (input.fPort !== 10 || bytes.length !== 26) {
    return { errors: ["Invalid calibration payload"] };
  }

  var SENTINEL = 0x7FFF;
  var BATTERY_INVALID = 0xFFFF;

  function readUint32LE(b, o) {
    return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
  }

  function readUint16LE(b, o) {
    return b[o] | (b[o + 1] << 8);
  }

  function readInt16LE(b, o) {
    var val = b[o] | (b[o + 1] << 8);
    if (val >= 0x8000) val -= 0x10000;
    return val;
  }

  function decodeValue(raw, scale) {
    if (raw === SENTINEL) return null;
    return raw / scale;
  }

  var battery_mv = readUint16LE(bytes, 24);

  return {
    data: {
      serial_number: readUint32LE(bytes, 0),
      uptime: readUint32LE(bytes, 4),
      internal_temperature: decodeValue(readInt16LE(bytes, 8), 100),
      internal_humidity: decodeValue(readInt16LE(bytes, 10), 100),
      w1_temperature_t1: decodeValue(readInt16LE(bytes, 12), 100),
      w1_temperature_t2: decodeValue(readInt16LE(bytes, 14), 100),
      machine_probe_1_temperature: decodeValue(readInt16LE(bytes, 16), 100),
      machine_probe_1_humidity: decodeValue(readInt16LE(bytes, 18), 100),
      machine_probe_2_temperature: decodeValue(readInt16LE(bytes, 20), 100),
      machine_probe_2_humidity: decodeValue(readInt16LE(bytes, 22), 100),
      battery_voltage: battery_mv === BATTERY_INVALID ? null : battery_mv / 1000,
    }
  };
}

// Make the codec importable from Node (tests) without affecting the
// TTN/ChirpStack sandbox, which has no `module`.
if (typeof module !== "undefined" && module.exports) {
  module.exports = { decodeUplink };
}
