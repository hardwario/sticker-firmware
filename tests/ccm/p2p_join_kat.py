#!/usr/bin/env python3
"""Generate the shared P2P join known-answer fixture (GitLab #73 / sticker-firmware #417).

An *independent* oracle: it uses pycryptodome's CMAC and CCM, not any of the three
implementations the fixture pins (Rust `control-radio`, Zephyr C `app_p2p.c`, the Python
replica in `proximos/firmware`). The same construction reproduces all five pre-existing
serial-keyed vectors byte-for-byte -- run with `--selftest` to check that -- so the only
thing this file changes about them is swapping `serial(4 BE)` for `dev_eui(8 MSB-first)`.

    python3 p2p_join_kat.py            # print the fixture JSON on stdout
    python3 p2p_join_kat.py --data     # print the data-frame fixture (p2p_data_kat.json)
    python3 p2p_join_kat.py --selftest # re-derive the five shipped vectors first
    diff <(python3 p2p_join_kat.py) p2p_join_kat.json   # must be empty

Requires pycryptodome (in this repo's bench venv: ~/hardwario-projects/sticker/.venv).
No key material beyond the fixed test constants below -- none of this is device material.
"""

import hashlib
import json
import struct
import sys

from Crypto.Cipher import AES
from Crypto.Hash import CMAC

LABEL_JOIN = b"HIO-P2P-JOIN"
LABEL_ACCEPT = b"HIO-P2P-ACC"
LABEL_SES = b"HIO-P2P-SES"

FRAME_JOIN_REQUEST = 0xF0
FRAME_JOIN_ACCEPT = 0xF1
FRAME_TELEMETRY = 0x02
FRAME_ACK = 0xFA

# Fixed test inputs. `dev_eui` is the one the control-radio runtime tests already register.
APP_KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
DEV_EUI = bytes.fromhex("70b3d57ed0000abe")
DEV_NONCE = 7
CENTRAL_NONCE = 0x22222222
NET_ID = 100
DEV_ADDR = 5
RX1_DELAY_S = 1
PRODUCT_TYPE = 1
PROTO_VERSION = 1
FW_VERSION = (1, 4, 0, 0)


def cmac(key: bytes, msg: bytes) -> bytes:
    return CMAC.new(key, ciphermod=AES).update(msg).digest()


# FCtrl bits (decision #22, p2p_link_check.md section 3.2).
FCTRL_CONFIRMED = 0x01  # uplink: the Node wants an ACK in RX1
FCTRL_FPENDING = 0x10  # downlink: more is queued for this Node
FCTRL_ACK = 0x20  # downlink: acknowledges the confirmed uplink it answers


def header(net_id: int, dev_addr: int, frame_type: int, counter: int, fctrl: int = 0) -> bytes:
    """The 12-byte cleartext header (decision #22):
    net_id(4 BE) dev_addr(2 BE) frame_type(1) FCtrl(1) counter(4 BE)."""
    return struct.pack(">IHBBI", net_id, dev_addr, frame_type, fctrl, counter)


def header_v11(net_id: int, dev_addr: int, frame_type: int, counter: int) -> bytes:
    """The 11-byte header before decision #22 (no FCtrl) -- selftest only."""
    return struct.pack(">IHBI", net_id, dev_addr, frame_type, counter)


def join_request_body(dev_eui: bytes) -> bytes:
    """product_type(1) proto_version(1) dev_eui(8 MSB-first) fw_version(4) -- 14 B."""
    assert len(dev_eui) == 8, "dev_eui is 8 bytes, MSB-first (D1)"
    return bytes([PRODUCT_TYPE, PROTO_VERSION]) + dev_eui + bytes(FW_VERSION)


def join_accept_body() -> bytes:
    """net_id(4 BE) dev_addr(2 BE) central_nonce(4 BE) rx1_delay_s(1) reserved(4) -- 15 B."""
    return (
        struct.pack(">IHI", NET_ID, DEV_ADDR, CENTRAL_NONCE)
        + bytes([RX1_DELAY_S])
        + bytes(4)
    )


def session_key_input(dev_nonce: int, central_nonce: int, dev_eui: bytes) -> bytes:
    """The exact 28-octet string, before the manual zero pad."""
    assert len(dev_eui) == 8
    return LABEL_SES + b"\x01" + struct.pack(">II", dev_nonce, central_nonce) + dev_eui


def session_key(app_key: bytes, dev_nonce: int, central_nonce: int, dev_eui: bytes) -> bytes:
    """Both shipped implementations zero-pad the input to 32 B before CMAC; so do we.

    RFC section 4.2's prose says "CMAC's own final-block subkey padding applies -- there is no
    manual pad". That is not what the code does, and the two constructions give different keys
    (see `unpadded_detector` in the JSON). The fixture pins the padded-to-32 form because that
    is the hardware-proven behaviour on both ends; the wording is raised on #73, not changed.
    """
    return cmac(app_key, session_key_input(dev_nonce, central_nonce, dev_eui).ljust(32, b"\0"))


def data_frame(key: bytes, frame_type: int, counter: int, direction: int, plaintext: bytes,
               fctrl: int = 0, net_id: int = NET_ID, dev_addr: int = DEV_ADDR):
    """An AES-CCM data frame: header is AAD, 13-byte nonce, 4-byte tag (p2p.md section 6.1).
    The nonce does not carry FCtrl: the AAD authenticates it."""
    aad = header(net_id, dev_addr, frame_type, counter, fctrl)
    nonce = struct.pack(">IHBB", counter, dev_addr, frame_type, direction) + bytes(5)
    c = AES.new(key, AES.MODE_CCM, nonce=nonce, mac_len=4)
    c.update(aad)
    ciphertext, tag = c.encrypt_and_digest(plaintext)
    return aad + ciphertext + tag, nonce


def selftest() -> None:
    """Re-derive the five vectors both code bases shipped *before* #73, from this same code.

    If these five match, the construction here is exactly what control-radio and app_p2p.c
    implement today, and the DevEUI vectors below differ from theirs in one field only.
    """
    serial = 0x12345678
    cases = [
        (
            "control-radio session_key (dn 1, cn 2, serial)",
            cmac(APP_KEY, (LABEL_SES + b"\x01" + struct.pack(">III", 1, 2, serial)).ljust(32, b"\0")),
            "b41fdfecf6834157a734bd30fa0e80e5",
        ),
        (
            "control-radio join_tag (dn 1, serial, fw 1.4.0.0)",
            cmac(APP_KEY, LABEL_JOIN + header_v11(0, 0, FRAME_JOIN_REQUEST, 1)
                 + bytes([1, 1]) + struct.pack(">I", serial) + bytes((1, 4, 0, 0))),
            "abdced02ada85e2afa3624f705ad1c49",
        ),
        (
            "firmware session_key (dn 0x11111111, cn 0x22222222, serial)",
            cmac(APP_KEY, (LABEL_SES + b"\x01"
                           + struct.pack(">III", 0x11111111, 0x22222222, serial)).ljust(32, b"\0")),
            "80887b1e99b61b0b19f42e457feb406f",
        ),
        (
            "firmware join_tag (dn 7, serial, fw 1.2.3.0)",
            cmac(APP_KEY, LABEL_JOIN + header_v11(0, 0, FRAME_JOIN_REQUEST, 7)
                 + bytes([1, 1]) + struct.pack(">I", serial) + bytes((1, 2, 3, 0))),
            "fd02843b756c81a997882c28edaf28f8",
        ),
        (
            "join_accept_tag (unchanged by #73)",
            cmac(APP_KEY, LABEL_ACCEPT + header_v11(0, 0, FRAME_JOIN_ACCEPT, DEV_NONCE)
                 + join_accept_body()),
            "ca91d30e195f4f38bf2af38fc4ead0e0",
        ),
    ]
    for name, got, want in cases:
        status = "OK " if got.hex() == want else "BAD"
        print(f"{status} {name}: {got.hex()}", file=sys.stderr)
        if got.hex() != want:
            raise SystemExit(f"selftest failed: {name} expected {want}")
    print("selftest: all 5 pre-#73 vectors reproduce (11 B header)", file=sys.stderr)


def fixture() -> dict:
    jr_header = header(0, 0, FRAME_JOIN_REQUEST, DEV_NONCE)
    jr_body = join_request_body(DEV_EUI)
    jr_tag = cmac(APP_KEY, LABEL_JOIN + jr_header + jr_body)

    ja_header = header(0, 0, FRAME_JOIN_ACCEPT, DEV_NONCE)
    ja_body = join_accept_body()
    ja_tag = cmac(APP_KEY, LABEL_ACCEPT + ja_header + ja_body)

    sk_input = session_key_input(DEV_NONCE, CENTRAL_NONCE, DEV_EUI)
    sk = session_key(APP_KEY, DEV_NONCE, CENTRAL_NONCE, DEV_EUI)

    up_plain = bytes.fromhex("01082a")
    up_frame, up_nonce = data_frame(sk, FRAME_TELEMETRY, 0, 0, up_plain, FCTRL_CONFIRMED)
    dn_plain = bytes.fromhex("00c407")
    dn_frame, dn_nonce = data_frame(sk, FRAME_ACK, 0, 1, dn_plain, FCTRL_ACK)

    return {
        "_comment": (
            "P2P join KAT -- DevEUI identity (GitLab #73 / sticker-firmware #417), "
            "12 B header with FCtrl (decision #22; join frames FCtrl 0). "
            "All multi-byte fields big-endian; dev_eui 8 B MSB-first, NOT LoRaWAN's LSB "
            "on-air order. session_key input is zero-padded to 32 B before CMAC, as all "
            "three implementations do. Generated by p2p_join_kat.py with pycryptodome: "
            "CMAC.new(key, ciphermod=AES); AES.MODE_CCM mac_len=4, 13-byte nonce. "
            "Byte-identical copies live in proximos-v2, sticker-firmware and "
            "proximos/firmware -- if they ever differ, one of the three is wrong."
        ),
        "app_key": APP_KEY.hex(),
        "dev_eui": DEV_EUI.hex(),
        "dev_nonce": DEV_NONCE,
        "central_nonce": CENTRAL_NONCE,
        "net_id": NET_ID,
        "dev_addr": DEV_ADDR,
        "rx1_delay_s": RX1_DELAY_S,
        "product_type": PRODUCT_TYPE,
        "proto_version": PROTO_VERSION,
        "fw_version": list(FW_VERSION),
        "join_request": {
            "header": jr_header.hex(),
            "body": jr_body.hex(),
            "body_len": len(jr_body),
            "cmac_input": (LABEL_JOIN + jr_header + jr_body).hex(),
            "tag": jr_tag.hex(),
            "frame": (jr_header + jr_body + jr_tag).hex(),
            "frame_len": len(jr_header + jr_body + jr_tag),
        },
        "join_accept": {
            "header": ja_header.hex(),
            "body": ja_body.hex(),
            "cmac_input": (LABEL_ACCEPT + ja_header + ja_body).hex(),
            "tag": ja_tag.hex(),
            "frame": (ja_header + ja_body + ja_tag).hex(),
            "frame_len": len(ja_header + ja_body + ja_tag),
        },
        "session_key": {
            "cmac_input_28": sk_input.hex(),
            "cmac_input_32": sk_input.ljust(32, b"\0").hex(),
            "key": sk.hex(),
            "unpadded_detector": cmac(APP_KEY, sk_input).hex(),
        },
        "first_telemetry_uplink": {
            "frame_type": FRAME_TELEMETRY,
            "fctrl": FCTRL_CONFIRMED,
            "counter": 0,
            "direction": 0,
            "plaintext": up_plain.hex(),
            "nonce": up_nonce.hex(),
            "frame": up_frame.hex(),
        },
        "first_ack_downlink": {
            "frame_type": FRAME_ACK,
            "fctrl": FCTRL_ACK,
            "counter": 0,
            "direction": 1,
            "plaintext": dn_plain.hex(),
            "nonce": dn_nonce.hex(),
            "frame": dn_frame.hex(),
        },
    }


# Data-frame KAT (decision #22, p2p_link_check.md section 3.2): the header with
# FCtrl under a fixed key, independent of the join / KDF chain above.
DATA_KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
DATA_NET_ID = 0x6D249662
DATA_DEV_ADDR = 2
DATA_COUNTER = 16
FRAME_COMMAND = 0x56


def data_fixture() -> dict:
    def vec(name, frame_type, fctrl, direction, plain):
        frame, nonce = data_frame(DATA_KEY, frame_type, DATA_COUNTER, direction, plain, fctrl,
                                  DATA_NET_ID, DATA_DEV_ADDR)
        return name, {
            "frame_type": frame_type,
            "fctrl": fctrl,
            "counter": DATA_COUNTER,
            "direction": direction,
            "plaintext": plain.hex(),
            "header": frame[:12].hex(),
            "nonce": nonce.hex(),
            "frame": frame.hex(),
        }

    vectors = dict([
        vec("telemetry_confirmed", FRAME_TELEMETRY, FCTRL_CONFIRMED, 0, bytes.fromhex("01020304")),
        vec("telemetry_unconfirmed", FRAME_TELEMETRY, 0, 0, bytes.fromhex("01020304")),
        vec("ack", FRAME_ACK, FCTRL_ACK, 1, bytes.fromhex("00c407")),
        vec("command_ack_fpending", FRAME_COMMAND, FCTRL_ACK | FCTRL_FPENDING, 1,
            bytes.fromhex("0801")),
    ])
    return {
        "_comment": (
            "P2P data-frame KAT (decision #22): header net_id(4 BE) dev_addr(2 BE) "
            "frame_type(1) FCtrl(1) counter(4 BE) = 12 B, all of it the CCM AAD; nonce "
            "counter(4 BE) dev_addr(2 BE) frame_type(1) direction(1) zeros(5) -- FCtrl is "
            "not in the nonce. FCtrl bits: 0 CONFIRMED (uplink), 4 FPending, 5 ACK "
            "(downlink). AES-128-CCM, 4 B tag. Generated by p2p_join_kat.py --data with "
            "pycryptodome; byte-identical copies in proximos-v2 and sticker-firmware."
        ),
        "key": DATA_KEY.hex(),
        "net_id": DATA_NET_ID,
        "dev_addr": DATA_DEV_ADDR,
        **vectors,
    }


def main() -> None:
    if "--selftest" in sys.argv[1:]:
        selftest()
    fx = data_fixture() if "--data" in sys.argv[1:] else fixture()
    text = json.dumps(fx, indent=2) + "\n"
    sys.stdout.write(text)
    if "--sha256" in sys.argv[1:]:
        print(f"sha256  {hashlib.sha256(text.encode()).hexdigest()}", file=sys.stderr)


if __name__ == "__main__":
    main()
