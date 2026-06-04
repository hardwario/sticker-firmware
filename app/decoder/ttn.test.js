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

// --- Uplink: history replay frames (fPort 85, device-driven multi-frame) ---
// HistoryFrame carries a shared present mask + interval_s; samples are fixed-size
// values-only records, time(j) = t0 + j*interval. Build frames with a tiny pb
// encoder so the vectors are self-describing.
function pbVarint(v) {
  const o = [];
  while (v > 127) { o.push((v & 0x7f) | 0x80); v = Math.floor(v / 128); }
  o.push(v);
  return o;
}
function pbTV(tag, varint) { return [(tag << 3) | 0].concat(pbVarint(varint)); }
function pbLD(tag, payload) { return [(tag << 3) | 2, payload.length].concat(payload); }
function histRec(tempC, humPct) {
  const t = Math.round(tempC * 100);
  return [t & 0xff, (t >> 8) & 0xff, Math.round(humPct * 2)]; // int16 LE temp, u8 hum
}
function buildHistoryFrame(seq, idx, count, t0, present, interval, samples) {
  // proto3 omits a zero field — mirror that for frame_index so the decoder's
  // default-to-0 is exercised (the firmware sends frame 0 with no field 1).
  let hf = idx ? pbTV(1, idx) : [];
  hf = hf.concat(pbTV(2, count)).concat(pbTV(3, t0))
    .concat(pbLD(4, samples)).concat(pbTV(5, present)).concat(pbTV(6, interval));
  return [].concat(pbTV(1, seq)).concat(pbLD(5, hf)); // Response.history_frame = field 5
}

test("decodeUplink decodes + reassembles multi-frame history (fPort 85)", () => {
  const present = 0x03; // temperature (bit0) + humidity (bit1)
  const interval = 900;
  const t0a = 1780000000;
  const f0 = buildHistoryFrame(10, 0, 2, t0a, present, interval,
    [].concat(histRec(21.5, 45)).concat(histRec(21.6, 46)));
  const t0b = t0a + 2 * interval;
  const f1 = buildHistoryFrame(10, 1, 2, t0b, present, interval, histRec(21.7, 47));

  const d0 = codec.decodeUplink({ bytes: f0, fPort: 85 }).data;
  const d1 = codec.decodeUplink({ bytes: f1, fPort: 85 }).data;

  assert.equal(d0.seq, 10);
  assert.equal(d0.history_frame.frame_index, 0);
  assert.equal(d0.history_frame.frame_count, 2);
  assert.equal(d0.history_frame.present, present);
  assert.equal(d0.history_frame.interval_s, interval);
  assert.equal(d0.history_frame.records.length, 2);
  assert.equal(d0.history_frame.records[0].temperature, 21.5);
  assert.equal(d0.history_frame.records[0].humidity, 45);
  assert.equal(d0.history_frame.records[0].time, t0a);
  assert.equal(d0.history_frame.records[1].time, t0a + interval); // implicit delta
  assert.equal(d1.history_frame.frame_index, 1);
  assert.equal(d1.history_frame.records[0].time, t0b);
  assert.equal(d1.history_frame.records[0].temperature, 21.7);

  // Consumer concatenates frames 0..count-1 into one window.
  const all = d0.history_frame.records.concat(d1.history_frame.records);
  assert.equal(all.length, 3);
  assert.deepEqual(all.map((r) => r.time), [t0a, t0a + interval, t0b]);
});

test("history sentinel values decode to null", () => {
  const present = 0x03;
  const f = buildHistoryFrame(1, 0, 1, 1780000000, present, 900, [0xff, 0x7f, 0xff]);
  const rec = codec.decodeUplink({ bytes: f, fPort: 85 }).data.history_frame.records[0];
  assert.equal(rec.temperature, null); // 0x7fff sentinel
  assert.equal(rec.humidity, null);    // 0xff sentinel
});

// --- Uplink: alarm-detail batch (fPort 3, #27) ----------------------------
function leU16(v) { return [v & 0xff, (v >> 8) & 0xff]; }
function leI16(v) { const u = v < 0 ? v + 0x10000 : v; return [u & 0xff, (u >> 8) & 0xff]; }
function leU32(v) { return [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff]; }
function alarmRec(source, side, active, rel, thr, val, hyst) {
  return [(source << 3) | (side << 1) | (active ? 1 : 0)]
    .concat(leU16(rel)).concat(leI16(thr)).concat(leI16(val)).concat(leI16(hyst));
}
function buildAlarmFrame(total, base, recs) {
  let b = [total & 0xff].concat(leU32(base));
  for (const r of recs) b = b.concat(r);
  return b;
}
const SENT = 0x8000; // discrete N/A sentinel

test("decodeUplink decodes an fPort-3 alarm batch (threshold + discrete)", () => {
  const base = 1780000000;
  const f = buildAlarmFrame(2, base, [
    alarmRec(5, 2, true, 10, 2000, 2660, 0),    // temperature, HI, activate, 20→26.6 °C
    alarmRec(0, 0, true, 15, SENT, SENT, SENT), // hall-left, discrete activate
  ]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;

  assert.equal(d.total, 2);
  assert.equal(d.base_time, base);
  assert.equal(d.truncated, false);
  assert.equal(d.alarms.length, 2);

  assert.equal(d.alarms[0].source, "temperature");
  assert.equal(d.alarms[0].event, "activate");
  assert.equal(d.alarms[0].side, "hi");
  assert.equal(d.alarms[0].threshold, 20);
  assert.equal(d.alarms[0].value, 26.6);
  assert.equal(d.alarms[0].time, base + 10);

  assert.equal(d.alarms[1].source, "hall-left");
  assert.equal(d.alarms[1].side, "na");
  assert.equal(d.alarms[1].threshold, null); // sentinel → null
  assert.equal(d.alarms[1].value, null);
  assert.equal(d.alarms[1].time, base + 15);
});

test("fPort-3 batch: humidity deactivate + truncation flag (total > records)", () => {
  const f = buildAlarmFrame(5, 1780000000, [alarmRec(6, 1, false, 0, 5000, 4500, 200)]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.total, 5);
  assert.equal(d.alarms.length, 1);
  assert.equal(d.truncated, true); // 5 alarms occurred, only 1 fit the frame
  assert.equal(d.alarms[0].source, "humidity");
  assert.equal(d.alarms[0].event, "deactivate");
  assert.equal(d.alarms[0].side, "lo");
  assert.equal(d.alarms[0].threshold, 50);
  assert.equal(d.alarms[0].value, 45);
  assert.equal(d.alarms[0].hysteresis, 2);
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
