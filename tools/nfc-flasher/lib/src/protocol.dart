// NFC firmware update protocol constants — mirror of doc/nfc-update-protocol.md.
// Keep in sync with the firmware bootloader's nfc_proto.h.

import 'dart:typed_data';

/// Image header magic: "SNFU".
const List<int> kSfuMagic = [0x53, 0x4E, 0x46, 0x55];

const int kSfuHeaderLen = 32;
const int kSfuSignatureLen = 64; // Ed25519
const int kSfuPreambleLen = kSfuHeaderLen + kSfuSignatureLen; // 96

const int kFlagSigned = 0x0001;
const int kFlagCrcPresent = 0x0002;

/// Max frame data bytes carried per DATA frame (ciphertext + tag when keyed).
const int kMaxData = 240;

/// AES-CCM tag length appended to each keyed frame's payload.
const int kCcmTagLen = 8;

/// Plaintext firmware bytes per DATA frame. The bootloader writes each frame at
/// `seq * kMaxPlaintext`, so the phone must chunk the payload by this size
/// regardless of keyed/unkeyed (unkeyed sends the bytes verbatim; keyed appends
/// an 8-byte tag, still <= kMaxData).
const int kMaxPlaintext = kMaxData - kCcmTagLen; // 232

// Command codes (phone -> MCU).
const int kCmdStart = 0x01;
const int kCmdData = 0x02;
const int kCmdFinish = 0x03;
const int kCmdAbort = 0x04;
const int kCmdPing = 0x05;

// Status codes (MCU -> phone).
const int kStReady = 0x10;
const int kStAck = 0x11;
const int kStRetry = 0x12;
const int kStOk = 0x13;
const int kStErrMagic = 0x20;
const int kStErrSize = 0x21;
const int kStErrFlash = 0x22;
const int kStErrVerify = 0x23;
const int kStErrState = 0x24;

String statusName(int s) {
  switch (s) {
    case kStReady:
      return 'READY';
    case kStAck:
      return 'ACK';
    case kStRetry:
      return 'RETRY';
    case kStOk:
      return 'OK';
    case kStErrMagic:
      return 'ERR_MAGIC';
    case kStErrSize:
      return 'ERR_SIZE';
    case kStErrFlash:
      return 'ERR_FLASH';
    case kStErrVerify:
      return 'ERR_VERIFY';
    case kStErrState:
      return 'ERR_STATE';
    default:
      return 'UNKNOWN(0x${s.toRadixString(16)})';
  }
}

bool isError(int status) => status >= 0x20;

/// CRC-32/IEEE (same polynomial as zlib / Zephyr crc32_ieee), little-endian result.
int crc32Ieee(Uint8List data, [int crc = 0]) {
  crc = crc ^ 0xFFFFFFFF;
  for (final b in data) {
    crc ^= b;
    for (int i = 0; i < 8; i++) {
      final mask = -(crc & 1) & 0xFFFFFFFF;
      crc = (crc >> 1) ^ (0xEDB88320 & mask);
      crc &= 0xFFFFFFFF;
    }
  }
  return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF;
}

/// A firmware image ready to stream: 96-byte preamble (header+signature) + payload.
class FirmwareImage {
  final Uint8List header; // 32 B
  final Uint8List signature; // 64 B
  final Uint8List payload; // raw slot0 bytes
  final bool signed;

  FirmwareImage({
    required this.header,
    required this.signature,
    required this.payload,
    required this.signed,
  });

  int get fwVersion =>
      header.buffer.asByteData().getUint32(8, Endian.little);

  String get versionString {
    final v = fwVersion;
    final major = (v >> 24) & 0xFF;
    final minor = (v >> 16) & 0xFF;
    final patch = (v >> 8) & 0xFF;
    final type = v & 0xFF;
    return '$major.$minor.$patch (type $type)';
  }

  int get payloadCrc32 =>
      header.buffer.asByteData().getUint32(20, Endian.little);

  int get totalDataFrames =>
      (payload.length + kMaxPlaintext - 1) ~/ kMaxPlaintext;

  /// Build a header for a raw/unsigned image (no signature, CRC only).
  static FirmwareImage unsigned(Uint8List payload, {int loadAddr = 0x08008000}) {
    final header = Uint8List(kSfuHeaderLen);
    final bd = header.buffer.asByteData();
    for (int i = 0; i < 4; i++) {
      header[i] = kSfuMagic[i];
    }
    bd.setUint16(4, 1, Endian.little); // hdr_version
    bd.setUint16(6, kFlagCrcPresent, Endian.little); // flags
    bd.setUint32(8, 0, Endian.little); // fw_version (unknown)
    bd.setUint32(12, payload.length, Endian.little);
    bd.setUint32(16, loadAddr, Endian.little);
    bd.setUint32(20, crc32Ieee(payload), Endian.little);
    return FirmwareImage(
      header: header,
      signature: Uint8List(kSfuSignatureLen),
      payload: payload,
      signed: false,
    );
  }

  /// Parse a full .sfu blob (preamble + payload).
  static FirmwareImage parseSfu(Uint8List blob) {
    if (blob.length < kSfuPreambleLen) {
      throw const FormatException('Image too short for .sfu preamble');
    }
    for (int i = 0; i < 4; i++) {
      if (blob[i] != kSfuMagic[i]) {
        throw const FormatException('Bad .sfu magic');
      }
    }
    final header = Uint8List.sublistView(blob, 0, kSfuHeaderLen);
    final signature =
        Uint8List.sublistView(blob, kSfuHeaderLen, kSfuPreambleLen);
    final declaredLen =
        header.buffer.asByteData().getUint32(12, Endian.little);
    final payload = Uint8List.sublistView(
        blob, kSfuPreambleLen, kSfuPreambleLen + declaredLen);
    final flags = header.buffer.asByteData().getUint16(6, Endian.little);
    return FirmwareImage(
      header: Uint8List.fromList(header),
      signature: Uint8List.fromList(signature),
      payload: Uint8List.fromList(payload),
      signed: (flags & kFlagSigned) != 0,
    );
  }
}
