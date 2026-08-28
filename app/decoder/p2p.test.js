// Tests for the raw-LoRa P2P frame codec + key derivation (p2p.js).
// Run: `node --test` (Node >= 18, zero dependencies).
//
// The frame format and AES-CCM nonce/AAD layout here are the wire contract with
// app_p2p.c -- a round-trip through encode/decode pins them so the firmware and
// the reference receiver can never silently drift apart. Same for the
// deriveSessionKey()/joinTag() known-answer vectors against app_p2p.c's
// derive_session_key() and send_join_request()/recv_join_accept() (#118
// phase 2 revision, proximos-v2 MR!7 §7).

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const p2p = require("./p2p.js");
const ttn = require("./ttn.js");

const hex = (h) => Buffer.from(h, "hex");
const toHex = (b) => Buffer.from(b).toString("hex");

// 16-byte test key -- used directly as the data-plane frame key (stands in for
// a derived session_key in tests that only exercise the wire format, not the KDF).
const KEY = "000102030405060708090a0b0c0d0e0f";

test("nonce layout: counter(4 BE) || dev_addr(2 BE) || type(1) || dir(1) || 0*5", () => {
  const n = p2p.buildNonce(0x01020304, 0xbeef, 2, p2p.P2P_DIR_TX);
  assert.equal(toHex(n), "01020304beef0200" + "0000000000");
  assert.equal(n.length, p2p.P2P_NONCE_LEN);
});

test("deriveSessionKey: known-answer vector (cross-checked against a reference AES-CMAC implementation)", () => {
  // block = "HIO-P2P-SES" (11 B, ASCII) || 0x01 || dev_nonce(4 BE) ||
  // central_nonce(4 BE) || serial_number(4 BE) || zero-pad to 32 B (2 CMAC
  // blocks); session_key = AES-128-CMAC(app_key, block). Vector computed
  // independently via PyCryptodome's CMAC.new(key, ciphermod=AES) (#118
  // phase 2 revision: session_key now derives directly from the device's
  // LoRaWAN AppKey, with no join_key intermediate).
  const appKey = KEY;
  const devNonce = 0x11111111;
  const centralNonce = 0x22222222;
  const serialNumber = 0x12345678;
  const sessionKey = p2p.deriveSessionKey(appKey, devNonce, centralNonce, serialNumber);

  assert.equal(sessionKey.length, 16);
  assert.equal(toHex(sessionKey), "80887b1e99b61b0b19f42e457feb406f");
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

test("deriveSessionKey: distinct app_key or nonces gives a distinct session_key", () => {
  const a = p2p.deriveSessionKey(KEY, 0x11111111, 0x22222222, 0x12345678);
  const b = p2p.deriveSessionKey(KEY, 0x11111112, 0x22222222, 0x12345678); // dev_nonce +1
  const c = p2p.deriveSessionKey("ffffffffffffffffffffffffffffffff", 0x11111111, 0x22222222, 0x12345678); // key differs

  assert.notEqual(toHex(a), toHex(b));
  assert.notEqual(toHex(a), toHex(c));
  // Deterministic: same inputs always give the same output.
  assert.equal(toHex(a), toHex(p2p.deriveSessionKey(KEY, 0x11111111, 0x22222222, 0x12345678)));
});

test("joinTag: known-answer vectors for JoinRequest and JoinAccept (cross-checked against a reference AES-CMAC implementation)", () => {
  // JoinRequest: header(11 B) net_id=0|dev_addr=0|frame_type=0xF0|counter=7,
  // body(10 B) product_type=1|proto_version=1|serial=0x12345678|fw=1.2.3|reserved=0.
  // tag = AES-128-CMAC(app_key, "HIO-P2P-JOIN" || header || body). Vector
  // computed independently via PyCryptodome.
  const appKey = KEY;
  const headerReq = hex("000000000000f000000007");
  const bodyReq = hex("01011234567801020300");

  assert.equal(headerReq.length, p2p.P2P_HDR_LEN);
  assert.equal(bodyReq.length, 10);

  const tagReq = p2p.joinTag(appKey, p2p.P2P_JOIN_TAG_LABEL, Buffer.concat([headerReq, bodyReq]));

  assert.equal(tagReq.length, p2p.P2P_JOIN_TAG_LEN);
  assert.equal(toHex(tagReq), "fd02843b756c81a997882c28edaf28f8");

  // JoinAccept: header(11 B) net_id=0|dev_addr=0|frame_type=0xF1|counter=7
  // (echoed dev_nonce), body(15 B) net_id=100|dev_addr=5|central_nonce=
  // 0x22222222|rx1_delay_s=1|reserved=0. tag = AES-128-CMAC(app_key,
  // "HIO-P2P-ACC" || header || body).
  const headerAcc = hex("000000000000f100000007");
  const bodyAcc = hex("00000064000522222222" + "01" + "00000000");

  assert.equal(headerAcc.length, p2p.P2P_HDR_LEN);
  assert.equal(bodyAcc.length, 15);

  const tagAcc = p2p.joinTag(appKey, p2p.P2P_JOINACCEPT_TAG_LABEL, Buffer.concat([headerAcc, bodyAcc]));

  assert.equal(tagAcc.length, p2p.P2P_JOIN_TAG_LEN);
  assert.equal(toHex(tagAcc), "ca91d30e195f4f38bf2af38fc4ead0e0");

  // The two labels domain-separate: the same header+body under the WRONG
  // label must not produce either frame's real tag.
  assert.notEqual(
    toHex(p2p.joinTag(appKey, p2p.P2P_JOINACCEPT_TAG_LABEL, Buffer.concat([headerReq, bodyReq]))),
    toHex(tagReq)
  );
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
