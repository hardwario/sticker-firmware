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
    name: "set_param (lorawan.adr + application interval_report + sensors cap_barometer)",
    hex: "0801120c0a0220011202187822023001",
    data: {
      seq: 1,
      command: "set_param",
      set_param: {
        lorawan: { adr: 1 },
        application: { interval_report: 120 },
        sensors: { cap_barometer: 1 },
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
    // accel_motion_sensitivity is enum-valued (sensors field 10): encode accepts
    // the symbolic name, decode renders it back ("high" = 3).
    name: "set_param (sensors accel_motion_sensitivity enum)",
    hex: "0803120422025003",
    data: {
      seq: 3,
      command: "set_param",
      set_param: { sensors: { accel_motion_sensitivity: "high" } },
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
  { name: "w1_scan", hex: "08097200", data: { seq: 9, command: "w1_scan" } },
  { name: "lrw_reset", hex: "0810820100", data: { seq: 16, command: "lrw_reset" } },
  { name: "lrw_join", hex: "08118a0100", data: { seq: 17, command: "lrw_join" } },
  { name: "enter_calibration", hex: "0812920100", data: { seq: 18, command: "enter_calibration" } },
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

// #92: cap_w1_sensors (sensors tag 8) reachable via the formatter both ways
// (was missing pre-regroup; now in the sensors submessage).
test("set_param cap_w1_sensors (tag 8) is reachable both ways (#92)", () => {
  const enc = codec.encodeDownlink({
    data: { seq: 1, command: "set_param", set_param: { sensors: { cap_w1_sensors: true } } },
  });
  assert.equal(enc.errors.length, 0, "encode errors");
  const back = codec.decodeDownlink({ bytes: enc.bytes, fPort: 85 }).data;
  assert.equal(back.set_param.sensors.cap_w1_sensors, 1);
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

test("decodeUplink splits Error.fault_field group*100 + tag (#196, fPort 85)", () => {
  // 01 prefix, Response{ seq=1, error=Error{ code=2 (OUT_OF_RANGE),
  // fault_field=205 } }. 205 = group 2 (application) * 100 + tag 5; the decoder
  // splits it into a readable fault_group + fault_field.
  //   01           APP_PROTO_VERSION prefix
  //   08 01        seq=1
  //   32 05        error (field 6), len=5
  //     08 02        code=2
  //     10 cd 01     fault_field=205 (varint)
  const got = codec.decodeUplink({ bytes: hex("0108013205080210cd01"), fPort: 85 }).data;
  assert.equal(got.seq, 1);
  assert.equal(got.error.code, 2);
  assert.equal(got.error.fault_group, 2);
  assert.equal(got.error.fault_field, 5);
});

// W1Scan response (field 7): the discovered 1-Wire ROMs come back as hex
// strings so the host can teach a slot via SetParam sensorN_rom.
//   01           APP_PROTO_VERSION prefix
//   08 02        seq=2
//   3a 14        w1_scan, len=20
//     0a 08 28000011223344a5   rom[0] (8 B: family 0x28 + serial + crc)
//     0a 08 28abcdef010203b7   rom[1]
test("decodeUplink decodes a W1Scan response (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108023a140a0828000011223344a50a0828abcdef010203b7"),
    fPort: 85,
  }).data;
  assert.equal(got.seq, 2);
  assert.deepEqual(got.w1_scan.rom, ["28000011223344a5", "28abcdef010203b7"]);
});

// Info response carrying the claim_token (#170): Info.claim_token = field 9
// (bytes), presented as hex. Readable over both NFC and LoRaWAN.
test("decodeUplink decodes get_info with claim_token (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a24080110041802200228d285d8cc04302a40014a10158a6a5d5b54c5118e62a8f4af0de8d2"),
    fPort: 85,
  }).data;
  assert.equal(got.seq, 3);
  assert.equal(got.info.fw_version, "1.4.2");
  assert.equal(got.info.serial_number, 1234567890);
  assert.equal(got.info.claim_token, "158a6a5d5b54c5118e62a8f4af0de8d2");
});

// An uncommissioned device omits claim_token (the all-zero sentinel) → absent.
test("decodeUplink get_info omits claim_token when uncommissioned (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a0c08011004180228d285d8cc04"),
    fPort: 85,
  }).data;
  assert.equal(got.seq, 3);
  assert.equal(got.info.claim_token, undefined);
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
  assert.equal(got.hall_right_count, 0);
  assert.equal(got.hall_right_is_active, true);
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
  assert.equal(got.hall_left_is_active, false);
});

test("fPort-2 telemetry decodes accel_motion_count (field 26)", () => {
  // 01 = version prefix, then tag (26 << 3 | varint) = 0xd0 0x01, value 3
  const got = codec.decodeUplink({ bytes: hex("01d00103"), fPort: 2 });
  assert.equal(got.data.accel_motion_count, 3);
});

// #92: pin the numeric scaling of barometer/light fields — these were never
// asserted, which is why the pressure unit could drift 10x. Frame: version 01,
// pressure (field 5, tag 0x28) raw 10135 -> 1013.5 hPa (hPa x10),
// altitude (field 6, tag 0x30, sint zigzag) raw 3210 -> 321.0 m (m x10),
// illuminance (field 7, tag 0x38) raw 250 -> 500 lux (lux /2... wire is lux/2).
test("fPort-2 telemetry: pressure/altitude/illuminance numeric scaling", () => {
  const got = codec.decodeUplink({ bytes: hex("0128974f30943238fa01"), fPort: 2 }).data;
  assert.equal(got.pressure, 1013.5); // hPa x10 on the wire -> hPa
  assert.equal(got.altitude, 321); // m x10
  assert.equal(got.illuminance, 500); // lux /2 on the wire -> lux
});

// 1-Wire slots are a repeated SensorReading on field 27 (tag 0xda 0x01,
// length-delimited). slot is 1-based (matches sensorN / `w1 list`). type travels
// with each reading; the firmware emits one per populated slot and may split the
// list across frames. Two readings here:
//   reading A: slot=3 type=2(machine-probe) temp=23.65 hum=54 flags=tilt
//     08 03 | 10 02 | 18 fa 24 | 20 6c | 28 01   (11 B body)
//   reading B: slot=1 type=1(dallas) temp=21.5 (temperature-only)
//     08 01 | 10 01 | 18 cc 21                   (7 B body)
test("fPort-2 telemetry decodes repeated w1_sensors (field 27)", () => {
  const got = codec.decodeUplink({
    bytes: hex("01da010b0803100218fa24206c2801da01070801100118cc21"),
    fPort: 2,
  }).data;
  assert.equal(got.w1_sensors.length, 2);
  assert.deepEqual(got.w1_sensors[0], {
    slot: 3, type: 2, type_name: "machine-probe",
    temperature: 23.65, humidity: 54, tilt_alert: true,
  });
  assert.equal(got.w1_sensors[1].slot, 1);
  assert.equal(got.w1_sensors[1].type_name, "dallas");
  assert.equal(got.w1_sensors[1].temperature, 21.5);
  assert.equal(got.w1_sensors[1].humidity, undefined); // dallas → no humidity
});

// Machine-probe sensor cluster: a single reading carrying the full set
// (slot=1 type=2 temp=21.5 lux=27 field=0.062mT accel=0.38/-9.35/-0.54 m/s²).
//   08 01 | 10 02 | 18 cc 21 | 30 1b | 38 7c | 40 4c | 48 cd 0e | 50 6b  (18 B body)
test("fPort-2 telemetry decodes machine-probe sensor cluster (fields 6-10)", () => {
  const got = codec.decodeUplink({
    bytes: hex("01da011208011002 18cc21 301b 387c 404c 48cd0e 506b".replace(/ /g, "")),
    fPort: 2,
  }).data;
  assert.equal(got.w1_sensors.length, 1);
  assert.deepEqual(got.w1_sensors[0], {
    slot: 1, type: 2, type_name: "machine-probe",
    temperature: 21.5, illuminance: 27, magnetic_field: 0.062,
    accel_x: 0.38, accel_y: -9.35, accel_z: -0.54,
  });
});

// Legacy flat 1-Wire fields (10-17, pre-SensorReading firmware) stay decodable
// so one formatter serves a mixed fleet: ext1 temp (field 10, sint 21.5 °C) +
// mp1 humidity (field 13, 54 %).
test("fPort-2 telemetry still decodes legacy flat 1-Wire fields (10-17)", () => {
  const got = codec.decodeUplink({ bytes: hex("0150cc21686c"), fPort: 2 }).data;
  assert.equal(got.ext_temperature_1, 21.5);
  assert.equal(got.machine_probe_humidity_1, 54);
  assert.equal(got.w1_sensors, undefined);
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

// --- Uplink: alarm-detail batch (fPort 3, protobuf AlarmReport) -----------
// AlarmReport{ base_time(1), total(2), repeated AlarmEvent events(3) };
// AlarmEvent{ source(1), edge(2), side(3), rel_s(4), optional sint32 value(5),
// quantity(6) }. Dynamic-alarm-rule model: source = enum app_alarm_source
// (0=onboard, 1..4=s1..s4, 5/6=hall l/r, 7/8=input a/b, 9=pir, 10=accel),
// quantity = enum app_alarm_quantity (0=temperature … 6=state, 7=count). proto3
// omits zero fields — the builders mirror that (default source=onboard,
// quantity=temperature, edge=activate, side=none).
function pbSint(tag, v) { return pbTV(tag, v < 0 ? -v * 2 - 1 : v * 2); }
function alarmEvent(source, quantity, edge, side, rel, value, slot) {
  let e = [];
  if (source) e = e.concat(pbTV(1, source));
  if (edge) e = e.concat(pbTV(2, edge));
  if (side) e = e.concat(pbTV(3, side));
  if (rel) e = e.concat(pbTV(4, rel));
  if (value !== null && value !== undefined) e = e.concat(pbSint(5, value));
  if (quantity) e = e.concat(pbTV(6, quantity));
  if (slot) e = e.concat(pbTV(7, slot));
  return e;
}
function buildAlarmReport(base, total, events) {
  // 0x01 = APP_PROTO_VERSION prefix (#165), then the AlarmReport protobuf.
  let b = [0x01].concat(pbTV(1, base)).concat(pbTV(2, total));
  for (const e of events) b = b.concat(pbLD(3, e));
  return b;
}

test("decodeUplink decodes an fPort-3 alarm batch (threshold + state)", () => {
  const base = 1780000000;
  const f = buildAlarmReport(base, 2, [
    alarmEvent(0, 0, 0, 2, 10, 2660, 7), // onboard temp, activate, HI, 26.6 °C, slot 7
    alarmEvent(5, 6, 0, 0, 15, 1),       // hall-left state, activate, level=1, slot 0
  ]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;

  assert.equal(d.total, 2);
  assert.equal(d.base_time, base);
  assert.equal(d.truncated, false);
  assert.equal(d.alarms.length, 2);

  assert.equal(d.alarms[0].slot, 7);
  assert.equal(d.alarms[0].source, "onboard");
  assert.equal(d.alarms[0].quantity, "temperature");
  assert.equal(d.alarms[0].event, "activate");
  assert.equal(d.alarms[0].side, "hi");
  assert.equal(d.alarms[0].value, 26.6);
  assert.equal(d.alarms[0].time, base + 10);

  assert.equal(d.alarms[1].slot, 0); // omitted on the wire → defaults to slot 0
  assert.equal(d.alarms[1].source, "hall-left");
  assert.equal(d.alarms[1].quantity, "state");
  assert.equal(d.alarms[1].event, "activate");
  assert.equal(d.alarms[1].side, "none");
  assert.equal(d.alarms[1].value, 1); // digital level
  assert.equal(d.alarms[1].time, base + 15);
});

test("fPort-3 batch: slot humidity deactivate + truncation flag (total > events)", () => {
  const base = 1780000000;
  // s2 humidity (source=2, quantity=1), deactivate, side lo, 45 %RH
  const f = buildAlarmReport(base, 5, [alarmEvent(2, 1, 1, 1, 0, 4500)]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.total, 5);
  assert.equal(d.alarms.length, 1);
  assert.equal(d.truncated, true); // 5 alarms occurred, only 1 fit the frame
  assert.equal(d.alarms[0].source, "s2");
  assert.equal(d.alarms[0].quantity, "humidity");
  assert.equal(d.alarms[0].event, "deactivate");
  assert.equal(d.alarms[0].side, "lo");
  assert.equal(d.alarms[0].value, 45);
  assert.equal(d.alarms[0].time, base);
});

test("fPort-3 batch: negative threshold value round-trips via sint zigzag", () => {
  const base = 1780000000;
  // s3 temperature (source=3, quantity=0), -12.34 °C, lo
  const f = buildAlarmReport(base, 1, [alarmEvent(3, 0, 0, 1, 5, -1234)]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.alarms[0].source, "s3");
  assert.equal(d.alarms[0].quantity, "temperature");
  assert.equal(d.alarms[0].side, "lo");
  assert.equal(d.alarms[0].value, -12.34);
});

test("fPort-3 batch: accel motion state activate", () => {
  const base = 1780652851;
  // accel (source=10) state (quantity=6) activate, value=1
  const f = buildAlarmReport(base, 1, [alarmEvent(10, 6, 0, 0, 0, 1)]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.total, 1);
  assert.equal(d.base_time, base);
  assert.equal(d.truncated, false);
  assert.equal(d.alarms.length, 1);
  assert.equal(d.alarms[0].source, "accel");
  assert.equal(d.alarms[0].quantity, "state");
  assert.equal(d.alarms[0].event, "activate");
  assert.equal(d.alarms[0].side, "none");
  assert.equal(d.alarms[0].value, 1);
  assert.equal(d.alarms[0].time, base);
});

test("fPort-3 batch: version prefix is stripped, body decodes after byte 0", () => {
  const base = 1780000000;
  const f = buildAlarmReport(base, 1, [alarmEvent(0, 0, 0, 2, 10, 2660, 7)]);
  assert.equal(f[0], 0x01); // APP_PROTO_VERSION prefix present (#165)
  const r = codec.decodeUplink({ bytes: f, fPort: 3 });
  assert.equal(r.warnings.length, 0);
  assert.equal(r.data.base_time, base);
  assert.equal(r.data.alarms[0].value, 26.6);
});

test("fPort-3 batch: unknown version byte warns but still decodes", () => {
  const base = 1780000000;
  const f = buildAlarmReport(base, 1, [alarmEvent(0, 0, 0, 2, 10, 2660, 7)]);
  f[0] = 0x02; // bump the version prefix to an unexpected value
  const r = codec.decodeUplink({ bytes: f, fPort: 3 });
  assert.equal(r.warnings.length, 1);
  assert.match(r.warnings[0], /unknown payload version 0x2/);
  assert.equal(r.data.base_time, base); // remainder still decoded best-effort
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

test("alarm slot set_param encodes as native bytes and round-trips (LRW)", () => {
  // Packed 17-byte rule: flags present|enabled, onboard temperature, lo=5 hi=30 hst=1.
  const rule = "03000000000000a0400000f0410000803f";
  const enc = codec.encodeDownlink({ data: { command: "set_param", seq: 1, set_param: { alarms: { alarm_0: rule } } } });
  assert.equal(enc.errors.length, 0, "encode errors");
  assert.equal(enc.fPort, 85);
  // The rule must appear raw on the wire (native bytes, 17 B), not as 34 hex chars.
  assert.ok(Buffer.from(enc.bytes).toString("hex").includes(rule));
  const dec = codec.decodeDownlink({ bytes: enc.bytes, fPort: 85 }).data;
  assert.equal(dec.set_param.alarms.alarm_0, rule);

  // ConfigDump (uplink) presents an alarm slot as hex too. Wire: ver 01, seq 1,
  // config_dump{ page_count=1, alarms{ alarm_0 } }. alarm_0 = alarms field 3
  // (the *_counter slots moved to the sensors group), tag 0x1a.
  const dump = hex("0108012217100132131a11" + rule);
  const u = codec.decodeUplink({ bytes: dump, fPort: 85 }).data;
  assert.equal(u.config_dump.alarms.alarm_0, rule);
});

// #205 follow-up: a config-enabled analog sensor is always on the wire; a NaN
// reading is sent as a sentinel and must decode to null (not a huge number).
test("fPort-2 telemetry: not-available sentinels decode to null", () => {
  // 01 version, temperature (field 3, tag 0x18) = INT32_MIN sentinel
  // (zigzag 0xFFFFFFFF), humidity (field 4, tag 0x20) = UINT32_MAX sentinel.
  const got = codec.decodeUplink({ bytes: hex("0118ffffffff0f20ffffffff0f"), fPort: 2 }).data;
  assert.equal(got.temperature, null);
  assert.equal(got.humidity, null);
});

// #205 follow-up: no_data watchdog event (sensor stopped reporting). slot 0xFF,
// no_data=true (field 8), value absent.
test("fPort-3 alarm: no_data flag decodes (sensor stopped reporting)", () => {
  const base = 1780000000;
  const ev = pbTV(7, 255).concat(pbTV(8, 1)); // slot=255, no_data=1 (source/quantity default onboard/temperature)
  const f = buildAlarmReport(base, 1, [ev]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.alarms[0].slot, 255);
  assert.equal(d.alarms[0].no_data, true);
  assert.equal(d.alarms[0].source, "onboard");
  assert.equal(d.alarms[0].quantity, "temperature");
  assert.equal(d.alarms[0].value, null);
});
