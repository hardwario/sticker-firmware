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
    name: "set_param (lorawan.adr + application interval_report/temperature_alarm_hi)",
    hex: "0801120d0a021801120720783d00004842",
    data: {
      seq: 1,
      command: "set_param",
      set_param: {
        lorawan: { adr: 1 },
        application: { interval_report: 120, temperature_alarm_hi: 50 },
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
    // accel_motion_sensitivity is enum-valued (field 54): encode accepts the
    // symbolic name, decode renders it back ("high" = 3).
    name: "set_param (application accel_motion_sensitivity enum)",
    hex: "080312051203b00303",
    data: {
      seq: 3,
      command: "set_param",
      set_param: { application: { accel_motion_sensitivity: "high" } },
    },
  },
  {
    // #55: optional save flag (field 3) commits a multi-message SetParam batch.
    name: "set_param (save flag only)",
    hex: "080912021801",
    data: {
      seq: 9,
      command: "set_param",
      set_param: { save: true },
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
  // 01 = APP_PROTO_VERSION prefix, then Response{ seq=1, ack={} }.
  const got = codec.decodeUplink({ bytes: hex("0108011200"), fPort: 85 });
  assert.equal(got.data.seq, 1);
  assert.deepEqual(got.data.ack, {});
});

// --- Uplink: protobuf telemetry (fPort 2) ---------------------------------
// Real HW capture (#78/#80 verification, device sticker-2162165131, 2026-06-09):
// a non-boot report with hall-left + hall-right capabilities enabled but no
// counts. Proves the #80 fix on the wire: the system group is always present
// (boot=false encoded explicitly, not omitted) and an enabled sensor group is
// sent whole even at count=0. Frame bytes:
//   08 a8 01  voltage=168 -> 3.36 V
//   10 00     system_flags=0 -> boot=false (present, not dropped)
//   18 fa 24  temperature (sint zigzag) -> 23.65 degC
//   20 6c     humidity=108 -> 54 %RH
//   40 02     orientation=2
//   90 01 00  hall_left_count=0          98 01 04  hall_left_flags=ACTIVE
//   a0 01 00  hall_right_count=0         a8 01 04  hall_right_flags=ACTIVE
test("decodeUplink fPort 2: real HW frame, system + enabled groups always present", () => {
  // 01 = APP_PROTO_VERSION prefix (#55), then the captured Telemetry protobuf.
  const got = codec.decodeUplink({
    bytes: hex("0108a801100018fa24206c4002900100980104a00100a80104"),
    fPort: 2,
  }).data;
  assert.equal(got.voltage, 3.36);
  assert.equal(got.boot, false); // system group sent even when boot=false
  assert.equal(got.temperature, 23.65);
  assert.equal(got.humidity, 54);
  assert.equal(got.orientation, 2);
  // hall groups present in full although counts are 0 (capabilities enabled)
  assert.equal(got.hall_left_count, 0);
  assert.equal(got.hall_left_is_active, true);
  assert.equal(got.hall_left_notify_act, false);
  assert.equal(got.hall_left_notify_deact, false);
  assert.equal(got.hall_right_count, 0);
  assert.equal(got.hall_right_is_active, true);
  assert.equal(got.hall_right_notify_act, false);
  assert.equal(got.hall_right_notify_deact, false);
});

// #78: an enabled-sensor group is sent whole even when ALL its values are 0 —
// the decoder must surface those as explicit false/0, not omit them. Synthetic
// frame: voltage=100 (field 1), system_flags=0 (field 2 -> boot=false),
// hall_left_count=0 (field 18), hall_left_flags=0 (field 19 -> all false).
test("decodeUplink fPort 2: zero-valued groups decode to explicit false/0", () => {
  const got = codec.decodeUplink({ bytes: hex("0108641000900100980100"), fPort: 2 }).data;
  assert.equal(got.voltage, 2);
  assert.equal(got.boot, false);
  assert.equal(got.hall_left_count, 0);
  assert.equal(got.hall_left_notify_act, false);
  assert.equal(got.hall_left_notify_deact, false);
  assert.equal(got.hall_left_is_active, false);
});

test("fPort-2 telemetry decodes accel_motion_count (field 26)", () => {
  // 01 = version prefix, then tag (26 << 3 | varint) = 0xd0 0x01, value 3
  const got = codec.decodeUplink({ bytes: hex("01d00103"), fPort: 2 });
  assert.equal(got.data.accel_motion_count, 3);
});

// 1-Wire slots are a repeated SensorReading on field 27 (tag 0xda 0x01,
// length-delimited). type travels with each reading; the firmware emits one per
// populated slot and may split the list across frames. Two readings here:
//   reading A: slot=2 type=2(machine-probe) temp=23.65 hum=54 flags=tilt
//     08 02 | 10 02 | 18 fa 24 | 20 6c | 28 01   (11 B body)
//   reading B: slot=0 type=1(dallas) temp=21.5 (temperature-only)
//     08 00 | 10 01 | 18 cc 21                   (7 B body)
test("fPort-2 telemetry decodes repeated w1_sensors (field 27)", () => {
  const got = codec.decodeUplink({
    bytes: hex("01da010b0802100218fa24206c2801da01070800100118cc21"),
    fPort: 2,
  }).data;
  assert.equal(got.w1_sensors.length, 2);
  assert.deepEqual(got.w1_sensors[0], {
    slot: 2, type: 2, type_name: "machine-probe",
    temperature: 23.65, humidity: 54, tilt_alert: true,
  });
  assert.equal(got.w1_sensors[1].slot, 0);
  assert.equal(got.w1_sensors[1].type_name, "dallas");
  assert.equal(got.w1_sensors[1].temperature, 21.5);
  assert.equal(got.w1_sensors[1].humidity, undefined); // dallas → no humidity
});

test("fPort 2: unknown payload version is flagged but still decodes", () => {
  // version 0x02 (unknown) followed by voltage=100 (field 1) -> warn + decode
  const r = codec.decodeUplink({ bytes: hex("020864"), fPort: 2 });
  assert.equal(r.data.voltage, 2);
  assert.ok(r.warnings.length >= 1, "expected a version warning");
  assert.match(r.warnings[0], /version/);
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
  // 0x01 = APP_PROTO_VERSION prefix (#55); Response.history_frame = field 5.
  return [0x01].concat(pbTV(1, seq)).concat(pbLD(5, hf));
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

// --- Uplink: alarm-detail batch (fPort 3, #27, protobuf AlarmReport) -------
// AlarmReport{ base_time(1), total(2), repeated AlarmEvent events(3) };
// AlarmEvent{ source(1), edge(2), side(3), rel_s(4), optional sint32 value(5) }.
// proto3 omits zero fields — the builders mirror that so default-to-0 decoding
// (activate / side none / hall-left source) is exercised.
function pbSint(tag, v) { return pbTV(tag, v < 0 ? -v * 2 - 1 : v * 2); }
function alarmEvent(source, edge, side, rel, value) {
  let e = [];
  if (source) e = e.concat(pbTV(1, source));
  if (edge) e = e.concat(pbTV(2, edge));
  if (side) e = e.concat(pbTV(3, side));
  if (rel) e = e.concat(pbTV(4, rel));
  if (value !== null && value !== undefined) e = e.concat(pbSint(5, value));
  return e;
}
function buildAlarmReport(base, total, events) {
  let b = pbTV(1, base).concat(pbTV(2, total));
  for (const e of events) b = b.concat(pbLD(3, e));
  return b;
}

test("decodeUplink decodes an fPort-3 alarm batch (threshold + discrete)", () => {
  const base = 1780000000;
  const f = buildAlarmReport(base, 2, [
    alarmEvent(5, 0, 2, 10, 2660), // temperature, activate, HI, 26.6 °C
    alarmEvent(0, 0, 0, 15, null), // hall-left, activate, side none, discrete
  ]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;

  assert.equal(d.total, 2);
  assert.equal(d.base_time, base);
  assert.equal(d.truncated, false);
  assert.equal(d.alarms.length, 2);

  assert.equal(d.alarms[0].source, "temperature");
  assert.equal(d.alarms[0].event, "activate");
  assert.equal(d.alarms[0].side, "hi");
  assert.equal(d.alarms[0].value, 26.6);
  assert.equal(d.alarms[0].time, base + 10);

  assert.equal(d.alarms[1].source, "hall-left");
  assert.equal(d.alarms[1].event, "activate");
  assert.equal(d.alarms[1].side, "none");
  assert.equal(d.alarms[1].value, null); // discrete → absent
  assert.equal(d.alarms[1].time, base + 15);
});

test("fPort-3 batch: humidity deactivate + truncation flag (total > events)", () => {
  const base = 1780000000;
  const f = buildAlarmReport(base, 5, [alarmEvent(6, 1, 1, 0, 4500)]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.total, 5);
  assert.equal(d.alarms.length, 1);
  assert.equal(d.truncated, true); // 5 alarms occurred, only 1 fit the frame
  assert.equal(d.alarms[0].source, "humidity");
  assert.equal(d.alarms[0].event, "deactivate");
  assert.equal(d.alarms[0].side, "lo");
  assert.equal(d.alarms[0].value, 45);
  assert.equal(d.alarms[0].time, base);
});

test("fPort-3 batch: negative threshold value round-trips via sint zigzag", () => {
  const base = 1780000000;
  const f = buildAlarmReport(base, 1, [alarmEvent(5, 0, 1, 5, -1234)]); // temp -12.34 °C
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.alarms[0].source, "temperature");
  assert.equal(d.alarms[0].side, "lo");
  assert.equal(d.alarms[0].value, -12.34);
});

test("fPort-3 batch: accel-motion activate (protobuf AlarmReport)", () => {
  // AlarmReport: base_time=1780652851, total=1, one AlarmEvent{source=10
  // (accel-motion), edge/side=0 → activate/none, no value (discrete source)}.
  const f = Array.from(Buffer.from("08b3b68ad10610011a02080a", "hex"));
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.total, 1);
  assert.equal(d.base_time, 1780652851); // 2026-06-05T09:47:31Z (RTC synced)
  assert.equal(d.truncated, false);
  assert.equal(d.alarms.length, 1);
  assert.equal(d.alarms[0].source, "accel-motion");
  assert.equal(d.alarms[0].event, "activate");
  assert.equal(d.alarms[0].side, "none");
  assert.equal(d.alarms[0].value, null); // discrete source → no value
  assert.equal(d.alarms[0].time, 1780652851);
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
