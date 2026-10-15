#!/usr/bin/env python3
#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

"""STICKER simulator NFC phone: talks to the emulated ST25DV of a running
simulator (zephyr.exe, UDP 127.0.0.1:47801) the way the Manager-App does over
the FTM mailbox (doc/nfc.md):

  frame = chan(1) || payload
    0x03 plaintext  Command                 -> 0x03 || ver || Response
    0x01 owner      wire(secret_key)        -> 0x01 || wire
    0x02 vendor     wire(vendor_token)      -> 0x02 || wire
  wire  = serial(4,BE) || counter(4,BE) || AES-CCM(Command, tag 16)
  nonce = serial || counter || dir (0 request, 1 response), AAD = serial || counter

The owner/vendor channels first read get_basic_info (plaintext) for the serial
and the nonce high-water, then send counter = high-water + 1.

Usage:
  sim_nfc.py basic
  sim_nfc.py cmd '{"get_info": {}}'            # all pages
  sim_nfc.py --key 000102...0f cmd '{"reboot": {}}'
  sim_nfc.py vnd '{"get_basic_info": {}}'
  sim_nfc.py raw 03 f201 00                     # chan + hex frame
  sim_nfc.py field on|off | status | mboff
  sim_nfc.py                                    # interactive, same commands
"""

import argparse
import json
import shlex
import socket
import sys
import tempfile
from pathlib import Path

from Crypto.Cipher import AES

HERE = Path(__file__).resolve().parent
PROTO = HERE.parent.parent / "app" / "src" / "app_config.proto"

CHAN_CMD = 0x01
CHAN_VND = 0x02
CHAN_PLAIN = 0x03
DIR_REQUEST = 0x00
DIR_RESPONSE = 0x01
TAG_LEN = 16

ERRORS = {1: "mailbox not authorised (MB_MODE=0)", 2: "previous message still unread",
          3: "bad frame length"}


def seal(key: bytes, serial: int, counter: int, plaintext: bytes) -> bytes:
    hdr = serial.to_bytes(4, "big") + counter.to_bytes(4, "big")
    cipher = AES.new(key, AES.MODE_CCM, nonce=hdr + bytes([DIR_REQUEST]), mac_len=TAG_LEN)
    cipher.update(hdr)
    ct, tag = cipher.encrypt_and_digest(plaintext)
    return hdr + ct + tag


def open_wire(key: bytes, wire: bytes) -> tuple[int, int, bytes]:
    hdr = wire[:8]
    cipher = AES.new(key, AES.MODE_CCM, nonce=hdr + bytes([DIR_RESPONSE]), mac_len=TAG_LEN)
    cipher.update(hdr)
    plain = cipher.decrypt_and_verify(wire[8:-TAG_LEN], wire[-TAG_LEN:])
    return int.from_bytes(hdr[:4], "big"), int.from_bytes(hdr[4:8], "big"), plain


def load_proto():
    """Compile app_config.proto into a temporary module (grpcio-tools)."""
    from grpc_tools import protoc

    out = Path(tempfile.mkdtemp(prefix="sim_nfc_"))
    rc = protoc.main(["protoc", f"-I{PROTO.parent}", f"--python_out={out}", str(PROTO)])
    if rc != 0:
        sys.exit("protoc failed on " + str(PROTO))
    sys.path.insert(0, str(out))
    import app_config_pb2  # noqa: E402

    return app_config_pb2


class Phone:
    def __init__(self, sim: str, key: bytes, vendor_token: bytes, timeout: float):
        host, port = sim.rsplit(":", 1)
        self.addr = (host, int(port))
        self.key = key
        self.vendor_token = vendor_token
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.pb = load_proto()
        self.seq = 1
        self.counter = None
        self.serial = None

    # --- RF link -----------------------------------------------------------

    def _request(self, datagram: bytes) -> bytes:
        self.sock.sendto(datagram, self.addr)
        while True:
            try:
                data, _ = self.sock.recvfrom(1024)
            except socket.timeout:
                raise RuntimeError("no answer from the STICKER (simulator not running, "
                                   "frame rejected or session busy)") from None
            if data[:1] == b"E":
                raise RuntimeError("tag: " + ERRORS.get(data[1], f"error {data[1]}"))
            return data

    def status(self) -> dict:
        st = self._request(b"S")
        names = ["field", "vcc", "mb_ctrl", "mb_len", "gpo_static", "gpo_dyn", "mb_mode",
                 "i2c_sso"]
        return {n: (f"0x{v:02x}" if n.startswith(("mb_", "gpo")) else v)
                for n, v in zip(names, st[1:])}

    def field(self, on: bool) -> dict:
        self._request(b"F" + bytes([on]))
        return self.status()

    def mb_off(self) -> dict:
        self._request(b"D")
        return self.status()

    def xfer(self, chan: int, payload: bytes) -> bytes:
        reply = self._request(b"X" + bytes([chan]) + payload)
        if reply[:1] != b"R" or len(reply) < 2 or reply[1] != chan:
            raise RuntimeError(f"unexpected answer {reply.hex()}")
        return reply[2:]

    # --- protocol ----------------------------------------------------------

    def _command(self, body) -> bytes:
        cmd = self.pb.Command()
        if isinstance(body, (bytes, bytearray)):
            cmd.ParseFromString(body)
        else:
            from google.protobuf import json_format

            try:
                json_format.ParseDict(body, cmd)
            except json_format.ParseError as e:
                raise ValueError(str(e)) from None
        if not cmd.seq:
            cmd.seq = self.seq
        self.seq += 1
        return cmd.SerializeToString()

    def _response(self, plain: bytes):
        if not plain or plain[0] != 0x01:
            raise RuntimeError(f"unknown APP_PROTO_VERSION in {plain.hex()}")
        resp = self.pb.Response()
        resp.ParseFromString(plain[1:])
        return resp

    def plain(self, body):
        return self._response(self.xfer(CHAN_PLAIN, self._command(body)))

    def basic(self):
        resp = self.plain({"get_basic_info": {}})
        if resp.WhichOneof("body") == "basic_info":
            self.serial = resp.basic_info.serial_number
            self.counter = resp.basic_info.nonce_counter
        return resp

    def secure(self, body, vendor: bool = False):
        key = self.vendor_token if vendor else self.key
        chan = CHAN_VND if vendor else CHAN_CMD
        if self.counter is None:
            self.basic()
        self.counter += 1
        wire = seal(key, self.serial, self.counter, self._command(body))
        rsp_wire = self.xfer(chan, wire)
        serial, counter, plain = open_wire(key, rsp_wire)
        if (serial, counter) != (self.serial, self.counter):
            raise RuntimeError(f"reply header {serial}/{counter} does not match the request")
        return self._response(plain)

    def secure_all_pages(self, body, vendor: bool = False):
        """Send a command; for a host-paged answer (page_count > 1) fetch the rest."""
        resps = [self.secure(body, vendor)]
        name = next(iter(body))
        while resps[-1].page_count > 1 and resps[-1].page_index + 1 < resps[-1].page_count:
            nxt = {name: dict(body[name], page=resps[-1].page_index + 1)}
            resps.append(self.secure(nxt, vendor))
        return resps


def show(resp) -> None:
    from google.protobuf import json_format

    print(json.dumps(json_format.MessageToDict(resp, preserving_proto_field_name=True),
                     indent=2))


def run(phone: Phone, argv: list[str]) -> None:
    op = argv[0]
    if op == "status":
        print(phone.status())
    elif op == "field":
        print(phone.field(argv[1] == "on"))
    elif op == "mboff":
        print(phone.mb_off())
    elif op == "basic":
        show(phone.basic())
    elif op == "plain":
        show(phone.plain(json.loads(argv[1])))
    elif op in ("cmd", "vnd"):
        for resp in phone.secure_all_pages(json.loads(argv[1]), vendor=op == "vnd"):
            show(resp)
    elif op == "raw":
        reply = phone.xfer(int(argv[1], 16), bytes.fromhex("".join(argv[2:])))
        print(reply.hex())
    else:
        raise RuntimeError(f"unknown command {op}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sim", default="127.0.0.1:47801", help="simulator NFC link host:port")
    ap.add_argument("--key", default="000102030405060708090a0b0c0d0e0f",
                    help="secret_key (hex) for the owner channel")
    ap.add_argument("--vendor-token", default="101112131415161718191a1b1c1d1e1f",
                    help="vendor_token (hex) for the vendor channel")
    ap.add_argument("--timeout", type=float, default=5.0, help="answer timeout, s")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    phone = Phone(args.sim, bytes.fromhex(args.key), bytes.fromhex(args.vendor_token),
                  args.timeout)

    if args.command:
        try:
            run(phone, args.command)
        except (RuntimeError, ValueError) as e:
            sys.exit(f"error: {e}")
        return

    print("sim_nfc: basic | cmd <json> | vnd <json> | plain <json> | raw <chan> <hex> | "
          "field on|off | mboff | status | quit")
    while True:
        try:
            line = input("nfc> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not line:
            continue
        if line in ("quit", "exit"):
            return
        try:
            run(phone, shlex.split(line))
        except (RuntimeError, ValueError, IndexError) as e:
            print(f"error: {e}")


if __name__ == "__main__":
    main()
