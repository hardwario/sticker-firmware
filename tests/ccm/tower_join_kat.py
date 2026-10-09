#!/usr/bin/env python3
"""TOWER-framed join KAT (plan #470 §6.3): prints tower_join_kat.json.

join_key    = CMAC(app_key, "HIO-TWR-JOIN" 0x01 DevEUI(8 MSB) pad32)
session_key = CMAC(app_key, "HIO-TWR-SES" 0x01 dev_nonce(4 BE) central_nonce(4 BE) DevEUI(8) pad32)
JoinReq    = TOWER Data, src=addr, dest=0, counter=dev_nonce, key=join_key,
             payload 0x91 07 0e product_type(1) proto_version(1) DevEUI(8 MSB) fw_version(4 BE)
JoinAccept = TOWER Data, src=net_id, dest=addr, counter=dev_nonce (echo), key=join_key,
             payload 0x91 08 0d net_id(4 LE) central_nonce(4 LE) rx_delay(1) tx_power(1) reserved(3)
Frame/CCM exactly as tower_frame_kat.json (14 B header = AAD, nonce src|counter|000000|0000, 8 B tag).
"""
import json
import struct

from Crypto.Cipher import AES
from Crypto.Hash import CMAC

APP_KEY = bytes(range(16))
DEV_EUI = bytes.fromhex("5876070000000413")
ADDR = int.from_bytes(DEV_EUI[4:], "big")
NET_ID = 0x6D249662
DEV_NONCE = 17
CENTRAL_NONCE = 0xA1B2C3D4


def cmac(key, msg):
    msg = msg + bytes(32 - len(msg))
    return CMAC.new(key, msg=msg, ciphermod=AES).digest()


def frame(key, ftype, flags, src, dest, counter, pt):
    hdr = bytes([(1 << 5) | ftype, flags]) + struct.pack("<III", src, dest, counter)
    nonce = struct.pack("<II", src, counter) + bytes(5)
    c = AES.new(key, AES.MODE_CCM, nonce=nonce, mac_len=8)
    c.update(hdr)
    ct, tag = c.encrypt_and_digest(pt)
    return {"src": src, "dest": dest, "counter": counter, "plaintext": pt.hex(),
            "header": hdr.hex(), "nonce": nonce.hex(), "frame": (hdr + ct + tag).hex(),
            "frame_len": len(hdr) + len(ct) + 8}


join_key = cmac(APP_KEY, b"HIO-TWR-JOIN" + b"\x01" + DEV_EUI)
session_key = cmac(APP_KEY, b"HIO-TWR-SES" + b"\x01" + struct.pack(">II", DEV_NONCE, CENTRAL_NONCE) + DEV_EUI)
req_body = bytes([1, 1]) + DEV_EUI + struct.pack(">I", 0x01050000)
acc_body = struct.pack("<II", NET_ID, CENTRAL_NONCE) + bytes([1, 14]) + bytes(3)
out = {
    "_comment": __doc__.strip(),
    "app_key": APP_KEY.hex(), "dev_eui": DEV_EUI.hex(), "addr": ADDR, "net_id": NET_ID,
    "dev_nonce": DEV_NONCE, "central_nonce": CENTRAL_NONCE,
    "join_key": join_key.hex(), "session_key": session_key.hex(),
    "join_request": frame(join_key, 0, 0, ADDR, 0, DEV_NONCE, bytes([0x91, 0x07, len(req_body)]) + req_body),
    "join_accept": frame(join_key, 0, 0, NET_ID, ADDR, DEV_NONCE, bytes([0x91, 0x08, len(acc_body)]) + acc_body),
    "first_uplink": frame(session_key, 0, 1, ADDR, NET_ID, 1, bytes([0x81, 0x02, 0x08, 0x01])),
}
print(json.dumps(out, indent=2))
