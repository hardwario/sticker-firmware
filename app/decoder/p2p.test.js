// Tests for the raw-LoRa P2P frame codec + key derivation (p2p.js).
// Run: `node --test` (Node >= 18, zero dependencies).
//
// The frame format and AES-CCM nonce/AAD layout here are the wire contract with
// app_p2p.c -- a round-trip through encode/decode pins them so the firmware and
// the reference receiver can never silently drift apart. Same for the
// deriveJoinKey() known-answer vector against app_p2p.c's derive_join_key().

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const p2p = require("./p2p.js");
const ttn = require("./ttn.js");

const hex = (h) => Buffer.from(h, "hex");
const toHex = (b) => Buffer.from(b).toString("hex");

// 16-byte test key -- phase 1 uses this directly as the frame key (stands in for
// a derived join_key in tests that only exercise the wire format, not the KDF).
const KEY = "000102030405060708090a0b0c0d0e0f";

test("nonce layout: counter(4 BE) || dev_addr(2 BE) || type(1) || dir(1) || 0*5", () => {
  const n = p2p.buildNonce(0x01020304, 0xbeef, 2, p2p.P2P_DIR_TX);
  assert.equal(toHex(n), "01020304beef0200" + "0000000000");
  assert.equal(n.length, p2p.P2P_NONCE_LEN);
});

test("deriveJoinKey: known-answer vector (cross-checked against a reference AES-CMAC implementation)", () => {
  // block = "HIO-P2P-JOIN" (12 B, ASCII) || serial_number(4 BE) = 16 B exactly;
  // join_key = AES-128-CMAC(secret_key, block). Vector computed independently
  // via PyCryptodome's CMAC.new(key, ciphermod=AES) (#118 phase 2, replacing
  // the phase-1 plain-ECB PRF after a crypto review).
  const secretKey = KEY;
  const serialNumber = 0x12345678;
  const joinKey = p2p.deriveJoinKey(secretKey, serialNumber);

  assert.equal(joinKey.length, 16);
  assert.equal(toHex(joinKey), "3815437f5476f37b7744a3c5b0935867");
});

test("aes128Cmac: RFC 4493 known-answer vectors", () => {
  const key = "2b7e151628aed2a6abf7158809cf4f3c";
  const msg = hex(
    "6bc1bee22e409f96e93d7e117393172a" +
      "ae2d8a571e03ac9c9eb76fac45af8e51" +
      "30c81c46a35ce411e5fbc1191a0a52ef" +
      "f69f2445df4f9b17ad2b417be66c3710"
  );

  assert.equal(toHex(p2p.aes128Cmac(key, msg.subarray(0, 0))), "bb1d6929e95937287fa37d129b756746");
  assert.equal(toHex(p2p.aes128Cmac(key, msg.subarray(0, 16))), "070a16b46b4d4144f79bdd9dd04a287c");
  assert.equal(toHex(p2p.aes128Cmac(key, msg.subarray(0, 40))), "dfa66747de9ae63030ca32611497c827");
  assert.equal(toHex(p2p.aes128Cmac(key, msg)), "51f0bebf7e3b9d92fc49741779363cfe");
});

test("deriveJoinKey: distinct secret_key or serial_number gives a distinct join_key", () => {
  const a = p2p.deriveJoinKey(KEY, 0x12345678);
  const b = p2p.deriveJoinKey(KEY, 0x12345679); // serial +1
  const c = p2p.deriveJoinKey("ffffffffffffffffffffffffffffffff", 0x12345678); // key differs

  assert.notEqual(toHex(a), toHex(b));
  assert.notEqual(toHex(a), toHex(c));
  // Deterministic: same inputs always give the same output.
  assert.equal(toHex(a), toHex(p2p.deriveJoinKey(KEY, 0x12345678)));
});

test("telemetry round-trip: header parsed, body recovered and decoded", () => {
  // A real fPort-2 Telemetry body (version byte + protobuf) reused from ttn.test.js.
  const body = "0108641000900100980100";
  const frame = p2p.encodeP2pFrame({
    netId: 0, // phase 1: fixed pre-join value (doc/p2p.md §5.3)
    devAddr: 0,
    frameType: 2,
    counter: 7,
    body,
    key: KEY,
  });

  // Header is 11 B cleartext; body is the same length as plaintext; +4 B tag.
  assert.equal(frame.length, p2p.P2P_HDR_LEN + hex(body).length + p2p.P2P_TAG_LEN);
  assert.equal(toHex(frame.subarray(0, p2p.P2P_HDR_LEN)), "000000000000" + "0200000007");

  const got = p2p.decodeP2pFrame(frame, KEY);
  assert.equal(got.netId, 0);
  assert.equal(got.devAddr, 0);
  assert.equal(got.frameType, 2);
  assert.equal(got.frameTypeName, "telemetry");
  assert.equal(got.counter, 7);
  assert.equal(toHex(got.body), body);

  // The body decodes exactly as the LoRaWAN fPort-2 path would.
  assert.deepEqual(got.data, ttn.decodeUplink({ fPort: 2, bytes: Array.from(hex(body)) }).data);
});

test("frame type maps to fPort decoder name", () => {
  for (const [type, name] of [[2, "telemetry"], [3, "alarm"], [85, "response"]]) {
    const frame = p2p.encodeP2pFrame({
      netId: 0,
      devAddr: 0,
      frameType: type,
      counter: 1,
      body: "00",
      key: KEY,
    });
    assert.equal(p2p.decodeP2pFrame(frame, KEY).frameTypeName, name);
  }
});

test("tampered ciphertext fails authentication", () => {
  const frame = p2p.encodeP2pFrame({
    netId: 0,
    devAddr: 0,
    frameType: 2,
    counter: 1,
    body: "0108641000900100980100",
    key: KEY,
  });
  frame[p2p.P2P_HDR_LEN] ^= 0x01; // flip a ciphertext bit
  assert.throws(() => p2p.decodeP2pFrame(frame, KEY));
});

test("tampered header (AAD) fails authentication", () => {
  const frame = p2p.encodeP2pFrame({
    netId: 0,
    devAddr: 0,
    frameType: 2,
    counter: 1,
    body: "0108641000900100980100",
    key: KEY,
  });
  frame[6] ^= 0x01; // flip the frame_type (AAD) byte -- net_id/dev_addr are 0 in phase 1
  assert.throws(() => p2p.decodeP2pFrame(frame, KEY));
});

test("wrong key fails authentication", () => {
  const frame = p2p.encodeP2pFrame({
    netId: 0,
    devAddr: 0,
    frameType: 2,
    counter: 1,
    body: "0108641000900100980100",
    key: KEY,
  });
  assert.throws(() => p2p.decodeP2pFrame(frame, "ffffffffffffffffffffffffffffffff"));
});

test("runt frame is rejected", () => {
  assert.throws(() => p2p.decodeP2pFrame(hex("deadbeef0042"), KEY));
});
