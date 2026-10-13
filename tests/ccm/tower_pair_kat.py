#!/usr/bin/env python3
"""TOWER OTA pairing + fsk packet KAT (issue #490): prints tower_pair_kat.json.

The 3-way JOIN of tower-firmware src/radio/net/pairing.rs, all frames under the
public PAIRING_KEY b"TOWER-PAIR-KEY!\\0":
JOIN_REQ     = type 4, src=addr, dest=0,  counter=c,   payload addr(4 LE)
JOIN_RESP    = type 5, src=gw,   dest=addr, counter=g, payload key(16) challenge(4 LE)
JOIN_CONFIRM = type 6, src=addr, dest=gw, counter=c+1, payload addr(4 LE) challenge(4 LE)
Frame/CCM exactly as tower_frame_kat.json (14 B header = AAD, nonce src|counter|0*5, 8 B tag).

fsk packet (SPIRIT1 on the stock TOWER, docs/radio.md): len(1) | frame | CRC16
(poly 0x1021, seed 0xFFFF, over len+frame, big-endian), then CC1101 PN9
whitening (seed 0x1FF) over all of it.
"""
import json
import struct

from Crypto.Cipher import AES

PAIRING_KEY = b"TOWER-PAIR-KEY!\x00"
DEV_EUI = bytes.fromhex("5876070000000413")
ADDR = int.from_bytes(DEV_EUI[4:], "big")
GW = 0x2D9B924E
COUNTER = 88
GW_COUNTER = 7
NODE_KEY = bytes.fromhex("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf")
CHALLENGE = 0xC0FFEE11


def frame(key, ftype, src, dest, counter, pt):
    hdr = bytes([(1 << 5) | ftype, 0]) + struct.pack("<III", src, dest, counter)
    nonce = struct.pack("<II", src, counter) + bytes(5)
    c = AES.new(key, AES.MODE_CCM, nonce=nonce, mac_len=8)
    c.update(hdr)
    ct, tag = c.encrypt_and_digest(pt)
    return hdr + ct + tag


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def pn9(data):
    out = bytearray()
    k = 0x1FF
    for b in data:
        out.append(b ^ (k & 0xFF))
        for _ in range(8):
            k = (k >> 1) | (((k ^ (k >> 5)) & 1) << 8)
    return bytes(out)


def fsk_wrap(f):
    body = bytes([len(f)]) + f
    return pn9(body + struct.pack(">H", crc16(body)))


req = frame(PAIRING_KEY, 4, ADDR, 0, COUNTER, struct.pack("<I", ADDR))
resp = frame(PAIRING_KEY, 5, GW, ADDR, GW_COUNTER, NODE_KEY + struct.pack("<I", CHALLENGE))
conf = frame(PAIRING_KEY, 6, ADDR, GW, COUNTER + 1, struct.pack("<II", ADDR, CHALLENGE))
out = {
    "_comment": __doc__.strip(),
    "dev_eui": DEV_EUI.hex(), "addr": ADDR, "gw": GW, "counter": COUNTER,
    "gw_counter": GW_COUNTER, "node_key": NODE_KEY.hex(), "challenge": CHALLENGE,
    "join_req": req.hex(), "join_resp": resp.hex(), "join_confirm": conf.hex(),
    "join_req_fsk": fsk_wrap(req).hex(),
}
print(json.dumps(out, indent=2))
