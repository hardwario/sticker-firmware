#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#
"""STICKER simulator P2P hub: the ProXimos Hub side of the TOWER protocol over
LoRa (doc/p2p.md), gateway and central in one, for sim_air.py.

- Join (doc/p2p.md 5.3): JoinRequest under join_key, JoinAccept 1 s later,
  session_key derived from both nonces.
- Data plane: AES-CCM TOWER frames, replay lane per node, a local ACK 20 ms
  after a confirmed uplink with PENDING when a downlink waits, a CTRL tail with
  LinkCheckAns / TimeAns for the requests in the frame, and PendingInfo when
  more than one downlink waits.
- 0x81 payloads are decoded with the TTN decoder (same fPort numbers), 0x91
  TLVs are printed by name.

The fsk profile (stock TOWER gateway, OTA pairing) is not simulated.
"""

import random
import struct
import time

from cryptography.hazmat.primitives import cmac
from cryptography.hazmat.primitives.ciphers import algorithms
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

P2P_SYNC = 0x1424  # LoRa private network sync word (public_network = false)
TOWER_VERSION = 1
T_DATA, T_ACK = 0, 1
FLAG_CONFIRMED = 0x01
ACK_PENDING, ACK_CTRL = 0x01, 0x02
HEADER = struct.Struct("<BBIII")
TAG_LEN = 8

ENV_DATA, ENV_CTRL = 0x81, 0x91
PORT_COMMAND = 86

TURNAROUND_US = 20_000
DOWNLINK_GAP_US = 30_000
JOIN_ACCEPT_DELAY_US = 1_000_000

CTRL_NAMES = {0x01: "Capabilities", 0x02: "Hello", 0x03: "Detach", 0x04: "RejoinReq",
              0x06: "HomeGateway", 0x07: "JoinReq", 0x08: "JoinAccept", 0x10: "LinkCheck",
              0x15: "Backoff", 0x20: "Time", 0x22: "PendingInfo", 0x30: "DevStatus"}


def cmac32(key, msg):
    c = cmac.CMAC(algorithms.AES(key))
    c.update(msg + bytes(32 - len(msg)))
    return c.finalize()


def join_key(app_key, deveui):
    return cmac32(app_key, b"HIO-TWR-JOIN\x01" + deveui.to_bytes(8, "big"))


def session_key(app_key, deveui, dev_nonce, central_nonce):
    return cmac32(app_key, b"HIO-TWR-SES\x01" + struct.pack(">II", dev_nonce, central_nonce) +
                  deveui.to_bytes(8, "big"))


def seal(key, ftype, flags, src, dest, counter, pt):
    hdr = HEADER.pack((TOWER_VERSION << 5) | ftype, flags, src, dest, counter)
    nonce = struct.pack("<II", src, counter) + bytes(5)
    return hdr + AESCCM(key, tag_length=TAG_LEN).encrypt(nonce, pt, hdr)


def unseal(key, raw):
    """Plaintext of a TOWER frame, or None when it does not open under key."""
    hdr = raw[:HEADER.size]
    _, _, src, _, counter = HEADER.unpack(hdr)
    nonce = struct.pack("<II", src, counter) + bytes(5)
    try:
        return AESCCM(key, tag_length=TAG_LEN).decrypt(nonce, raw[HEADER.size:], hdr)
    except Exception:
        return None


def lora_toa_us(sf, bw_hz, length, cr=1, preamble=8):
    """LoRa time on air: explicit header, CRC on (SX126x datasheet 6.1.4)."""
    t_sym = (1 << sf) * 1e6 / bw_hz
    ldro = 1 if t_sym > 16_000 else 0
    payload = 8 * length - 4 * sf + 28 + 16
    n = 8 + max(-(-payload // (4 * (sf - 2 * ldro))) * (cr + 4), 0)
    return int((preamble + 4.25 + n) * t_sym)


def tlvs(data):
    out, i = [], 0
    while i + 2 <= len(data):
        cmd, n = data[i], data[i + 1]
        if i + 2 + n > len(data):
            break
        out.append((cmd, data[i + 2:i + 2 + n]))
        i += 2 + n
    return out


class Peer:
    kind = "p2p"

    def __init__(self, deveui, app_key):
        self.deveui = deveui
        self.addr = deveui & 0xFFFFFFFF
        self.app_key = app_key
        self.session_key = None
        self.node_last = 0
        self.gw_counter = 0
        self.queue = []  # (fport, payload, confirmed), as for LoRaWAN; or ("ctrl", tlvs)

    def next_counter(self):
        self.gw_counter += 1
        return self.gw_counter


class Hub:
    def __init__(self, server, net_id, eui_prefixes):
        self.server = server
        self.net_id = net_id
        self.eui_prefixes = eui_prefixes
        self.peers = {}  # addr -> Peer

    def log(self, msg):
        self.server.log("p2p " + msg)

    def accepts(self, f):
        return f.modem == 1 and f.sync_word == P2P_SYNC and not f.iq_inverted and \
            len(f.data) >= HEADER.size + TAG_LEN

    def send(self, addr, f, frame, delay_us):
        self.server.reply(addr, f, frame, delay_us, iq_inverted=0, crc_on=1)

    # --- uplinks -----------------------------------------------------------

    def handle(self, addr, f):
        ver_type, flags, src, dest, counter = HEADER.unpack_from(f.data)
        if ver_type >> 5 != TOWER_VERSION:
            self.log(f"{src:08x}: TOWER version {ver_type >> 5}, dropped")
            return False
        ftype = ver_type & 0x1F
        if ftype == T_DATA and dest == 0:
            return self.join_request(addr, f, src, counter)
        peer = self.peers.get(src)
        if peer is None or peer.session_key is None or dest != self.net_id:
            self.log(f"{src:08x}: no session for this node (dest {dest:08x}), dropped")
            return False
        pt = unseal(peer.session_key, f.data)
        if pt is None:
            self.log(f"{src:08x}: frame does not open under the session key")
            return False
        if ftype == T_ACK:
            acked, rssi, aflags = struct.unpack_from("<IbB", pt) if len(pt) >= 6 else (0, 0, 0)
            self.log(f"{peer.deveui:016x}: ACK of downlink {acked}, node RSSI {rssi} dBm")
            return False
        return self.data_up(addr, f, peer, flags, counter, pt)

    def candidates(self, addr):
        found = [(eui, key) for eui, key in self.server.keys.items() if eui & 0xFFFFFFFF == addr]
        if self.server.default_key:
            found += [((p << 32) | addr, self.server.default_key) for p in self.eui_prefixes]
        return found

    def join_request(self, addr, f, src, dev_nonce):
        for deveui, app_key in self.candidates(src):
            jkey = join_key(app_key, deveui)
            pt = unseal(jkey, f.data)
            if pt is None:
                continue
            if len(pt) < 3 + 14 or pt[0] != ENV_CTRL or pt[1] != 0x07 or \
                    int.from_bytes(pt[5:13], "big") != deveui:
                self.log(f"{deveui:016x}: malformed JoinRequest {pt.hex()}")
                return False
            fw = pt[13:17]
            peer = self.peers.get(src)
            if peer is None or peer.deveui != deveui:
                peer = Peer(deveui, app_key)
                self.peers[src] = peer
            central_nonce = random.randrange(1, 1 << 32)
            body = struct.pack("<II", self.net_id, central_nonce) + bytes([1, 0]) + bytes(3)
            accept = seal(jkey, T_DATA, 0, self.net_id, src, dev_nonce,
                          bytes([ENV_CTRL, 0x08, len(body)]) + body)
            peer.session_key = session_key(app_key, deveui, dev_nonce, central_nonce)
            peer.node_last = 0
            peer.gw_counter = 0
            self.send(addr, f, accept, JOIN_ACCEPT_DELAY_US)
            self.log(f"{deveui:016x}: JoinRequest dev_nonce {dev_nonce} fw "
                     f"{fw[0]}.{fw[1]}.{fw[2]} -> JoinAccept net_id {self.net_id:08x}")
            return True
        self.log(f"{src:08x}: JoinRequest from an unknown device (--device / --appkey "
                 f"+ --p2p-eui-prefix)")
        return False

    def data_up(self, addr, f, peer, flags, counter, pt):
        confirmed = bool(flags & FLAG_CONFIRMED)
        if counter == peer.node_last:
            self.log(f"{peer.deveui:016x}: retransmission of {counter}, re-ACK only")
            fresh = False
        elif counter < peer.node_last:
            self.log(f"{peer.deveui:016x}: replayed counter {counter} <= {peer.node_last}, "
                     f"dropped")
            return False
        else:
            peer.node_last = counter
            fresh = True

        requests = []
        if fresh:
            requests = self.deliver(peer, counter, confirmed, pt)
        if not confirmed:
            return False

        tail = b""
        if 0x10 in requests:
            margin = int(self.server.args.snr - (-7.5 - 2.5 * (f.sf - 7)))
            tail += bytes([0x10, 4]) + struct.pack("<bbbB", self.server.args.rssi,
                                                    self.server.args.snr, margin, 1)
        if 0x20 in requests:
            now = time.time()
            tail += bytes([0x20, 9]) + struct.pack("<IBI", int(now), int((now % 1) * 256),
                                                    counter)
        if len(peer.queue) > 1:
            tail += bytes([0x22, 1, 1])
        aflags = (ACK_PENDING if peer.queue else 0) | (ACK_CTRL if tail else 0)
        ack_pt = struct.pack("<IbB", counter, self.server.args.rssi, aflags) + tail
        ack = seal(peer.session_key, T_ACK, 0, self.net_id, peer.addr, peer.next_counter(),
                   ack_pt)
        self.send(addr, f, ack, TURNAROUND_US)

        if peer.queue:
            dl_pt = self.downlink_payload(peer.queue.pop(0))
            dl = seal(peer.session_key, T_DATA, FLAG_CONFIRMED, self.net_id, peer.addr,
                      peer.next_counter(), dl_pt)
            delay = TURNAROUND_US + lora_toa_us(f.sf, f.bw_hz, len(ack)) + DOWNLINK_GAP_US
            self.send(addr, f, dl, delay)
            self.log(f"{peer.deveui:016x}: downlink {dl_pt.hex()} "
                     f"({len(peer.queue)} more queued)")
        return True

    def deliver(self, peer, counter, confirmed, pt):
        """Log one fresh uplink; returns the control request IDs it carries."""
        tag = f"{peer.deveui:016x}: up {counter}{' C' if confirmed else ''}"
        if pt[:1] == bytes([ENV_DATA]) and len(pt) >= 2:
            port, body = pt[1], pt[2:]
            self.log(f"{tag} port {port} {body.hex()}")
            dec = self.server.codec({"op": "up", "fPort": port, "bytes": body.hex()})
            if dec is not None:
                self.log(f"  decoded: {dec}")
            return []
        if pt[:1] == bytes([ENV_CTRL]):
            items = tlvs(pt[1:])
            names = [f"{CTRL_NAMES.get(c, hex(c))}({v.hex()})" for c, v in items]
            self.log(f"{tag} ctrl {' '.join(names) or '(empty: fetch)'}")
            return [c for c, _ in items]
        self.log(f"{tag} unknown envelope {pt.hex()}")
        return []

    def downlink_payload(self, item):
        if item[0] == "ctrl":
            return bytes([ENV_CTRL]) + item[1]
        fport, payload, _ = item
        port = PORT_COMMAND if fport == 85 else fport
        return bytes([ENV_DATA, port]) + payload
