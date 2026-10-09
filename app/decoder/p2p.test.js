// Tests for the TOWER P2P frame codec + key derivation (p2p.js).
// Run: `node --test` (Node >= 18, zero dependencies).
//
// The vectors are the wire contract with app_radio_p2p.c, the Northbridge and the
// central: tests/ccm/tower_frame_kat.json is generated from the TOWER Rust
// reference (frame, nonce, CCM), tests/ccm/tower_join_kat.json pins the key
// derivation and the TOWER-framed join (doc/plan/470 §4, §6.3). The same files
// feed the firmware's native ztests, so the three implementations can never
// silently drift apart.

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const p2p = require("./p2p.js");
const ttn = require("./ttn.js");

const hex = (h) => Buffer.from(h, "hex");
const toHex = (b) => Buffer.from(b).toString("hex");

const FRAME_KAT = require("../../tests/ccm/tower_frame_kat.json");
const JOIN_KAT = require("../../tests/ccm/tower_join_kat.json");

const FRAME_VECTORS = Object.entries(FRAME_KAT).filter(
  ([, v]) => v && typeof v === "object" && "frame" in v,
);

test("aes128Cmac: RFC 4493 §4 vectors", () => {
  const key = "2b7e151628aed2a6abf7158809cf4f3c";
  const msg = hex(
    "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51" +
      "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
  );

  assert.equal(toHex(p2p.aes128Cmac(key, Buffer.alloc(0))), "bb1d6929e95937287fa37d129b756746");
  assert.equal(toHex(p2p.aes128Cmac(key, msg.subarray(0, 16))), "070a16b46b4d4144f79bdd9dd04a287c");
  assert.equal(toHex(p2p.aes128Cmac(key, msg.subarray(0, 40))), "dfa66747de9ae63030ca32611497c827");
  assert.equal(toHex(p2p.aes128Cmac(key, msg)), "51f0bebf7e3b9d92fc49741779363cfe");
});

test("frame KAT: the fixture carries all vectors", () => {
  assert.ok(FRAME_VECTORS.length >= 9, `only ${FRAME_VECTORS.length} vectors`);
});

for (const [name, v] of FRAME_VECTORS) {
  test(`frame KAT ${name}: nonce, encode and decode`, () => {
    assert.equal(toHex(p2p.towerNonce(v.src, v.counter)), v.nonce);

    const frame = p2p.encodeTowerFrame({
      key: FRAME_KAT.key,
      type: v.frame_type,
      flags: v.flags,
      src: v.src,
      dest: v.dest,
      counter: v.counter,
      payload: v.plaintext,
    });
    assert.equal(toHex(frame), v.frame);
    assert.equal(frame.length, v.frame_len);
    assert.equal(toHex(frame.subarray(0, p2p.TWR_HDR_LEN)), v.header);
    assert.equal(toHex(frame.subarray(p2p.TWR_HDR_LEN, frame.length - p2p.TWR_TAG_LEN)), v.ciphertext);
    assert.equal(toHex(frame.subarray(frame.length - p2p.TWR_TAG_LEN)), v.tag);

    const d = p2p.decodeTowerFrame(v.frame, FRAME_KAT.key);
    assert.equal(d.type, v.frame_type);
    assert.equal(d.flags, v.flags);
    assert.equal(d.confirmed, (v.flags & p2p.TWR_FLAG_CONFIRMED) !== 0);
    assert.equal(d.src, v.src);
    assert.equal(d.dest, v.dest);
    assert.equal(d.counter, v.counter);
    assert.equal(toHex(d.payload), v.plaintext);
  });
}

test("frame KAT: ACK payload acked | rssi | PENDING", () => {
  const plain = p2p.decodeTowerFrame(FRAME_KAT.ack_no_pending.frame, FRAME_KAT.key);
  const pend = p2p.decodeTowerFrame(FRAME_KAT.ack_pending.frame, FRAME_KAT.key);

  assert.equal(plain.typeName, "ack");
  assert.deepEqual(plain.ack, { acked: 2, rssi: -57, pending: false });
  assert.deepEqual(pend.ack, { acked: 2, rssi: -57, pending: true });
  assert.equal(plain.src, FRAME_KAT.gw_addr);
  assert.equal(plain.dest, FRAME_KAT.node_addr);
});

test("parseAck: any payload of at least 4 B is an ACK, appended bytes ignored", () => {
  assert.deepEqual(p2p.parseAck(hex("07000000")), { acked: 7, rssi: null, pending: false });
  assert.deepEqual(p2p.parseAck(hex("07000000f0")), { acked: 7, rssi: -16, pending: false });
  assert.deepEqual(p2p.parseAck(hex("07000000f003aabb")), { acked: 7, rssi: -16, pending: true });
  assert.throws(() => p2p.parseAck(hex("070000")), /too short/);
});

test("frame KAT: data and control envelopes", () => {
  const up = p2p.decodeTowerFrame(FRAME_KAT.uplink_confirmed.frame, FRAME_KAT.key);
  assert.equal(up.envelope.envelope, "data");
  assert.equal(up.envelope.port, 2);
  assert.equal(up.envelope.portName, "telemetry");

  const dl = p2p.decodeTowerFrame(FRAME_KAT.downlink_data.frame, FRAME_KAT.key);
  assert.equal(dl.envelope.port, 0x55);
  assert.equal(dl.envelope.portName, "response");

  const tr = p2p.decodeTowerFrame(FRAME_KAT.uplink_control_timereq.frame, FRAME_KAT.key);
  assert.equal(tr.envelope.envelope, "control");
  assert.equal(tr.envelope.tlvs.length, 1);
  assert.equal(tr.envelope.tlvs[0].name, "time");
  assert.equal(tr.envelope.tlvs[0].fields, null); // TimeReq: empty
});

test("decodeTowerFrame: tamper, wrong key, runt and wrong version are rejected", () => {
  const v = FRAME_KAT.uplink_confirmed;
  const frame = hex(v.frame);

  for (const off of [0 + 1, 5, p2p.TWR_HDR_LEN, frame.length - 1]) {
    const bad = Buffer.from(frame);
    bad[off] ^= 0x01;
    assert.throws(() => p2p.decodeTowerFrame(bad, FRAME_KAT.key), `byte ${off}`);
  }
  assert.throws(() => p2p.decodeTowerFrame(frame, "ffeeddccbbaa99887766554433221100"));
  assert.throws(() => p2p.decodeTowerFrame(frame.subarray(0, 21), FRAME_KAT.key), /too short/);

  const v2 = Buffer.from(frame);
  v2[0] = (2 << 5) | (v2[0] & 0x1f);
  assert.throws(() => p2p.decodeTowerFrame(v2, FRAME_KAT.key), /not a TOWER v1 frame/);
  assert.throws(() => p2p.decodeTowerFrame(frame, "0001"), /16 bytes/);
});

test("parseHeader: readable without the key", () => {
  const h = p2p.parseHeader(FRAME_KAT.uplink_counter_high.frame);

  assert.equal(h.typeName, "data");
  assert.equal(h.confirmed, true);
  assert.equal(h.counter, 4294967294);
  assert.equal(h.src, FRAME_KAT.node_addr);
  assert.equal(h.dest, FRAME_KAT.gw_addr);
});

test("join KAT: address, join_key and session_key", () => {
  assert.equal(p2p.nodeAddr(JOIN_KAT.dev_eui), JOIN_KAT.addr);
  assert.equal(toHex(p2p.deriveJoinKey(JOIN_KAT.app_key, JOIN_KAT.dev_eui)), JOIN_KAT.join_key);
  assert.equal(
    toHex(
      p2p.deriveSessionKey(JOIN_KAT.app_key, JOIN_KAT.dev_nonce, JOIN_KAT.central_nonce, JOIN_KAT.dev_eui),
    ),
    JOIN_KAT.session_key,
  );
});

test("join KAT: JoinRequest under join_key", () => {
  const v = JOIN_KAT.join_request;
  const frame = p2p.encodeTowerFrame({ key: JOIN_KAT.join_key, ...v, payload: v.plaintext });

  assert.equal(toHex(frame), v.frame);
  assert.equal(frame.length, v.frame_len);

  const d = p2p.decodeTowerFrame(v.frame, JOIN_KAT.join_key);
  const tlv = d.envelope.tlvs[0];
  assert.equal(d.dest, 0);
  assert.equal(d.counter, JOIN_KAT.dev_nonce);
  assert.equal(tlv.name, "join_request");
  assert.deepEqual(tlv.fields, {
    productType: 1,
    protoVersion: 1,
    devEui: JOIN_KAT.dev_eui,
    fwVersion: "1.5.0",
  });
});

test("join KAT: JoinAccept under join_key, fields", () => {
  const v = JOIN_KAT.join_accept;
  const frame = p2p.encodeTowerFrame({ key: JOIN_KAT.join_key, ...v, payload: v.plaintext });

  assert.equal(toHex(frame), v.frame);
  assert.equal(frame.length, v.frame_len);

  const d = p2p.decodeTowerFrame(v.frame, JOIN_KAT.join_key);
  const tlv = d.envelope.tlvs[0];
  assert.equal(d.src, JOIN_KAT.net_id);
  assert.equal(d.counter, JOIN_KAT.dev_nonce); // echoes the JoinRequest
  assert.equal(tlv.name, "join_accept");
  assert.deepEqual(tlv.fields, {
    netId: JOIN_KAT.net_id,
    centralNonce: JOIN_KAT.central_nonce,
    rxDelay: 1,
    txPower: 14,
  });
});

test("join KAT: first uplink under session_key", () => {
  const v = JOIN_KAT.first_uplink;
  const frame = p2p.encodeTowerFrame({ key: JOIN_KAT.session_key, ...v, flags: 1, payload: v.plaintext });

  assert.equal(toHex(frame), v.frame);
  assert.equal(frame.length, v.frame_len);

  const d = p2p.decodeTowerFrame(v.frame, JOIN_KAT.session_key);
  assert.equal(d.envelope.port, 2);
  assert.deepEqual(d.envelope.data, ttn.decodeUplink({ fPort: 2, bytes: [0x08, 0x01] }).data);
  assert.throws(() => p2p.decodeTowerFrame(v.frame, JOIN_KAT.join_key));
});

test("deriveSessionKey: each input changes the key", () => {
  const base = toHex(p2p.deriveSessionKey(JOIN_KAT.app_key, 17, 1, JOIN_KAT.dev_eui));
  const variants = [
    p2p.deriveSessionKey(JOIN_KAT.app_key, 18, 1, JOIN_KAT.dev_eui),
    p2p.deriveSessionKey(JOIN_KAT.app_key, 17, 2, JOIN_KAT.dev_eui),
    p2p.deriveSessionKey(JOIN_KAT.app_key, 17, 1, "5876070000000414"),
    p2p.deriveSessionKey("ffeeddccbbaa99887766554433221100", 17, 1, JOIN_KAT.dev_eui),
  ].map(toHex);

  assert.equal(new Set([base, ...variants]).size, 5);
  assert.notEqual(base, JOIN_KAT.join_key);
});

test("asDevEui: strict 8 bytes, MSB-first (#417)", () => {
  assert.equal(p2p.nodeAddr("58:76:07:00:00:00:04:13"), 0x0413);
  assert.equal(p2p.nodeAddr(hex("5876070000000413")), 0x0413);
  assert.throws(() => p2p.nodeAddr("80d20413"), /8 bytes/);
  assert.throws(() => p2p.nodeAddr(2162190413), /not a number/);
  assert.throws(() => p2p.deriveJoinKey(JOIN_KAT.app_key, "80d20413"), /8 bytes/);
});

test("parseTlvs: an unknown cmd is skipped, a truncated entry throws", () => {
  const t = p2p.parseTlvs(hex("7f02aabb" + "2000" + "1000"));

  assert.deepEqual(
    t.map((e) => e.name),
    ["unknown", "time", "link_check"],
  );
  assert.equal(toHex(t[0].value), "aabb");
  assert.throws(() => p2p.parseTlvs(hex("2003aabb")), /malformed/);
  assert.throws(() => p2p.parseTlvs(hex("20")), /malformed/);
  assert.deepEqual(p2p.parseTlvs(Buffer.alloc(0)), []);
});

test("control answers: LinkCheckAns and TimeAns", () => {
  const t = p2p.parseTlvs(hex("1004c50a0c01" + "2009" + "80f1e66a" + "40" + "03000000"));

  assert.deepEqual(t[0].fields, { rssi: -59, snr: 10, margin: 12, gwCount: 1 });
  assert.deepEqual(t[1].fields, { unix: 0x6ae6f180, frac: 0x40, reqCounter: 3 });
});

test("control uplink: Capabilities | Hello as app_radio_p2p.c ctrl_build() lays them out", () => {
  // 0x91 | Caps(12): proto 1, MTU 100, profiles lora, cmd bitmap, power class
  // battery | Hello(9): session_id, reset_reason, fw 1.5.0, reserved.
  // | LinkCheckReq | TimeReq -- the full set, 30 B.
  const pt = hex(
    "91" +
      "010c" + "01" + "64" + "02" + "9e01010001000000" + "01" +
      "0209" + "78563412" + "03" + "010500" + "00" +
      "1000" +
      "2000",
  );
  const env = p2p.decodeEnvelope(pt);

  assert.equal(pt.length, 30);
  assert.equal(env.envelope, "control");
  assert.deepEqual(
    env.tlvs.map((e) => e.name),
    ["capabilities", "hello", "link_check", "time"],
  );
  assert.deepEqual(env.tlvs[0].fields, {
    protoVersion: 1,
    mtu: p2p.TWR_FRAME_MAX,
    profiles: { fsk: false, lora: true },
    cmdBitmap: "9e01010001000000",
    powerClass: 1,
  });
  assert.deepEqual(env.tlvs[1].fields, { sessionId: 0x12345678, resetReason: 3, fwVersion: "1.5.0" });

  // The bitmap marks exactly the IDs the node implements (bit id % 8 of byte id / 8).
  const bitmap = hex(env.tlvs[0].fields.cmdBitmap);
  const ids = [];
  for (let id = 0; id < 64; id++) {
    if (bitmap[id >> 3] & (1 << (id & 7))) {
      ids.push(id);
    }
  }
  assert.deepEqual(ids, [0x01, 0x02, 0x03, 0x04, 0x07, 0x08, 0x10, 0x20]);
});

test("decodeEnvelope: 0x81 ports go through the ttn.js fPort decoders; unknown envelope", () => {
  const cmd = p2p.decodeEnvelope(hex("8156aa"));
  assert.equal(cmd.portName, "command");
  assert.equal(cmd.data, undefined); // downlink: not decoded as an uplink

  assert.equal(p2p.decodeEnvelope(hex("42")).envelope, "unknown");
  assert.equal(p2p.decodeEnvelope(Buffer.alloc(0)).envelope, "unknown");
});
