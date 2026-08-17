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
  { name: "clock_sync", hex: "08056200", data: { seq: 5, command: "clock_sync", clock_sync: {} } },
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

// Info carries battery (field 10, mV) and reset_cause (field 11, hwinfo bitmask
// of the last boot, #88). Inner Info: fw 1.4.2, battery=3062 mV, reset_cause=0x20
// (RESET_WATCHDOG).
test("decodeUplink decodes get_info battery + reset_cause (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a0b08011004180250f6175820"),
    fPort: 85,
  }).data;
  assert.equal(got.seq, 3);
  assert.equal(got.info.battery, 3062);
  assert.equal(got.info.reset_cause, 0x20);
});

// reset_cause is decoded into named flags so the backend sees WHY the device
// rebooted, straight off the on-join GetInfo (fPort 85 over LoRaWAN). Inner Info:
// fw 1.4.2, battery=3062 mV, reset_cause=0x09 (RESET_PIN | RESET_POR).
test("decodeUplink decodes get_info reset_cause_flags (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a0b08011004180250f6175809"),
    fPort: 85,
  }).data;
  assert.equal(got.info.reset_cause, 0x09);
  assert.deepEqual(got.info.reset_cause_flags, ["pin", "power_on"]);
});

// Info carries lrw_state (field 12, mirrors app_lrw_state) and dev_eui (field 13,
// 8 bytes). Both are emitted over NFC only. Inner Info: fw 1.4.2,
// lrw_state=2 (HEALTHY), dev_eui=0102030405060708.
test("decodeUplink decodes get_info lrw_state + dev_eui (NFC)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a1208011004180260026a080102030405060708"),
    fPort: 85,
  }).data;
  assert.equal(got.seq, 3);
  assert.equal(got.info.lrw_state, 2);
  assert.equal(got.info.lrw_state_name, "healthy");
  assert.equal(got.info.dev_eui, "0102030405060708");
});

// Over LoRaWAN the device omits BOTH lrw_state and dev_eui (NFC-only fields):
// the LNS already knows the DevEUI, and the link state is redundant on a frame it
// just received. Inner Info: fw 1.4.2 only.
test("decodeUplink get_info omits lrw_state + dev_eui over LoRaWAN (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a06080110041802"),
    fPort: 85,
  }).data;
  assert.equal(got.seq, 3);
  assert.equal(got.info.lrw_state, undefined);
  assert.equal(got.info.lrw_state_name, undefined);
  assert.equal(got.info.dev_eui, undefined);
});

// Info carries device_status (field 14, bitmask). Inner Info: fw 1.4.2,
// device_status=0x03 (alarm_any | alarm_threshold).
test("decodeUplink decodes get_info device_status (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a080801100418027003"),
    fPort: 85,
  }).data;
  assert.equal(got.seq, 3);
  assert.equal(got.info.device_status, 0x03);
  assert.deepEqual(got.info.device_status_flags, ["alarm_any", "alarm_threshold"]);
});

// Info carries active_alarms (field 15, repeated AlarmStatus, #288). Inner Info:
// fw 1.4.2, device_status=0x11 (alarm_any | alarm_no_data), one active alarm
// AlarmStatus{ source=1 (s1), quantity=0 (temperature, proto3-omitted), type=4
// (no_data) } = 7a04 08011804. Mirrors an absent 1-Wire sensor in slot 1.
test("decodeUplink decodes get_info active_alarms (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a0e08011004180270117a0408011804"),
    fPort: 85,
  }).data;
  assert.equal(got.info.device_status, 0x11);
  assert.deepEqual(got.info.device_status_flags, ["alarm_any", "alarm_no_data"]);
  assert.deepEqual(got.info.active_alarms, [
    { source: "s1", quantity: "temperature", type: "no_data" },
  ]);
});

// A healthy device sends no active_alarms (empty repeated field) -> [].
test("decodeUplink get_info active_alarms defaults to [] when absent (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a080801100418027000"),
    fPort: 85,
  }).data;
  assert.deepEqual(got.info.active_alarms, []);
});

// lrw_state = 5 decodes to "disabled" (DevEUI all-zero radio-silent, #98).
test("decodeUplink get_info lrw_state=5 decodes as disabled (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a080801100418026005"),
    fPort: 85,
  }).data;
  assert.equal(got.info.lrw_state, 5);
  assert.equal(got.info.lrw_state_name, "disabled");
});

// A healthy device omits device_status (0 -> proto3 drops it); decoder defaults to 0/[].
test("decodeUplink get_info device_status defaults to 0 when absent (fPort 85)", () => {
  const got = codec.decodeUplink({
    bytes: hex("0108031a080801100418026002"),
    fPort: 85,
  }).data;
  assert.equal(got.info.device_status, 0);
  assert.deepEqual(got.info.device_status_flags, []);
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
function buildHistoryFrame(seq, idx, count, t0, present, interval, samples, synced, nextOrd,
                           hasMore) {
  // proto3 omits a zero field — mirror that for frame_index so the decoder's
  // default-to-0 is exercised (the firmware sends frame 0 with no field 1).
  let hf = idx ? pbTV(1, idx) : [];
  hf = hf.concat(pbTV(2, count)).concat(pbTV(3, t0))
    .concat(pbLD(4, samples)).concat(pbTV(5, present)).concat(pbTV(6, interval));
  // time_synced (field 7). Omitted when `synced` is undefined so the old-FW
  // "absent = treat as synced" default is exercised by the other vectors.
  if (synced !== undefined) hf = hf.concat(pbTV(7, synced ? 1 : 0));
  // next_ord (field 8) / has_more (field 9): NFC paged read (#260) only; omitted
  // over the device-driven LoRaWAN replay.
  if (nextOrd !== undefined) hf = hf.concat(pbTV(8, nextOrd));
  if (hasMore !== undefined) hf = hf.concat(pbTV(9, hasMore ? 1 : 0));
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

// #260: an NFC paged-read frame carries next_ord + has_more (fields 8/9). The
// shared decoder must surface them for the phone's cursor while still decoding
// records unchanged; a device-driven LoRaWAN frame omits them (has_more absent).
test("NFC paged history frame surfaces next_ord + has_more (#260)", () => {
  const present = 0x03;
  const interval = 900;
  const t0 = 1780000000;
  const mid = buildHistoryFrame(0, 0, 0, t0, present, interval, histRec(21.5, 45), true, 4, true);
  const dmid = codec.decodeUplink({ bytes: mid, fPort: 85 }).data;
  assert.equal(dmid.history_frame.next_ord, 4);
  assert.equal(dmid.history_frame.has_more, true);
  assert.equal(dmid.history_frame.records[0].temperature, 21.5);
  assert.equal(dmid.history_frame.records[0].time, t0);

  // Final page: has_more=false tells the phone to stop tapping.
  const last = buildHistoryFrame(1, 0, 0, t0, present, interval, histRec(21.7, 47), true, 9, false);
  const dlast = codec.decodeUplink({ bytes: last, fPort: 85 }).data;
  assert.equal(dlast.history_frame.next_ord, 9);
  assert.equal(dlast.history_frame.has_more, false);

  // Device-driven LoRaWAN replay omits both fields (undefined, not present).
  const lrw = buildHistoryFrame(2, 0, 1, t0, present, interval, histRec(20.0, 40));
  const dlrw = codec.decodeUplink({ bytes: lrw, fPort: 85 }).data;
  assert.equal(dlrw.history_frame.next_ord, undefined);
  assert.equal(dlrw.history_frame.has_more, undefined);
});

test("history sentinel values decode to null", () => {
  const present = 0x03;
  const f = buildHistoryFrame(1, 0, 1, 1780000000, present, 900, [0xff, 0x7f, 0xff]);
  const rec = codec.decodeUplink({ bytes: f, fPort: 85 }).data.history_frame.records[0];
  assert.equal(rec.temperature, null); // 0x7fff sentinel
  assert.equal(rec.humidity, null);    // 0xff sentinel
});

// #311: pressure (uint16 LE hPa×10), illuminance (uint16 LE lux/2), orientation
// (uint8 raw), accel_motion_count (uint32 LE) — bits 15..18.
function histRecNew(pressureHpa, lux, orient, accelCount) {
  const p = Math.round(pressureHpa * 10);
  const l = Math.round(lux / 2);
  return [
    p & 0xff, (p >> 8) & 0xff,
    l & 0xff, (l >> 8) & 0xff,
    orient & 0xff,
    accelCount & 0xff, (accelCount >> 8) & 0xff, (accelCount >> 16) & 0xff,
    (accelCount >> 24) & 0xff
  ];
}
const _PRESENT_NEW = 0x78000; // bit15 pressure | bit16 illuminance | bit17 orientation | bit18 accel_motion_count

test("history decodes pressure/illuminance/orientation/accel_motion_count (#311)", () => {
  const t0 = 1780000000;
  const f = buildHistoryFrame(1, 0, 1, t0, _PRESENT_NEW, 900,
    histRecNew(1013.2, 450, 3, 7));
  const rec = codec.decodeUplink({ bytes: f, fPort: 85 }).data.history_frame.records[0];
  assert.equal(rec.pressure, 1013.2);
  assert.equal(rec.illuminance, 450);
  assert.equal(rec.orientation, 3);
  assert.equal(rec.accel_motion_count, 7);
});

test("history sentinels for pressure/illuminance/orientation decode to null (#311)", () => {
  const t0 = 1780000000;
  const f = buildHistoryFrame(1, 0, 1, t0, _PRESENT_NEW, 900,
    [0xff, 0xff, 0xff, 0xff, 0xff, 7, 0, 0, 0]);
  const rec = codec.decodeUplink({ bytes: f, fPort: 85 }).data.history_frame.records[0];
  assert.equal(rec.pressure, null);
  assert.equal(rec.illuminance, null);
  assert.equal(rec.orientation, null);
  assert.equal(rec.accel_motion_count, 7); // counters have no sentinel, 0 is valid
});

test("history time_synced=false decodes record times as null (L-1/L-3)", () => {
  const present = 0x03;
  // t0 is uptime-relative here; the frame flags it unsynced, so times must be null
  // rather than a bogus ~1970 date — but the sensor values still decode.
  const f = buildHistoryFrame(7, 0, 1, 1234, present, 900,
    [].concat(histRec(21.5, 45)).concat(histRec(21.6, 46)), false);
  const hf = codec.decodeUplink({ bytes: f, fPort: 85 }).data.history_frame;
  assert.equal(hf.time_synced, false);
  assert.equal(hf.records.length, 2);
  assert.equal(hf.records[0].time, null);
  assert.equal(hf.records[1].time, null);
  assert.equal(hf.records[0].temperature, 21.5);
  assert.equal(hf.records[1].humidity, 46);
});

test("history time_synced=true keeps absolute record times", () => {
  const present = 0x03;
  const t0 = 1780000000;
  const f = buildHistoryFrame(8, 0, 1, t0, present, 900, histRec(21.5, 45), true);
  const hf = codec.decodeUplink({ bytes: f, fPort: 85 }).data.history_frame;
  assert.equal(hf.time_synced, true);
  assert.equal(hf.records[0].time, t0);
});

// --- Uplink: alarm-detail batch (fPort 3, protobuf AlarmReport) -----------
// AlarmReport{ base_time(1), total(2), repeated AlarmEvent events(3) };
// AlarmEvent{ source(1), edge(2), rel_s(4), optional sint32 value(5),
// quantity(6), slot(7), type(9) }. Dynamic-alarm-rule model: source = enum
// app_alarm_source (0=onboard, 1..4=s1..s4, 5/6=hall l/r, 7/8=input a/b, 9=pir,
// 10=accel), quantity = enum app_alarm_quantity (0=temperature … 6=state,
// 7=count). type = enum AlarmEvent.Type (0=none, 1=low, 2=high, 3=trigger,
// 4=no_data) — orthogonal to edge (#212). proto3 omits zero fields — the
// builders mirror that (default source=onboard, quantity=temperature,
// edge=activate, type=none).
function pbSint(tag, v) { return pbTV(tag, v < 0 ? -v * 2 - 1 : v * 2); }
function alarmEvent(source, quantity, edge, type, rel, value, slot) {
  let e = [];
  if (source) e = e.concat(pbTV(1, source));
  if (edge) e = e.concat(pbTV(2, edge));
  if (rel) e = e.concat(pbTV(4, rel));
  if (value !== null && value !== undefined) e = e.concat(pbSint(5, value));
  if (quantity) e = e.concat(pbTV(6, quantity));
  if (slot) e = e.concat(pbTV(7, slot));
  if (type) e = e.concat(pbTV(9, type));
  return e;
}
function buildAlarmReport(base, total, events, synced) {
  // 0x01 = APP_PROTO_VERSION prefix (#165), then the AlarmReport protobuf.
  let b = [0x01].concat(pbTV(1, base)).concat(pbTV(2, total));
  for (const e of events) b = b.concat(pbLD(3, e));
  // time_synced (field 4). Omitted when undefined → old-FW "treat as synced".
  if (synced !== undefined) b = b.concat(pbTV(4, synced ? 1 : 0));
  return b;
}

test("decodeUplink decodes an fPort-3 alarm batch (threshold + state)", () => {
  const base = 1780000000;
  const f = buildAlarmReport(base, 2, [
    alarmEvent(0, 0, 0, 2, 10, 2660, 7), // onboard temp, activate, HIGH, 26.6 °C, slot 7
    alarmEvent(5, 6, 0, 3, 15, 1),       // hall-left state, activate, TRIGGER, level=1, slot 0
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
  assert.equal(d.alarms[0].type, "high");
  assert.equal(d.alarms[0].value, 26.6);
  assert.equal(d.alarms[0].time, base + 10);

  assert.equal(d.alarms[1].slot, 0); // omitted on the wire → defaults to slot 0
  assert.equal(d.alarms[1].source, "hall-left");
  assert.equal(d.alarms[1].quantity, "state");
  assert.equal(d.alarms[1].event, "activate");
  assert.equal(d.alarms[1].type, "trigger");
  assert.equal(d.alarms[1].value, 1); // digital level
  assert.equal(d.alarms[1].time, base + 15);
});

test("fPort-3 batch time_synced=false → per-event time null (L-3/L-4)", () => {
  // base_time is uptime-relative; the report flags it unsynced, so per-event
  // times must be null instead of a bogus ~1970 date. Values still decode.
  const f = buildAlarmReport(120, 1, [
    alarmEvent(0, 0, 0, 2, 10, 2660, 7), // onboard temp HIGH 26.6 °C
  ], false);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.time_synced, false);
  assert.equal(d.alarms.length, 1);
  assert.equal(d.alarms[0].time, null);
  assert.equal(d.alarms[0].value, 26.6);
});

test("fPort-3 batch: slot humidity deactivate + truncation flag (total > events)", () => {
  const base = 1780000000;
  // s2 humidity (source=2, quantity=1), deactivate, type low, 45 %RH
  const f = buildAlarmReport(base, 5, [alarmEvent(2, 1, 1, 1, 0, 4500)]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.total, 5);
  assert.equal(d.alarms.length, 1);
  assert.equal(d.truncated, true); // 5 alarms occurred, only 1 fit the frame
  assert.equal(d.alarms[0].source, "s2");
  assert.equal(d.alarms[0].quantity, "humidity");
  assert.equal(d.alarms[0].event, "deactivate");
  assert.equal(d.alarms[0].type, "low");
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
  assert.equal(d.alarms[0].type, "low");
  assert.equal(d.alarms[0].value, -12.34);
});

test("fPort-3 batch: accel motion state activate", () => {
  const base = 1780652851;
  // accel (source=10) state (quantity=6) activate, type trigger, value=1
  const f = buildAlarmReport(base, 1, [alarmEvent(10, 6, 0, 3, 0, 1)]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.total, 1);
  assert.equal(d.base_time, base);
  assert.equal(d.truncated, false);
  assert.equal(d.alarms.length, 1);
  assert.equal(d.alarms[0].source, "accel");
  assert.equal(d.alarms[0].quantity, "state");
  assert.equal(d.alarms[0].event, "activate");
  assert.equal(d.alarms[0].type, "trigger");
  assert.equal(d.alarms[0].value, 1);
  assert.equal(d.alarms[0].time, base);
});

test("fPort-3 batch: low-battery watchdog event (battery/voltage, type=low, slot 0xFE)", () => {
  const base = 1780000000;
  // battery (source=11) voltage (quantity=8), activate, type low (1), 2.15 V (×100=215), slot 254
  const f = buildAlarmReport(base, 1, [alarmEvent(11, 8, 0, 1, 0, 215, 254)]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.alarms.length, 1);
  assert.equal(d.alarms[0].slot, 254);
  assert.equal(d.alarms[0].source, "battery");
  assert.equal(d.alarms[0].quantity, "voltage");
  assert.equal(d.alarms[0].event, "activate");
  assert.equal(d.alarms[0].type, "low");
  assert.equal(d.alarms[0].value, 2.15); // V×100 unscaled
});

test("fPort-3 batch: no-data watchdog event (type=no_data, slot 0xFF)", () => {
  const base = 1780000000;
  // s1 temperature (source=1, quantity=0) stopped reporting: activate, type
  // no_data (4), no value, slot 0xFF (255)
  const f = buildAlarmReport(base, 1, [alarmEvent(1, 0, 0, 4, 0, null, 255)]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.alarms.length, 1);
  assert.equal(d.alarms[0].slot, 255);
  assert.equal(d.alarms[0].source, "s1");
  assert.equal(d.alarms[0].quantity, "temperature");
  assert.equal(d.alarms[0].event, "activate");
  assert.equal(d.alarms[0].type, "no_data");
  assert.equal(d.alarms[0].value, null);
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
  // Packed 17-byte rule: flags present|enabled, onboard temperature, lo=5 hi=30 dwell=1.
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

// #205 follow-up / #212: no_data watchdog event (sensor stopped reporting).
// slot 0xFF, type=no_data (field 9 = 4), value absent.
test("fPort-3 alarm: no_data type decodes (sensor stopped reporting)", () => {
  const base = 1780000000;
  const ev = pbTV(7, 255).concat(pbTV(9, 4)); // slot=255, type=no_data (source/quantity default onboard/temperature)
  const f = buildAlarmReport(base, 1, [ev]);
  const d = codec.decodeUplink({ bytes: f, fPort: 3 }).data;
  assert.equal(d.alarms[0].slot, 255);
  assert.equal(d.alarms[0].type, "no_data");
  assert.equal(d.alarms[0].source, "onboard");
  assert.equal(d.alarms[0].quantity, "temperature");
  assert.equal(d.alarms[0].value, null);
});

// --- H2: config maps no longer drop LoRaWAN-readable fields ----------------
test("set_param application.battery_level round-trips (#H2)", () => {
  const enc = codec.encodeDownlink({
    data: { seq: 1, command: "set_param", set_param: { application: { battery_level: 2000 } } },
  });
  assert.equal(enc.errors.length, 0, "encode errors: " + enc.errors);
  const back = codec.decodeDownlink({ bytes: enc.bytes, fPort: 85 }).data;
  assert.equal(back.set_param.application.battery_level, 2000);
});

test("set_param application.vendor_reset_allow round-trips (#H2)", () => {
  const enc = codec.encodeDownlink({
    data: { seq: 2, command: "set_param", set_param: { application: { vendor_reset_allow: true } } },
  });
  assert.equal(enc.errors.length, 0, "encode errors: " + enc.errors);
  const back = codec.decodeDownlink({ bytes: enc.bytes, fPort: 85 }).data;
  assert.equal(back.set_param.application.vendor_reset_allow, 1);
});

test("set_param lorawan.radio_mode (enum) + link-check fields round-trip (#H2)", () => {
  const enc = codec.encodeDownlink({
    data: {
      seq: 3, command: "set_param",
      set_param: { lorawan: { radio_mode: "P2P", link_check_interval: 7, link_check_fail_rejoin: 3 } },
    },
  });
  assert.equal(enc.errors.length, 0, "encode errors: " + enc.errors);
  const back = codec.decodeDownlink({ bytes: enc.bytes, fPort: 85 }).data;
  assert.equal(back.set_param.lorawan.radio_mode, 2); // P2P index
  assert.equal(back.set_param.lorawan.link_check_interval, 7);
  assert.equal(back.set_param.lorawan.link_check_fail_rejoin, 3);
});

test("encode surfaces an error on an unknown config field instead of a silent no-op (#H2)", () => {
  const enc = codec.encodeDownlink({
    data: { seq: 4, command: "set_param", set_param: { application: { not_a_field: 1 } } },
  });
  assert.equal(enc.errors.length, 1, "expected one error");
  assert.match(enc.errors[0], /unknown config field: not_a_field/);
  assert.deepEqual(enc.bytes, []);
});

test("get_param.page encodes and decodes (#93.3 / M12)", () => {
  const enc = codec.encodeDownlink({
    data: { seq: 5, command: "get_param", get_param: { application_field: [6, 7], page: 1 } },
  });
  assert.equal(enc.errors.length, 0, "encode errors: " + enc.errors);
  const back = codec.decodeDownlink({ bytes: enc.bytes, fPort: 85 }).data;
  assert.deepEqual(back.get_param.application_field, [6, 7]);
  assert.equal(back.get_param.page, 1);
});

// --- #345: encodeDownlinkCommand/decodeDownlinkCommand previously dropped
// clock_sync.unix_time and req_history_page's fields entirely (silent no-op
// body instead of an error) -------------------------------------------------
test("clock_sync.unix_time round-trips instead of encoding an empty body (#345)", () => {
  const enc = codec.encodeDownlink({
    data: { seq: 6, command: "clock_sync", clock_sync: { unix_time: 1786449445 } },
  });
  assert.equal(enc.errors.length, 0, "encode errors: " + enc.errors);
  assert.ok(enc.bytes.length > 2, "body must not be empty");
  const back = codec.decodeDownlink({ bytes: enc.bytes, fPort: 85 }).data;
  assert.equal(back.clock_sync.unix_time, 1786449445);
});

test("clock_sync with no unix_time still encodes an empty body (network re-sync)", () => {
  const enc = codec.encodeDownlink({ data: { seq: 7, command: "clock_sync" } });
  assert.equal(enc.errors.length, 0, "encode errors: " + enc.errors);
  const back = codec.decodeDownlink({ bytes: enc.bytes, fPort: 85 }).data;
  assert.equal(back.command, "clock_sync");
  assert.equal(back.clock_sync.unix_time, undefined);
});

test("req_history_page.from_unix/to_unix/start_ord round-trip (#345, #260)", () => {
  const enc = codec.encodeDownlink({
    data: {
      seq: 8, command: "req_history_page",
      req_history_page: { from_unix: 1780000000, to_unix: 1780003600, start_ord: 440 },
    },
  });
  assert.equal(enc.errors.length, 0, "encode errors: " + enc.errors);
  const back = codec.decodeDownlink({ bytes: enc.bytes, fPort: 85 }).data;
  assert.equal(back.req_history_page.from_unix, 1780000000);
  assert.equal(back.req_history_page.to_unix, 1780003600);
  assert.equal(back.req_history_page.start_ord, 440);
});

// --- buzzer_play: encodeDownlinkCommand previously had no branch for it, so
// the body stayed empty (silently producing a STOP instead of the requested
// melody). BuzzerPlay has no oneof/nesting quirks decodeDownlinkCommand
// special-cases (unlike set_param/get_param/etc.), and there is no per-command
// decode branch for tag 28 — so a round-trip via decodeDownlink is not
// available; assert directly on the encoded wire bytes instead.
test("buzzer_play encodes a non-empty body (kind + repeat_s)", () => {
  const enc = codec.encodeDownlink({
    data: { seq: 1, command: "buzzer_play", buzzer_play: { kind: 5, repeat_s: 30 } },
  });
  assert.equal(enc.errors.length, 0, "encode errors: " + enc.errors);
  assert.equal(enc.fPort, 85);
  assert.ok(enc.bytes.length > 0, "body must not be empty (old bug: silent STOP)");
  // seq=1 (tag 0x08, varint 1), then field 28 length-delimited (tag (28<<3)|2 =
  // 0xe2 0x01, varint-encoded as two bytes since 226 > 127), then the body:
  // tag 1 varint (0x08) value 5, tag 2 varint (0x10) value 30.
  const hexOut = toHex(enc.bytes);
  assert.equal(hexOut, "0801e201040805101e");
  // Manually decode the BuzzerPlay body (bytes after the outer length prefix)
  // to confirm kind/repeat_s land at the documented tag positions.
  const body = enc.bytes.slice(enc.bytes.length - 4); // last 4 bytes: 08 05 10 1e
  assert.deepEqual(Array.from(body), [0x08, 0x05, 0x10, 0x1e]);
});

test("buzzer_play with empty body encodes a stop (proto3 default)", () => {
  const enc = codec.encodeDownlink({ data: { seq: 2, command: "buzzer_play" } });
  assert.equal(enc.errors.length, 0, "encode errors: " + enc.errors);
  // seq=2 (0x08 0x02) + field 28 length-delimited with length 0 (0xe2 0x01 0x00).
  assert.equal(toHex(enc.bytes), "0802e20100");
});

// --- Fix for a systemic decoder hang: _pbReadVarint(bytes, offset) with
// offset >= bytes.length returned {next: offset} UNCHANGED (loop body never
// ran), so any `while (p < end)` loop bounded by a length taken FROM the
// payload (not bytes.length) spun forever once `end` exceeded the real
// buffer — a malformed/truncated frame could hang the ChirpStack/TTN sandbox,
// which has no execution timeout. Every such loop now also requires
// `idx < bytes.length`. These three cases exercise three different decoders
// (Info, set_param SetParam-in-DownlinkCommand, and the fPort-3 AlarmEvent
// decoder) with a declared length that overruns the real byte array.
test("decodeUplink (fPort 85, Info) terminates on an over-length declared field (#hang)", () => {
  // version 0x01 stripped; remaining bytes: tag=0x1a (field 3 Info, wire 2),
  // len=0x7f (127) -- far beyond the 0-byte remainder that actually follows.
  const got = codec.decodeUplink({ bytes: hex("011a7f"), fPort: 85 });
  assert.equal(typeof got, "object");
  assert.equal(typeof got.data, "object");
  // Info decode bails out immediately (no bytes left) but still returns the
  // well-formed Info skeleton with its defaults.
  assert.equal(got.data.info.fw_major, 0);
  assert.deepEqual(got.data.info.active_alarms, []);
});

test("decodeDownlink (fPort 85, set_param body) terminates on an over-length declared field (#hang)", () => {
  // tag=0x12 (field 2 set_param, wire 2), len=0x7f (127) -- no bytes follow.
  const got = codec.decodeDownlink({ bytes: hex("127f"), fPort: 85 });
  assert.equal(got.data.command, "set_param");
  assert.deepEqual(got.data.set_param, {});
});

test("decodeUplink (fPort 3, AlarmEvent) terminates on an over-length declared field (#hang)", () => {
  // version 0x01 stripped; remaining bytes: tag=0x1a (field 3 events, wire 2),
  // len=0x7f (127) -- no bytes follow, so the AlarmEvent decodes to defaults.
  const got = codec.decodeUplink({ bytes: hex("011a7f"), fPort: 3 });
  assert.equal(got.data.alarms.length, 1);
  assert.equal(got.data.alarms[0].source, "onboard");
  assert.equal(got.data.alarms[0].type, "none");
});
