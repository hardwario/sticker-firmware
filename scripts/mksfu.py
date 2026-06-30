#!/usr/bin/env python3
"""Pack a raw firmware .bin into a .sfu update image and (optionally) sign it.

Layout (doc/nfc-update-protocol.md §2), little-endian, mirrors `struct sfu_header`
in include/sticker/nfc_proto.h:

    unsigned:  header(32) + payload
    signed:    header(32) + signature(64) + payload     (SFU_FLAG_SIGNED set)

The header carries the payload CRC-32 for integrity. With `--key` the 32-byte
header is signed with Ed25519 (the header binds the payload via its CRC), so the
bootloader authenticates the whole image against its baked-in public key — on top
of the per-device AES-CCM channel used during the transfer.
"""
import argparse
import struct
import zlib

SFU_MAGIC = b"SNFU"
SFU_HDR_VERSION = 1
SFU_FLAG_SIGNED = 0x0001
SFU_FLAG_CRC_PRESENT = 0x0002
SFU_HEADER_LEN = 32
SFU_SIG_LEN = 64


def build_header(payload: bytes, fw_version: int, load_addr: int, signed: bool) -> bytes:
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    flags = SFU_FLAG_CRC_PRESENT | (SFU_FLAG_SIGNED if signed else 0)
    # <4s H H I I I I 8s  -> magic, hdr_version, flags, fw_version,
    #                        payload_len, load_addr, payload_crc32, reserved
    hdr = struct.pack(
        "<4sHHIIII8s",
        SFU_MAGIC,
        SFU_HDR_VERSION,
        flags,
        fw_version & 0xFFFFFFFF,
        len(payload),
        load_addr & 0xFFFFFFFF,
        crc,
        b"\x00" * 8,
    )
    assert len(hdr) == SFU_HEADER_LEN, len(hdr)
    return hdr


def sign_header(header: bytes, key_path: str) -> bytes:
    """Ed25519-sign the 32-byte header; returns the 64-byte signature."""
    from cryptography.hazmat.primitives.serialization import load_pem_private_key
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    with open(key_path, "rb") as f:
        key = load_pem_private_key(f.read(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise SystemExit(f"{key_path} is not an Ed25519 private key")
    sig = key.sign(header)
    assert len(sig) == SFU_SIG_LEN, len(sig)
    return sig


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
    ap.add_argument(
        "--key",
        help="Ed25519 private key (PEM) to sign the image; omit for an unsigned .sfu",
    )
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        payload = f.read()
    if not payload:
        raise SystemExit(f"empty payload: {args.input}")

    signed = args.key is not None
    hdr = build_header(payload, args.fw_version, args.load_addr, signed)
    sig = sign_header(hdr, args.key) if signed else b""

    with open(args.output, "wb") as f:
        f.write(hdr)
        f.write(sig)
        f.write(payload)

    crc = zlib.crc32(payload) & 0xFFFFFFFF
    print(
        f"wrote {args.output}: {len(payload)} B payload "
        f"@ {hex(args.load_addr)} crc=0x{crc:08x} "
        f"{'SIGNED (Ed25519)' if signed else 'unsigned'} "
        f"(total {len(hdr) + len(sig) + len(payload)} B)"
    )


if __name__ == "__main__":
    main()
