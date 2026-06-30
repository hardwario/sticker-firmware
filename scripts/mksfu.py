#!/usr/bin/env python3
"""Pack a raw firmware .bin into a .sfu update image (32-byte header + payload).

The .sfu is the distributable form of a variant-B firmware image consumed by the
NFC bootloader and the Manager-App flasher (doc/nfc-update-protocol.md §2). There
is no image signature: authenticity comes from the per-device AES-CCM channel at
transfer time, so the header only carries the payload CRC-32 for integrity. The
header layout mirrors `struct sfu_header` in include/sticker/nfc_proto.h.
"""
import argparse
import struct
import zlib

SFU_MAGIC = b"SNFU"
SFU_HDR_VERSION = 1
SFU_FLAG_CRC_PRESENT = 0x0002
SFU_HEADER_LEN = 32


def build_header(payload: bytes, fw_version: int, load_addr: int) -> bytes:
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    # <4s H H I I I I 8s  -> magic, hdr_version, flags, fw_version,
    #                        payload_len, load_addr, payload_crc32, reserved
    hdr = struct.pack(
        "<4sHHIIII8s",
        SFU_MAGIC,
        SFU_HDR_VERSION,
        SFU_FLAG_CRC_PRESENT,
        fw_version & 0xFFFFFFFF,
        len(payload),
        load_addr & 0xFFFFFFFF,
        crc,
        b"\x00" * 8,
    )
    assert len(hdr) == SFU_HEADER_LEN, len(hdr)
    return hdr


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", help="raw firmware image (.bin)")
    ap.add_argument("output", help="output .sfu path")
    ap.add_argument(
        "--load-addr",
        type=lambda x: int(x, 0),
        required=True,
        help="absolute flash base of the slot, e.g. 0x08009000",
    )
    ap.add_argument(
        "--fw-version",
        type=lambda x: int(x, 0),
        default=0,
        help="(major<<24)|(minor<<16)|(patch<<8)|build_type",
    )
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        payload = f.read()
    if not payload:
        raise SystemExit(f"empty payload: {args.input}")

    hdr = build_header(payload, args.fw_version, args.load_addr)
    with open(args.output, "wb") as f:
        f.write(hdr)
        f.write(payload)

    crc = zlib.crc32(payload) & 0xFFFFFFFF
    print(
        f"wrote {args.output}: {len(payload)} B payload "
        f"@ {hex(args.load_addr)} crc=0x{crc:08x} "
        f"(total {len(hdr) + len(payload)} B)"
    )


if __name__ == "__main__":
    main()
