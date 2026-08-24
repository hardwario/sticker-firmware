// Reference receiver / decoder for the raw-LoRa P2P transport (#118, phase 1,
// doc/p2p.md).
//
// Mirrors the on-air frame app_p2p.c produces:
//   [ net_id(4 BE) | dev_addr(2 BE) | frame_type(1) | counter(4 BE) ]  11 B header
//   [ AES-CCM ciphertext (= plaintext) ] [ AES-CCM tag (4 B) ]
// The header is cleartext and fed as AAD; the body is AES-CCM (AES-128). The CCM
// nonce is counter(4 BE) || dev_addr(2 BE) || frame_type(1) || direction(1) ||
// zeros(5) = 13 B (direction 0 = device TX).
//
// Phase 1 (doc/p2p.md §13): net_id/dev_addr are the fixed pre-join value 0
// (§5.3's own JoinRequest convention); there is no manual p2p_key config
// parameter (§4) -- the frame key is `join_key`, derived on demand from the
// device's secret_key via deriveJoinKey() below (the same AES-CMAC PRF
// app_p2p.c's derive_join_key() computes, so both sides always agree without
// provisioning a separate secret).
//
// The decrypted body is the exact payload LoRaWAN would carry, so frame types
// reuse the LoRaWAN fPort decoders in ttn.js (telemetry=2, alarm=3, response=85).
//
// Zero-dependency (Node >= 18 built-in crypto). Run the tests with `node --test`.

"use strict";

const crypto = require("crypto");
const ttn = require("./ttn.js");

const P2P_HDR_LEN = 11;
const P2P_TAG_LEN = 4;
const P2P_NONCE_LEN = 13;
const P2P_DIR_TX = 0x00;
const P2P_DIR_RX = 0x01;
const P2P_JOIN_KEY_LABEL = "HIO-P2P-JOIN";

const FRAME_TYPE_NAMES = { 2: "telemetry", 3: "alarm", 85: "response", 86: "command" };

function buildNonce(counter, devAddr, frameType, dir) {
  const n = Buffer.alloc(P2P_NONCE_LEN);
  n.writeUInt32BE(counter >>> 0, 0);
  n.writeUInt16BE(devAddr & 0xffff, 4);
  n[6] = frameType & 0xff;
  n[7] = dir & 0xff;
  return n;
}

function asKey(key) {
  if (typeof key === "string") {
    key = Buffer.from(key, "hex");
  }
  if (!Buffer.isBuffer(key) || key.length !== 16) {
    throw new Error("p2p key must be 16 bytes (32 hex digits)");
  }
  return key;
}

// Single-block AES-128 ECB forward encrypt, no padding: `block` must be
// exactly 16 B. The one primitive both aes128Cmac() below and (on the device
// side) app_ccm_ecb_encrypt_block() are built from.
function aesEcbEncryptBlock(key, block) {
  const cipher = crypto.createCipheriv("aes-128-ecb", key, null);
  cipher.setAutoPadding(false);
  return Buffer.concat([cipher.update(block), cipher.final()]).subarray(0, 16);
}

function xor16(a, b) {
  const out = Buffer.alloc(16);
  for (let i = 0; i < 16; i++) {
    out[i] = a[i] ^ b[i];
  }
  return out;
}

// GF(2^128) "shift left 1, XOR Rb if the vacated MSB was 1" doubling used to
// derive both CMAC subkeys from L = E(K, 0^128). Rb = 0x87 (RFC 4493 §2.3).
// Mirrors app_ccm.c's double_gf128() exactly.
function doubleGf128(v) {
  const out = Buffer.alloc(16);
  const msb = (v[0] & 0x80) !== 0;

  for (let i = 0; i < 15; i++) {
    out[i] = ((v[i] << 1) | (v[i + 1] >>> 7)) & 0xff;
  }
  out[15] = (v[15] << 1) & 0xff;
  if (msb) {
    out[15] ^= 0x87;
  }
  return out;
}

// AES-128 CMAC (NIST SP 800-38B / RFC 4493). `key` a Buffer or 32-hex-digit
// string; `msg` a Buffer (any length, including 0). Mirrors app_ccm.c's
// app_ccm_cmac() exactly (same subkey derivation, same 10...0 padding on a
// partial final block).
function aes128Cmac(key, msg) {
  key = asKey(key);
  if (!Buffer.isBuffer(msg)) {
    msg = Buffer.from(msg);
  }

  const k1 = doubleGf128(aesEcbEncryptBlock(key, Buffer.alloc(16)));
  const k2 = doubleGf128(k1);

  const nBlocks = msg.length === 0 ? 1 : Math.ceil(msg.length / 16);
  const lastFull = msg.length !== 0 && msg.length % 16 === 0;

  let x = Buffer.alloc(16);
  for (let i = 0; i < nBlocks - 1; i++) {
    x = aesEcbEncryptBlock(key, xor16(x, msg.subarray(i * 16, i * 16 + 16)));
  }

  const lastOff = (nBlocks - 1) * 16;
  let lastBlock;
  if (lastFull) {
    lastBlock = xor16(msg.subarray(lastOff, lastOff + 16), k1);
  } else {
    const padded = Buffer.alloc(16);
    msg.subarray(lastOff).copy(padded, 0);
    padded[msg.length - lastOff] = 0x80;
    lastBlock = xor16(padded, k2);
  }
  return aesEcbEncryptBlock(key, xor16(x, lastBlock));
}

// join_key = AES128-CMAC(secret_key, "HIO-P2P-JOIN" || serial_number(4 BE)),
// doc/p2p.md §4 -- a proper PRF with domain-separated subkeys (#118 phase 2;
// phase 1 used a bare one-block AES-ECB PRF, replaced pre-ship after a crypto
// review). `secretKey` is a Buffer or 32-hex-digit string; `serialNumber` a
// uint32. Returns the 16-byte join_key as a Buffer. Matches app_p2p.c's
// derive_join_key() exactly (same label, same big-endian serial encoding).
function deriveJoinKey(secretKey, serialNumber) {
  secretKey = asKey(secretKey);

  const block = Buffer.alloc(16);
  const label = Buffer.from(P2P_JOIN_KEY_LABEL, "ascii");

  if (label.length + 4 > block.length) {
    throw new Error("join-key label too long"); // defensive; label is fixed at 12 B
  }
  label.copy(block, 0);
  block.writeUInt32BE(serialNumber >>> 0, label.length);

  return aes128Cmac(secretKey, block);
}

// Decode one raw P2P frame. `frame` is a Buffer / hex string / byte array, `key`
// the 16-byte AES-CCM key (Buffer or hex; phase 1: deriveJoinKey(secretKey,
// serialNumber)). Returns the parsed header, the decrypted body and (for known
// frame types) the decoded payload. Throws if the frame is malformed or the
// AES-CCM tag does not verify.
function decodeP2pFrame(frame, key, opts) {
  opts = opts || {};
  if (typeof frame === "string") {
    frame = Buffer.from(frame, "hex");
  } else if (!Buffer.isBuffer(frame)) {
    frame = Buffer.from(frame);
  }
  key = asKey(key);

  if (frame.length < P2P_HDR_LEN + P2P_TAG_LEN) {
    throw new Error(`frame too short: ${frame.length} B`);
  }

  const netId = frame.readUInt32BE(0);
  const devAddr = frame.readUInt16BE(4);
  const frameType = frame[6];
  const counter = frame.readUInt32BE(7);
  const header = frame.subarray(0, P2P_HDR_LEN);
  const ctLen = frame.length - P2P_HDR_LEN - P2P_TAG_LEN;
  const ct = frame.subarray(P2P_HDR_LEN, P2P_HDR_LEN + ctLen);
  const tag = frame.subarray(P2P_HDR_LEN + ctLen);
  const dir = opts.dir != null ? opts.dir : P2P_DIR_TX;

  const nonce = buildNonce(counter, devAddr, frameType, dir);
  const decipher = crypto.createDecipheriv("aes-128-ccm", key, nonce, {
    authTagLength: P2P_TAG_LEN,
  });
  decipher.setAuthTag(tag);
  decipher.setAAD(header, { plaintextLength: ctLen });
  const body = Buffer.concat([decipher.update(ct), decipher.final()]); // throws on bad tag

  const out = {
    netId,
    devAddr,
    frameType,
    frameTypeName: FRAME_TYPE_NAMES[frameType] || "unknown",
    counter,
    body,
  };

  // Frame types mirror LoRaWAN fPorts, so reuse the ttn.js payload decoders.
  if (frameType === 2 || frameType === 3 || frameType === 85) {
    try {
      out.data = ttn.decodeUplink({ fPort: frameType, bytes: Array.from(body) }).data;
    } catch (e) {
      out.decodeError = String(e);
    }
  }
  return out;
}

// Build a frame from a plaintext body (for tests and a software sender). Returns
// the wire Buffer. Phase 1: netId/devAddr default to 0 (the fixed pre-join
// value, doc/p2p.md §5.3) if not given.
function encodeP2pFrame(params) {
  const key = asKey(params.key);
  let body = params.body;
  if (typeof body === "string") {
    body = Buffer.from(body, "hex");
  } else if (!Buffer.isBuffer(body)) {
    body = Buffer.from(body);
  }
  const dir = params.dir != null ? params.dir : P2P_DIR_TX;

  const header = Buffer.alloc(P2P_HDR_LEN);
  header.writeUInt32BE((params.netId || 0) >>> 0, 0);
  header.writeUInt16BE((params.devAddr || 0) & 0xffff, 4);
  header[6] = params.frameType & 0xff;
  header.writeUInt32BE((params.counter || 0) >>> 0, 7);

  const nonce = buildNonce(params.counter || 0, params.devAddr || 0, params.frameType, dir);
  const cipher = crypto.createCipheriv("aes-128-ccm", key, nonce, {
    authTagLength: P2P_TAG_LEN,
  });
  cipher.setAAD(header, { plaintextLength: body.length });
  const ct = Buffer.concat([cipher.update(body), cipher.final()]);
  const tag = cipher.getAuthTag();
  return Buffer.concat([header, ct, tag]);
}

module.exports = {
  decodeP2pFrame,
  encodeP2pFrame,
  deriveJoinKey,
  aes128Cmac,
  buildNonce,
  P2P_HDR_LEN,
  P2P_TAG_LEN,
  P2P_NONCE_LEN,
  P2P_DIR_TX,
  P2P_DIR_RX,
};
