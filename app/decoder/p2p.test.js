// Tests for the raw-LoRa P2P frame codec (p2p.js).
// Run: `node --test` (Node >= 18, zero dependencies).
//
// The frame format and AES-CCM nonce/AAD layout here are the wire contract with
// app_p2p.c — a round-trip through encode/decode pins them so the firmware and
// the reference receiver can never silently drift apart.

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const p2p = require("./p2p.js");
const ttn = require("./ttn.js");

const hex = (h) => Buffer.from(h, "hex");
const toHex = (b) => Buffer.from(b).toString("hex");

// 16-byte test key (matches a `config p2p-key` provisioning value).
const KEY = "000102030405060708090a0b0c0d0e0f";

test("nonce layout: counter(4 BE) || dev_addr(2 BE) || type(1) || dir(1) || 0*5", () => {
  const n = p2p.buildNonce(0x01020304, 0xbeef, 2, p2p.P2P_DIR_TX);
  assert.equal(toHex(n), "01020304beef0200" + "0000000000");
  assert.equal(n.length, p2p.P2P_NONCE_LEN);
});

test("telemetry round-trip: header parsed, body recovered and decoded", () => {
  // A real fPort-2 Telemetry body (version byte + protobuf) reused from ttn.test.js.
  const body = "0108641000900100980100";
  const frame = p2p.encodeP2pFrame({
    netId: 0xdeadbeef,
    devAddr: 0x0042,
    frameType: 2,
    counter: 7,
    body,
    key: KEY,
  });

  // Header is 11 B cleartext; body is the same length as plaintext; +4 B tag.
  assert.equal(frame.length, p2p.P2P_HDR_LEN + hex(body).length + p2p.P2P_TAG_LEN);
  assert.equal(toHex(frame.subarray(0, p2p.P2P_HDR_LEN)), "deadbeef00420200000007");

  const got = p2p.decodeP2pFrame(frame, KEY);
  assert.equal(got.netId, 0xdeadbeef);
  assert.equal(got.devAddr, 0x42);
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
      netId: 1,
      devAddr: 2,
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
    netId: 1,
    devAddr: 2,
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
    netId: 1,
    devAddr: 2,
    frameType: 2,
    counter: 1,
    body: "0108641000900100980100",
    key: KEY,
  });
  frame[0] ^= 0x01; // flip a net_id (AAD) bit
  assert.throws(() => p2p.decodeP2pFrame(frame, KEY));
});

test("wrong key fails authentication", () => {
  const frame = p2p.encodeP2pFrame({
    netId: 1,
    devAddr: 2,
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
