// Reference receiver / decoder for the TOWER P2P transport (doc/plan/470, doc/p2p.md).
//
// Mirrors the on-air frame app_radio_p2p.c produces and receives:
//   [ ver_type(1) | flags(1) | src(4 LE) | dest(4 LE) | counter(4 LE) ]  14 B header
//   [ AES-128-CCM ciphertext (= plaintext length) ] [ CCM tag (8 B) ]
// ver_type = version(3 bits, 1) << 5 | type (Data 0, Ack 1); flags bit 0 CONFIRMED.
// The whole header is the CCM AAD; the nonce is src(4 LE) || counter(4 LE) ||
// bulk_idx(3 LE, 0) || 0x0000 = 13 B. There is no direction byte: the two
// directions differ by src. The node's address is the low 32 bits of its DevEUI,
// the gateway's the network's net_id.
//
// An ACK payload is acked(4 LE) | rssi(i8) | flags(PENDING bit 0); any payload of
// at least 4 B is an ACK. A Data payload carries one of two STICKER envelopes:
//   0x81 | port(1) | LoRaWAN fPort payload   telemetry 2, alarm 3, answers 85,
//                                            commands 86 (downlink)
//   0x91 | { cmd(1) | len(1) | value } x n   control TLVs (Capabilities, Hello,
//                                            join, LinkCheck, Time, ...)
// The 0x81 body is the exact payload LoRaWAN would carry on that fPort, so it is
// decoded by the ttn.js fPort decoders.
//
// Keys: join_key seals the JoinRequest / JoinAccept, session_key every frame after
// the join. Both are AES-128-CMAC of the device's LoRaWAN OTAA AppKey (app_key):
//   join_key    = CMAC(app_key, "HIO-TWR-JOIN" || 0x01 || dev_eui(8) || pad to 32 B)
//   session_key = CMAC(app_key, "HIO-TWR-SES" || 0x01 || dev_nonce(4 BE) ||
//                      central_nonce(4 BE) || dev_eui(8) || pad to 32 B)
// dev_eui MSB-first, as the hex string reads. Pinned against the firmware and the
// central by tests/ccm/tower_join_kat.json and tests/ccm/tower_frame_kat.json.
//
// Zero-dependency (Node >= 18 built-in crypto). Run the tests with `node --test`.

"use strict";

const crypto = require("crypto");
const ttn = require("./ttn.js");

const TWR_VERSION = 1;
const TWR_HDR_LEN = 14;
const TWR_TAG_LEN = 8;
const TWR_NONCE_LEN = 13;
const TWR_TYPE_DATA = 0;
const TWR_TYPE_ACK = 1;
const TWR_FLAG_CONFIRMED = 0x01;
const TWR_ACK_PENDING = 0x01;
const TWR_FRAME_MAX = 100; // the node's MTU (Capabilities)

const ENV_DATA = 0x81;
const ENV_CTRL = 0x91;

const JOIN_KEY_LABEL = "HIO-TWR-JOIN";
const SESSION_KEY_LABEL = "HIO-TWR-SES";

const TYPE_NAMES = { [TWR_TYPE_DATA]: "data", [TWR_TYPE_ACK]: "ack" };

const PORT_NAMES = {
  2: "telemetry",
  3: "alarm",
  85: "response",
  86: "command",
};

const CTRL = {
  CAPABILITIES: 0x01,
  HELLO: 0x02,
  DETACH: 0x03,
  REJOIN_REQ: 0x04,
  JOIN_REQ: 0x07,
  JOIN_ACCEPT: 0x08,
  LINK_CHECK: 0x10,
  TIME: 0x20,
};

const CTRL_NAMES = {
  [CTRL.CAPABILITIES]: "capabilities",
  [CTRL.HELLO]: "hello",
  [CTRL.DETACH]: "detach",
  [CTRL.REJOIN_REQ]: "rejoin_request",
  [CTRL.JOIN_REQ]: "join_request",
  [CTRL.JOIN_ACCEPT]: "join_accept",
  [CTRL.LINK_CHECK]: "link_check",
  [CTRL.TIME]: "time",
};

function asBuffer(b) {
  if (typeof b === "string") {
    return Buffer.from(b, "hex");
  }
  return Buffer.isBuffer(b) ? b : Buffer.from(b || []);
}

function asKey(key) {
  if (typeof key === "string") {
    key = Buffer.from(key, "hex");
  }
  if (!Buffer.isBuffer(key) || key.length !== 16) {
    throw new Error("key must be 16 bytes (32 hex digits)");
  }
  return key;
}

// Coerce a DevEUI to its 8 raw bytes, MSB-first.
//
// Deliberately strict: a 4-byte serial (the identity before #417) must fail here
// rather than be zero-extended into a key that differs from the central's -- the
// join would succeed and every data frame after it fail to decrypt with no clue why.
function asDevEui(devEui) {
  if (typeof devEui === "string") {
    devEui = Buffer.from(devEui.replace(/[:-]/g, ""), "hex");
  } else if (!Buffer.isBuffer(devEui)) {
    if (typeof devEui === "number") {
      throw new TypeError("dev_eui must be 8 bytes (MSB-first), not a number -- see #417");
    }
    devEui = Buffer.from(devEui);
  }
  if (devEui.length !== 8) {
    throw new RangeError(`dev_eui must be 8 bytes (MSB-first), got ${devEui.length}`);
  }
  return devEui;
}

// The node's TOWER address: the low 32 bits of the DevEUI (plan §6.1).
function nodeAddr(devEui) {
  return asDevEui(devEui).readUInt32BE(4);
}

// Single-block AES-128 ECB forward encrypt, no padding: `block` must be 16 B.
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

// GF(2^128) doubling for the CMAC subkeys, Rb = 0x87 (RFC 4493 §2.3). Mirrors
// app_ccm.c's double_gf128().
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

// AES-128 CMAC (NIST SP 800-38B / RFC 4493), any message length. Mirrors
// app_ccm.c's app_ccm_cmac().
function aes128Cmac(key, msg) {
  key = asKey(key);
  msg = asBuffer(msg);

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

// CMAC(app_key, label || 0x01 || fields || zero pad to 32 B), app_radio_p2p.c's
// derive_key().
function deriveKey(appKey, label, fields) {
  const block = Buffer.alloc(32);
  const l = Buffer.from(label, "ascii");

  l.copy(block, 0);
  block[l.length] = 0x01;
  fields.copy(block, l.length + 1);
  return aes128Cmac(appKey, block);
}

function deriveJoinKey(appKey, devEui) {
  return deriveKey(asKey(appKey), JOIN_KEY_LABEL, asDevEui(devEui));
}

function deriveSessionKey(appKey, devNonce, centralNonce, devEui) {
  const fields = Buffer.alloc(16);

  fields.writeUInt32BE(devNonce >>> 0, 0);
  fields.writeUInt32BE(centralNonce >>> 0, 4);
  asDevEui(devEui).copy(fields, 8);
  return deriveKey(asKey(appKey), SESSION_KEY_LABEL, fields);
}

// src(4 LE) || counter(4 LE) || bulk_idx(3 LE) || 0x0000.
function towerNonce(src, counter, bulkIdx) {
  const n = Buffer.alloc(TWR_NONCE_LEN);

  n.writeUInt32LE(src >>> 0, 0);
  n.writeUInt32LE(counter >>> 0, 4);
  n.writeUIntLE((bulkIdx || 0) & 0xffffff, 8, 3);
  return n;
}

function encodeHeader(h) {
  const out = Buffer.alloc(TWR_HDR_LEN);

  out[0] = ((TWR_VERSION << 5) | (h.type & 0x1f)) & 0xff;
  out[1] = (h.flags || 0) & 0xff;
  out.writeUInt32LE(h.src >>> 0, 2);
  out.writeUInt32LE(h.dest >>> 0, 6);
  out.writeUInt32LE(h.counter >>> 0, 10);
  return out;
}

// The cleartext header of a frame, without opening it (a listener has no key).
function parseHeader(frame) {
  frame = asBuffer(frame);
  if (frame.length < TWR_HDR_LEN + TWR_TAG_LEN) {
    throw new Error(`frame too short: ${frame.length} B`);
  }
  if (frame[0] >> 5 !== TWR_VERSION) {
    throw new Error(`not a TOWER v${TWR_VERSION} frame (ver_type 0x${frame[0].toString(16)})`);
  }

  const type = frame[0] & 0x1f;
  const flags = frame[1];

  return {
    type,
    typeName: TYPE_NAMES[type] || "unknown",
    flags,
    confirmed: (flags & TWR_FLAG_CONFIRMED) !== 0,
    src: frame.readUInt32LE(2),
    dest: frame.readUInt32LE(6),
    counter: frame.readUInt32LE(10),
  };
}

// Seal a frame: { key, type (default Data), flags, src, dest, counter, payload }.
function encodeTowerFrame(p) {
  const key = asKey(p.key);
  const pt = asBuffer(p.payload);
  const h = { type: p.type != null ? p.type : TWR_TYPE_DATA, ...p };
  const header = encodeHeader(h);
  const cipher = crypto.createCipheriv("aes-128-ccm", key, towerNonce(h.src, h.counter), {
    authTagLength: TWR_TAG_LEN,
  });

  cipher.setAAD(header, { plaintextLength: pt.length });
  const ct = Buffer.concat([cipher.update(pt), cipher.final()]);
  return Buffer.concat([header, ct, cipher.getAuthTag()]);
}

// acked(4 LE) | rssi(i8) | flags(PENDING bit 0); fields past the first 4 B optional.
function parseAck(pt) {
  if (pt.length < 4) {
    throw new Error(`ACK payload too short: ${pt.length} B`);
  }
  return {
    acked: pt.readUInt32LE(0),
    rssi: pt.length > 4 ? pt.readInt8(4) : null,
    pending: pt.length > 5 && (pt[5] & TWR_ACK_PENDING) !== 0,
  };
}

// The TLV list of a 0x91 payload (after the envelope byte). An unknown cmd is
// skipped by its length; an entry running past the end throws.
function parseTlvs(buf) {
  buf = asBuffer(buf);
  const out = [];
  let off = 0;

  while (off < buf.length) {
    if (buf.length - off < 2 || buf.length - off - 2 < buf[off + 1]) {
      throw new Error(`control TLV malformed at byte ${off}`);
    }
    const cmd = buf[off];
    const value = buf.subarray(off + 2, off + 2 + buf[off + 1]);

    out.push({ cmd, name: CTRL_NAMES[cmd] || "unknown", value, fields: decodeCtrl(cmd, value) });
    off += 2 + value.length;
  }
  return out;
}

// The fields of one control value (doc/plan/470 §8.2), or null for an empty or
// unknown one. Requests and answers share an ID; the length tells them apart.
function decodeCtrl(cmd, v) {
  switch (cmd) {
    case CTRL.CAPABILITIES:
      if (v.length < 12) {
        return null;
      }
      return {
        protoVersion: v[0],
        mtu: v[1],
        profiles: { fsk: (v[2] & 0x01) !== 0, lora: (v[2] & 0x02) !== 0 },
        cmdBitmap: Buffer.from(v.subarray(3, 11)).toString("hex"),
        powerClass: v[11],
      };
    case CTRL.HELLO:
      if (v.length < 9) {
        return null;
      }
      return {
        sessionId: v.readUInt32LE(0),
        resetReason: v[4],
        fwVersion: `${v[5]}.${v[6]}.${v[7]}`,
      };
    case CTRL.DETACH:
    case CTRL.REJOIN_REQ:
      return v.length ? { reason: v[0] } : null;
    case CTRL.JOIN_REQ:
      if (v.length < 14) {
        return null;
      }
      return {
        productType: v[0],
        protoVersion: v[1],
        devEui: Buffer.from(v.subarray(2, 10)).toString("hex"),
        fwVersion: `${v[10]}.${v[11]}.${v[12]}`,
      };
    case CTRL.JOIN_ACCEPT:
      if (v.length < 13) {
        return null;
      }
      return {
        netId: v.readUInt32LE(0),
        centralNonce: v.readUInt32LE(4),
        rxDelay: v[8],
        txPower: v[9], // 0 = no assignment
      };
    case CTRL.LINK_CHECK:
      if (v.length < 4) {
        return null; // LinkCheckReq
      }
      return { rssi: v.readInt8(0), snr: v.readInt8(1), margin: v.readInt8(2), gwCount: v[3] };
    case CTRL.TIME:
      if (v.length < 9) {
        return null; // TimeReq
      }
      return { unix: v.readUInt32LE(0), frac: v[4], reqCounter: v.readUInt32LE(5) };
    default:
      return null;
  }
}

// A Data payload's STICKER envelope.
function decodeEnvelope(pt) {
  pt = asBuffer(pt);
  if (pt.length >= 2 && pt[0] === ENV_DATA) {
    const port = pt[1];
    const body = pt.subarray(2);
    const out = { envelope: "data", port, portName: PORT_NAMES[port] || "unknown", body };

    if (port === 2 || port === 3 || port === 85) {
      try {
        out.data = ttn.decodeUplink({ fPort: port, bytes: Array.from(body) }).data;
      } catch (e) {
        out.decodeError = String(e);
      }
    }
    return out;
  }
  if (pt.length >= 1 && pt[0] === ENV_CTRL) {
    return { envelope: "control", tlvs: parseTlvs(pt.subarray(1)) };
  }
  return { envelope: "unknown" };
}

// Open one frame under `key` (join_key for the join frames, session_key after).
// Returns the header, the plaintext and, by type, `ack` or `envelope`. Throws if
// the frame is malformed or the tag does not verify.
function decodeTowerFrame(frame, key) {
  frame = asBuffer(frame);
  key = asKey(key);

  const h = parseHeader(frame);
  const ctLen = frame.length - TWR_HDR_LEN - TWR_TAG_LEN;
  const decipher = crypto.createDecipheriv("aes-128-ccm", key, towerNonce(h.src, h.counter), {
    authTagLength: TWR_TAG_LEN,
  });

  decipher.setAuthTag(frame.subarray(TWR_HDR_LEN + ctLen));
  decipher.setAAD(frame.subarray(0, TWR_HDR_LEN), { plaintextLength: ctLen });
  const payload = Buffer.concat([
    decipher.update(frame.subarray(TWR_HDR_LEN, TWR_HDR_LEN + ctLen)),
    decipher.final(), // throws on a bad tag
  ]);
  const out = { ...h, payload };

  if (h.type === TWR_TYPE_ACK) {
    out.ack = parseAck(payload);
  } else if (h.type === TWR_TYPE_DATA) {
    out.envelope = decodeEnvelope(payload);
  }
  return out;
}

module.exports = {
  decodeTowerFrame,
  encodeTowerFrame,
  parseHeader,
  parseAck,
  parseTlvs,
  decodeEnvelope,
  deriveJoinKey,
  deriveSessionKey,
  nodeAddr,
  towerNonce,
  aes128Cmac,
  TWR_VERSION,
  TWR_HDR_LEN,
  TWR_TAG_LEN,
  TWR_NONCE_LEN,
  TWR_TYPE_DATA,
  TWR_TYPE_ACK,
  TWR_FLAG_CONFIRMED,
  TWR_ACK_PENDING,
  TWR_FRAME_MAX,
  ENV_DATA,
  ENV_CTRL,
  CTRL,
  JOIN_KEY_LABEL,
  SESSION_KEY_LABEL,
};
