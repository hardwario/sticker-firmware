// Tests for the STICKER calibration-mode codec (calibration.js, fPort 10).
// Run: `node --test` (Node >= 18, zero dependencies).
//
// calibration.js decodes a fixed 26 B struct (app_calibration.c
// compose_calibration_payload), not a nanopb message, so there is no protobuf
// schema to cross-check against -- these vectors are built from the documented
// byte layout instead of captured off a device. See app/src/app_calibration.c
// for the authoritative field offsets/scales.

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const codec = require("./calibration.js");

const SENTINEL = 0x7fff;
const BATTERY_INVALID = 0xffff;

// Builds the 26 B calibration payload from named fields; anything omitted
// defaults to "not read" (the same sentinel app_calibration.c uses when a
// sensor read fails or the slot is unpopulated).
function buildFrame(fields) {
  const f = Object.assign(
    {
      serial_number: 0,
      uptime: 0,
      internal_temperature: SENTINEL,
      internal_humidity: SENTINEL,
      w1_temperature_t1: SENTINEL,
      w1_temperature_t2: SENTINEL,
      machine_probe_1_temperature: SENTINEL,
      machine_probe_1_humidity: SENTINEL,
      machine_probe_2_temperature: SENTINEL,
      machine_probe_2_humidity: SENTINEL,
      battery_mv: BATTERY_INVALID,
    },
    fields
  );
  const buf = Buffer.alloc(26);
  buf.writeUInt32LE(f.serial_number >>> 0, 0);
  buf.writeUInt32LE(f.uptime >>> 0, 4);
  buf.writeInt16LE(f.internal_temperature, 8);
  buf.writeInt16LE(f.internal_humidity, 10);
  buf.writeInt16LE(f.w1_temperature_t1, 12);
  buf.writeInt16LE(f.w1_temperature_t2, 14);
  buf.writeInt16LE(f.machine_probe_1_temperature, 16);
  buf.writeInt16LE(f.machine_probe_1_humidity, 18);
  buf.writeInt16LE(f.machine_probe_2_temperature, 20);
  buf.writeInt16LE(f.machine_probe_2_humidity, 22);
  buf.writeUInt16LE(f.battery_mv, 24);
  return buf;
}

test("decodeUplink decodes a full calibration payload (fPort 10)", () => {
  const buf = buildFrame({
    serial_number: 2162165147,
    uptime: 125,
    internal_temperature: 2345, // 23.45 C
    internal_humidity: 5120, // 51.20 %
    w1_temperature_t1: 1830, // 18.30 C
    w1_temperature_t2: -550, // -5.50 C
    machine_probe_1_temperature: 2110, // 21.10 C
    machine_probe_1_humidity: 4567, // 45.67 %
    // machine_probe_2_* left at SENTINEL (probe not connected)
    battery_mv: 3000,
  });

  const got = codec.decodeUplink({ bytes: buf, fPort: 10 });

  assert.deepEqual(got.data, {
    serial_number: 2162165147,
    uptime: 125,
    internal_temperature: 23.45,
    internal_humidity: 51.2,
    w1_temperature_t1: 18.3,
    w1_temperature_t2: -5.5,
    machine_probe_1_temperature: 21.1,
    machine_probe_1_humidity: 45.67,
    machine_probe_2_temperature: null,
    machine_probe_2_humidity: null,
    battery_voltage: 3.0,
  });
});

test("decodeUplink nulls out every sensor at its sentinel + invalid battery (fPort 10)", () => {
  const buf = buildFrame({ serial_number: 1, uptime: 2 });

  const got = codec.decodeUplink({ bytes: buf, fPort: 10 });

  assert.deepEqual(got.data, {
    serial_number: 1,
    uptime: 2,
    internal_temperature: null,
    internal_humidity: null,
    w1_temperature_t1: null,
    w1_temperature_t2: null,
    machine_probe_1_temperature: null,
    machine_probe_1_humidity: null,
    machine_probe_2_temperature: null,
    machine_probe_2_humidity: null,
    battery_voltage: null,
  });
});

test("decodeUplink rejects the wrong fPort", () => {
  const buf = buildFrame({});
  const got = codec.decodeUplink({ bytes: buf, fPort: 2 });
  assert.ok(Array.isArray(got.errors) && got.errors.length > 0);
});

test("decodeUplink rejects a payload of the wrong length", () => {
  const got = codec.decodeUplink({ bytes: buildFrame({}).subarray(0, 25), fPort: 10 });
  assert.ok(Array.isArray(got.errors) && got.errors.length > 0);
});
