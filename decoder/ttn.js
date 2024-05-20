function decodeUplink(input) {
    function toSignedInt16(value) {
        return value > 0x7fff ? value - 0x10000 : value;
    }

    var data = {};
    var bytes = input.bytes;
    var header = (bytes[0] << 8) | bytes[1];
    var index = 2;

    if (header & (1 << 15)) {
        data.boot = true;
    } else {
        data.boot = false;
    }

    if (header & (1 << 14)) {
        data.orientation = header & 0xf;
    } else {
        data.orientation = null;
    }

    if (header & (1 << 13)) {
        data.voltage = bytes[index++];
        data.voltage = data.voltage / 50;
    } else {
        data.voltage = null;
    }

    if (header & (1 << 12)) {
        var temperature = (bytes[index++] << 8) | bytes[index++];
        data.temperature = toSignedInt16(temperature) / 100;
    } else {
        data.temperature = null;
    }

    if (header & (1 << 11)) {
        data.humidity = bytes[index++];
        data.humidity = data.humidity / 2;
    } else {
        data.humidity = null;
    }

    if (header & (1 << 10)) {
        var illuminance = (bytes[index++] << 8) | bytes[index++];
        data.illuminance = illuminance / 2;
    } else {
        data.illuminance = null;
    }

    if (header & (1 << 9)) {
        var ext_temperature_1 = (bytes[index++] << 8) | bytes[index++];
        data.ext_temperature_1 = toSignedInt16(ext_temperature_1) / 100;
    } else {
        data.ext_temperature_1 = null;
    }

    if (header & (1 << 8)) {
        var ext_temperature_2 = (bytes[index++] << 8) | bytes[index++];
        data.ext_temperature_2 = toSignedInt16(ext_temperature_2) / 100;
    } else {
        data.ext_temperature_2 = null;
    }

    return {
        data: data,
        warnings: [],
        errors: []
    };
}
