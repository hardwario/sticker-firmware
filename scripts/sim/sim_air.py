#!/usr/bin/env python3
#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#
"""STICKER simulator air peer: a LoRaWAN 1.0.x network server (EU868) and a
P2P hub (TOWER protocol over LoRa, sim_p2p.py) for STICKERs running on
native_sim (doc/simulator.md). The sync word tells the two apart: LoRaWAN
public 0x3444, P2P private 0x1424.

Each simulated node's SX1262 emulator sends every transmission here as a UDP
datagram (drivers/sim/sim_air.h). This server answers each of them with a
downlink or with NONE, joins nodes over OTAA, checks MICs, decrypts uplinks,
answers LinkCheckReq/DeviceTimeReq, decodes the payload with the
application's TTN decoder and sends queued downlinks.

Commands on stdin:
  down <deveui|*> <fport> <hex> [c]   queue a raw downlink (c = confirmed;
                                      P2P: 0x81 envelope, fPort 85 -> port 86)
  cmd <deveui|*> <json>               queue a DownlinkCommand through the
                                      decoder's encodeDownlink(), e.g.
                                      cmd * {"command":"get_info"}
  ctrl <deveui|*> <hex>               queue a P2P 0x91 TLV list, e.g. 3000
                                      (DevStatusReq), 040100 (RejoinReq)
  list                                show sessions and queued downlinks
"""

import argparse
import json
import os
import random
import socket
import struct
import subprocess
import sys
import threading
import time

import sim_p2p
from cryptography.hazmat.primitives import cmac
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

MAGIC = 0x4153
VERSION = 1
HELLO, PACKET, NONE = 1, 2, 3
GFSK, LORA = 0, 1
ASAP = 0xFFFFFFFF
HEADER = struct.Struct("<HBBIIIQIIIIBBBBBBHHbbhH")

LORAWAN_SYNC = 0x3444
JOIN_ACCEPT_DELAY1_US = 5_000_000
RX1_DELAY_S = 1
NET_ID = 0x000013
GPS_EPOCH_UNIX = 315964800
GPS_LEAP_SECONDS = 18

# Uplink MAC command payload lengths (LoRaWAN 1.0.4, 5.), by CID.
UP_MAC_LEN = {0x02: 0, 0x03: 1, 0x04: 0, 0x05: 1, 0x06: 2, 0x07: 1, 0x08: 0, 0x09: 0,
              0x0A: 1, 0x0D: 0}
UP_MAC_NAME = {0x02: "LinkCheckReq", 0x03: "LinkADRAns", 0x04: "DutyCycleAns",
               0x05: "RXParamSetupAns", 0x06: "DevStatusAns", 0x07: "NewChannelAns",
               0x08: "RXTimingSetupAns", 0x09: "TxParamSetupAns", 0x0A: "DlChannelAns",
               0x0D: "DeviceTimeReq"}


def aes_encrypt(key, block):
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return enc.update(block) + enc.finalize()


def aes_decrypt(key, block):
    dec = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return dec.update(block) + dec.finalize()


def aes_cmac(key, data):
    c = cmac.CMAC(algorithms.AES(key))
    c.update(data)
    return c.finalize()


def frm_crypt(key, devaddr, fcnt, direction, payload):
    """FRMPayload encryption (LoRaWAN 1.0.x, 4.3.3); symmetric."""
    out = bytearray()
    for i in range((len(payload) + 15) // 16):
        a = bytes([1, 0, 0, 0, 0, direction]) + struct.pack("<II", devaddr, fcnt) + bytes([0, i + 1])
        s = aes_encrypt(key, a)
        chunk = payload[16 * i:16 * i + 16]
        out += bytes(x ^ y for x, y in zip(chunk, s))
    return bytes(out)


def data_mic(key, devaddr, fcnt, direction, msg):
    b0 = bytes([0x49, 0, 0, 0, 0, direction]) + struct.pack("<II", devaddr, fcnt) + bytes([0, len(msg)])
    return aes_cmac(key, b0 + msg)[:4]


class Frame:
    """One sim_air datagram."""

    FIELDS = ("magic", "version", "type", "seq", "ref_seq", "delay_us", "time_us", "freq_hz",
              "bw_hz", "bitrate", "fdev_hz", "modem", "sf", "cr", "iq_inverted", "crc_on",
              "implicit_header", "preamble", "sync_word", "power_dbm", "snr_db", "rssi_dbm",
              "len")

    def __init__(self, **kw):
        for name in self.FIELDS:
            setattr(self, name, kw.get(name, 0))
        self.magic = MAGIC
        self.version = VERSION
        self.data = kw.get("data", b"")

    @classmethod
    def parse(cls, raw):
        if len(raw) < HEADER.size:
            return None
        f = cls(**dict(zip(cls.FIELDS, HEADER.unpack_from(raw))))
        f.data = raw[HEADER.size:HEADER.size + f.len]
        return f

    def pack(self):
        self.len = len(self.data)
        return HEADER.pack(*(getattr(self, n) for n in self.FIELDS)) + bytes(self.data)


class Session:
    def __init__(self, deveui, appkey):
        self.deveui = deveui
        self.appkey = appkey
        self.devaddr = None
        self.nwkskey = None
        self.appskey = None
        self.fcnt_up = None
        self.fcnt_down = 0
        self.queue = []  # (fport, payload, confirmed)

    def full_fcnt(self, fcnt16):
        if self.fcnt_up is None:
            return fcnt16
        base = self.fcnt_up & ~0xFFFF
        full = base | fcnt16
        if full < self.fcnt_up:
            full += 0x10000
        return full


class Server:
    def __init__(self, args):
        self.args = args
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        host, port = args.listen.rsplit(":", 1)
        self.sock.bind((host, int(port)))
        self.lock = threading.Lock()
        self.sessions = {}  # deveui (int) -> Session
        self.keys = {}
        for dev in args.device or []:
            eui, key = dev.split(":")
            self.keys[int(eui, 16)] = bytes.fromhex(key)
        self.default_key = bytes.fromhex(args.appkey) if args.appkey else None
        self.join_nonce = random.randrange(1 << 20)
        self.hub = sim_p2p.Hub(self, int(args.net_id, 16),
                               [int(x, 16) for x in args.p2p_eui_prefix])
        self.decoder = None
        self.decoder_lock = threading.Lock()
        bridge = os.path.join(os.path.dirname(os.path.abspath(__file__)), "decoder_bridge.js")
        if not args.no_decode:
            try:
                self.decoder = subprocess.Popen(["node", bridge], stdin=subprocess.PIPE,
                                                stdout=subprocess.PIPE, text=True)
            except OSError as e:
                self.log(f"decoder off: {e}")

    def log(self, msg):
        print(time.strftime("%H:%M:%S ") + msg, flush=True)

    def codec(self, req):
        if self.decoder is None:
            return None
        with self.decoder_lock:
            self.decoder.stdin.write(json.dumps(req) + "\n")
            self.decoder.stdin.flush()
            return json.loads(self.decoder.stdout.readline())

    # --- air -------------------------------------------------------------

    def reply(self, addr, f, data=None, delay_us=None, **params):
        r = Frame(seq=0, ref_seq=f.seq)
        if data is None:
            r.type = NONE
        else:
            r.type = PACKET
            r.delay_us = delay_us
            r.data = data
            r.modem, r.freq_hz, r.sf, r.bw_hz, r.cr = f.modem, f.freq_hz, f.sf, f.bw_hz, f.cr
            r.iq_inverted, r.crc_on, r.preamble, r.sync_word = 1, 0, 8, f.sync_word
            r.rssi_dbm, r.snr_db = self.args.rssi, self.args.snr
            for k, v in params.items():
                setattr(r, k, v)
        self.sock.sendto(r.pack(), addr)

    def run(self):
        threading.Thread(target=self.stdin_loop, daemon=True).start()
        self.log(f"listening on {self.args.listen}")
        while True:
            raw, addr = self.sock.recvfrom(512)
            f = Frame.parse(raw)
            if f is None or f.magic != MAGIC or f.version != VERSION:
                continue
            if f.type == HELLO:
                self.sock.sendto(Frame(type=HELLO).pack(), addr)
                self.log(f"{addr[1]}: hello")
                continue
            if f.type != PACKET:
                continue
            with self.lock:
                try:
                    self.handle_packet(addr, f)
                except Exception as e:  # keep serving the other nodes
                    self.log(f"{addr[1]}: error {e!r}")
                    self.reply(addr, f)

    def handle_packet(self, addr, f):
        mod = (f"LoRa SF{f.sf} {f.bw_hz // 1000} kHz" if f.modem == LORA
               else f"GFSK {f.bitrate} b/s")
        self.log(f"{addr[1]}: rx {f.freq_hz / 1e6:.3f} MHz {mod} {f.power_dbm} dBm "
                 f"{len(f.data)} B {f.data.hex()}")
        if self.hub.accepts(f):
            if not self.hub.handle(addr, f):
                self.reply(addr, f)
            return
        if f.modem != LORA or f.sync_word != LORAWAN_SYNC or f.iq_inverted or not f.data:
            self.reply(addr, f)  # not LoRaWAN: nothing to answer
            return
        mtype = f.data[0] >> 5
        if mtype == 0:
            self.join_request(addr, f)
        elif mtype in (2, 4):
            self.data_up(addr, f, confirmed=mtype == 4)
        else:
            self.reply(addr, f)

    # --- LoRaWAN -----------------------------------------------------------

    def join_request(self, addr, f):
        d = f.data
        if len(d) != 23:
            self.reply(addr, f)
            return
        joineui, deveui, devnonce = struct.unpack_from("<QQH", d, 1)
        key = self.keys.get(deveui, self.default_key)
        if key is None:
            self.log(f"  join from unknown DevEUI {deveui:016x}, no AppKey")
            self.reply(addr, f)
            return
        if aes_cmac(key, d[:19])[:4] != d[19:]:
            self.log(f"  join DevEUI {deveui:016x}: MIC mismatch (wrong AppKey?)")
            self.reply(addr, f)
            return

        s = self.sessions.get(deveui) or Session(deveui, key)
        self.sessions[deveui] = s
        self.join_nonce = (self.join_nonce + 1) & 0xFFFFFF
        devaddr = (0x26 << 24) | random.randrange(1 << 24)
        jn = self.join_nonce.to_bytes(3, "little")
        netid = NET_ID.to_bytes(3, "little")
        dn = struct.pack("<H", devnonce)
        s.devaddr = devaddr
        s.nwkskey = aes_encrypt(key, b"\x01" + jn + netid + dn + bytes(7))
        s.appskey = aes_encrypt(key, b"\x02" + jn + netid + dn + bytes(7))
        s.fcnt_up = None
        s.fcnt_down = 0

        mhdr = b"\x20"
        body = jn + netid + struct.pack("<I", devaddr) + bytes([0x00, RX1_DELAY_S])
        mic = aes_cmac(key, mhdr + body)[:4]
        accept = mhdr + aes_decrypt(key, body + mic)
        self.log(f"  join DevEUI {deveui:016x} JoinEUI {joineui:016x} DevNonce {devnonce} "
                 f"-> DevAddr {devaddr:08x}")
        self.reply(addr, f, accept, JOIN_ACCEPT_DELAY1_US)

    def data_up(self, addr, f, confirmed):
        d = f.data
        devaddr, fctrl, fcnt16 = struct.unpack_from("<IBH", d, 1)
        s = next((x for x in self.sessions.values() if x.devaddr == devaddr), None)
        if s is None:
            self.log(f"  uplink from unknown DevAddr {devaddr:08x}")
            self.reply(addr, f)
            return
        fcnt = s.full_fcnt(fcnt16)
        if data_mic(s.nwkskey, devaddr, fcnt, 0, d[:-4]) != d[-4:]:
            self.log(f"  DevAddr {devaddr:08x} FCnt {fcnt}: MIC mismatch")
            self.reply(addr, f)
            return
        s.fcnt_up = fcnt

        foptslen = fctrl & 0x0F
        fopts = d[8:8 + foptslen]
        rest = d[8 + foptslen:-4]
        fport = rest[0] if rest else None
        payload = frm_crypt(s.nwkskey if fport == 0 else s.appskey, devaddr, fcnt, 0,
                            rest[1:]) if rest else b""
        mac = payload if fport == 0 else fopts
        flags = ("ADR " if fctrl & 0x80 else "") + ("ADRACKReq " if fctrl & 0x40 else "") + \
                ("ACK " if fctrl & 0x20 else "") + ("confirmed" if confirmed else "")
        self.log(f"  {s.deveui:016x} FCnt {fcnt} FPort {fport} {flags} payload {payload.hex()}")

        answers = self.mac_commands(mac, f)
        if fport:
            decoded = self.codec({"op": "up", "fPort": fport, "bytes": payload.hex()})
            if decoded is not None:
                self.log("  decoded " + json.dumps(decoded.get("data", decoded)))
                for key in ("warnings", "errors"):
                    if decoded.get(key):
                        self.log(f"  {key}: {decoded[key]}")

        down = s.queue.pop(0) if s.queue else None
        if not (answers or down or confirmed or fctrl & 0x40):
            self.reply(addr, f)
            return

        fport_d, frm, conf_d = down if down else (None, b"", False)
        mhdr = bytes([(5 if conf_d else 3) << 5])
        fctrl_d = (0x20 if confirmed else 0) | (0x10 if s.queue else 0) | len(answers)
        fcnt_d = s.fcnt_down
        s.fcnt_down += 1
        msg = mhdr + struct.pack("<IBH", devaddr, fctrl_d, fcnt_d & 0xFFFF) + answers
        if fport_d is not None:
            msg += bytes([fport_d]) + frm_crypt(s.appskey, devaddr, fcnt_d, 1, frm)
        msg += data_mic(s.nwkskey, devaddr, fcnt_d, 1, msg)
        self.log(f"  downlink FCnt {fcnt_d} FPort {fport_d} mac {answers.hex()} "
                 f"payload {frm.hex()}")
        self.reply(addr, f, msg, RX1_DELAY_S * 1_000_000)

    def mac_commands(self, mac, f):
        answers = b""
        i = 0
        while i < len(mac):
            cid = mac[i]
            n = UP_MAC_LEN.get(cid)
            if n is None:
                self.log(f"  MAC: unknown CID 0x{cid:02x}, rest ignored")
                break
            arg = mac[i + 1:i + 1 + n]
            self.log(f"  MAC: {UP_MAC_NAME[cid]} {arg.hex()}")
            if cid == 0x02:
                margin = max(0, min(254, self.args.snr + 20))
                answers += bytes([0x02, margin, 1])
            elif cid == 0x0D:
                gps = time.time() - GPS_EPOCH_UNIX + GPS_LEAP_SECONDS
                answers += bytes([0x0D]) + struct.pack("<I", int(gps)) + \
                    bytes([int((gps % 1) * 256)])
            i += 1 + n
        return answers[:15]

    # --- console -----------------------------------------------------------

    def targets(self, who, p2p_only=False):
        nodes = [] if p2p_only else list(self.sessions.values())
        nodes += [p for p in self.hub.peers.values() if p.session_key]
        if who == "*":
            return nodes
        eui = int(who, 16)
        return [s for s in nodes if s.deveui == eui]

    def stdin_loop(self):
        for line in sys.stdin:
            parts = line.strip().split(None, 2)
            if not parts:
                continue
            try:
                with self.lock:
                    self.command(parts)
            except Exception as e:
                self.log(f"command failed: {e!r}")

    def command(self, parts):
        if parts[0] == "list":
            for s in self.sessions.values():
                self.log(f"{s.deveui:016x} DevAddr {s.devaddr or 0:08x} FCntUp {s.fcnt_up} "
                         f"FCntDown {s.fcnt_down} queued {len(s.queue)}")
            for p in self.hub.peers.values():
                self.log(f"{p.deveui:016x} P2P addr {p.addr:08x} paired {bool(p.session_key)} "
                         f"up {p.node_last} down {p.gw_counter} queued {len(p.queue)}")
        elif parts[0] == "down" and len(parts) == 3:
            args = parts[2].split()
            item = (int(args[0]), bytes.fromhex(args[1]), len(args) > 2 and args[2] == "c")
            for s in self.targets(parts[1]):
                s.queue.append(item)
                self.log(f"queued for {s.deveui:016x}: FPort {item[0]} {item[1].hex()}")
        elif parts[0] == "cmd" and len(parts) == 3:
            enc = self.codec({"op": "down", "input": {"data": json.loads(parts[2])}})
            if enc is None or enc.get("errors"):
                self.log(f"encode failed: {enc}")
                return
            item = (enc["fPort"], bytes(enc["bytes"]), False)
            for s in self.targets(parts[1]):
                s.queue.append(item)
                self.log(f"queued for {s.deveui:016x}: FPort {item[0]} {item[1].hex()}")
        elif parts[0] == "ctrl" and len(parts) == 3:
            item = ("ctrl", bytes.fromhex(parts[2]))
            for p in self.targets(parts[1], p2p_only=True):
                p.queue.append(item)
                self.log(f"queued for {p.deveui:016x}: 0x91 {item[1].hex()}")
        else:
            self.log("commands: down <deveui|*> <fport> <hex> [c] | cmd <deveui|*> <json> | "
                     "ctrl <deveui|*> <hex> | list")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--listen", default="127.0.0.1:47800", help="UDP address (host:port)")
    p.add_argument("--appkey", help="AppKey for any DevEUI (hex)")
    p.add_argument("--device", action="append", help="DEVEUI:APPKEY, repeatable")
    p.add_argument("--rssi", type=int, default=-60, help="downlink RSSI seen by the node, dBm")
    p.add_argument("--snr", type=int, default=9, help="downlink SNR seen by the node, dB")
    p.add_argument("--net-id", default="4e420001", help="P2P hub net_id (hex)")
    p.add_argument("--p2p-eui-prefix", action="append", default=[],
                   help="upper 32 bits of P2P DevEUIs to try with --appkey (hex), repeatable; "
                   "default 58760700")
    p.add_argument("--no-decode", action="store_true", help="do not run the TTN decoder")
    args = p.parse_args()
    args.p2p_eui_prefix = args.p2p_eui_prefix or ["58760700"]
    try:
        Server(args).run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
