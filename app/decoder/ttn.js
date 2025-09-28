function decodeUplink(input) {

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
  }

  return {
    data: data,
    warnings: [],
    errors: []
  };
}

if (true) {

  // console.log("Decoded data:", JSON.stringify(decodeUplink({
  //   bytes: [0x78, 0x1a, 0x00, 0x01, 0xa5, 0x08, 0xd7, 0x88, 0x08, 0xed, 0x75],
  // }), null, 2));
  console.log("Decoded data:", JSON.stringify(decodeUplink({
    bytes: Buffer.from("7a01a109fa580258", "hex"),
  }), null, 2));
}
