// Reference receiver / decoder for the raw-LoRa P2P transport (#118 / #123).
//
// Mirrors the on-air frame app_p2p.c produces:
//   [ net_id(4 BE) | dev_addr(2 BE) | frame_type(1) | counter(4 BE) ]  11 B header
//   [ AES-CCM ciphertext (= plaintext) ] [ AES-CCM tag (4 B) ]
// The header is cleartext and fed as AAD; the body is AES-CCM (AES-128) with the
// shared p2p_key. The CCM nonce is counter(4 BE) || dev_addr(2 BE) ||
// frame_type(1) || direction(1) || zeros(5) = 13 B (direction 0 = device TX).
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

// Decode one raw P2P frame. `frame` is a Buffer / hex string / byte array, `key`
// the 16-byte AES-CCM key (Buffer or hex). Returns the parsed header, the
// decrypted body and (for known frame types) the decoded payload. Throws if the
// frame is malformed or the AES-CCM tag does not verify.
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
// the wire Buffer.
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
  buildNonce,
  P2P_HDR_LEN,
  P2P_TAG_LEN,
  P2P_NONCE_LEN,
  P2P_DIR_TX,
  P2P_DIR_RX,
};
