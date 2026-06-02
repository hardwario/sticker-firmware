// Tests for the STICKER LoRaWAN codec (ttn.js).
// Run: `node --test` (Node >= 18, zero dependencies).
//
// Vectors are HW-verified / current-schema captures. The downlink command hex
// are real DownlinkCommand frames (fPort 85); the uplink hex are real frames.

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const codec = require("./ttn.js");

const hex = (h) => Buffer.from(h, "hex");
const toHex = (bytes) => Buffer.from(bytes).toString("hex");

// --- Downlink commands (decodeDownlink, fPort 85) -------------------------
// Each entry: hex on the wire <-> the structured command it represents.
const COMMANDS = [
  {
    name: "set_param (lorawan.adr + application interval_report/alarm_temperature_hi)",
    hex: "0801120d0a021801120720783d00004842",
    data: {
      seq: 1,
      command: "set_param",
      set_param: {
        lorawan: { adr: 1 },
        application: { interval_report: 120, alarm_temperature_hi: 50 },
      },
    },
  },
  {
    name: "get_param (field lists)",
    hex: "08021a070a010312020407",
    data: {
      seq: 2,
      command: "get_param",
      get_param: { lorawan_field: [3], application_field: [4, 7] },
    },
  },
  {
    name: "reset_counters (hall_left + input_a)",
    hex: "0807520408011801",
    data: {
      seq: 7,
      command: "reset_counters",
      reset_counters: { hall_left: true, input_a: true },
    },
  },
  { name: "get_info", hex: "08032200", data: { seq: 3, command: "get_info" } },
  { name: "settings_save", hex: "08043200", data: { seq: 4, command: "settings_save" } },
  { name: "clock_sync", hex: "08056200", data: { seq: 5, command: "clock_sync" } },
  { name: "force_send", hex: "08064a00", data: { seq: 6, command: "force_send" } },
  { name: "reboot", hex: "08083a00", data: { seq: 8, command: "reboot" } },
];

test("decodeDownlink decodes command frames", () => {
  for (const c of COMMANDS) {
    const got = codec.decodeDownlink({ bytes: hex(c.hex), fPort: 85 });
    assert.deepEqual(got.data, c.data, c.name);
  }
});

test("encode/decode are symmetric (byte-exact round-trip)", () => {
  for (const c of COMMANDS) {
    const decoded = codec.decodeDownlink({ bytes: hex(c.hex), fPort: 85 }).data;
    const reencoded = codec.encodeDownlink({ data: decoded });
    assert.equal(reencoded.errors.length, 0, c.name + " encode errors");
    assert.equal(reencoded.fPort, 85, c.name + " fPort");
    assert.equal(toHex(reencoded.bytes), c.hex, c.name + " round-trip bytes");
  }
});

// --- Uplink: legacy bitmap (fPort 1) --------------------------------------
test("decodeUplink decodes legacy bitmap (fPort 1)", () => {
  const got = codec.decodeUplink({ bytes: hex("7a01a109fa580258"), fPort: 1 });
  assert.equal(got.data.boot, false);
  assert.equal(got.data.orientation, 1);
  assert.equal(got.data.voltage, 3.22);
  assert.equal(got.data.temperature, 25.54);
  assert.equal(got.data.humidity, 44);
  assert.equal(got.data.ext_temperature_1, 6);
  // sensors absent in this frame decode to null
  assert.equal(got.data.illuminance, null);
  assert.equal(got.data.pressure, null);
});

// --- Uplink: command response (fPort 85) ----------------------------------
test("decodeUplink decodes an Ack response (fPort 85)", () => {
  const got = codec.decodeUplink({ bytes: hex("08011200"), fPort: 85 });
  assert.equal(got.data.seq, 1);
  assert.deepEqual(got.data.ack, {});
});

// --- Uplink: protobuf telemetry (fPort 2) ---------------------------------
// TODO(#51): assert an exact HW telemetry frame once captured. Until then,
// confirm the fPort-2 path is wired and returns an object without throwing.
test("decodeUplink routes fPort 2 to the telemetry decoder", () => {
  const got = codec.decodeUplink({ bytes: hex("0864"), fPort: 2 });
  assert.equal(typeof got.data, "object");
});

// --- Negative: a corrupted frame must change the result (tests are sensitive) ---
test("a tampered command byte does not silently decode to the original", () => {
  const good = codec.decodeDownlink({ bytes: hex("08032200"), fPort: 85 }).data;
  // flip the command tag (0x22 get_info -> 0x3a reboot)
  const bad = codec.decodeDownlink({ bytes: hex("08033a00"), fPort: 85 }).data;
  assert.notDeepEqual(bad, good);
  assert.equal(bad.command, "reboot");
});

test("a truncated buffer is handled without throwing", () => {
  assert.doesNotThrow(() => codec.decodeUplink({ bytes: hex("7a01"), fPort: 1 }));
  assert.doesNotThrow(() => codec.decodeDownlink({ bytes: hex("0801"), fPort: 85 }));
});
