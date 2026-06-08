// ISO15693 (NfcV) firmware streaming to an ST25DV tag, EEPROM software-mailbox
// binding (baseline) per doc/nfc-update-protocol.md §4.
//
// NOTE: NFC plugin APIs change between major versions. This targets
// nfc_manager ^3.5.0 (Android NfcV.transceive). If you bump the plugin,
// adjust the session/transceive calls in `run()`.

import 'dart:async';
import 'dart:typed_data';

import 'package:nfc_manager/nfc_manager.dart';
import 'package:nfc_manager/platform_tags.dart';

import 'protocol.dart';

class FlashProgress {
  final String phase;
  final int sent;
  final int total;
  FlashProgress(this.phase, this.sent, this.total);
  double get fraction => total == 0 ? 0 : sent / total;
}

class FlashException implements Exception {
  final String message;
  FlashException(this.message);
  @override
  String toString() => message;
}

// EEPROM mailbox block layout (4-byte ISO15693 blocks).
const int _blkPhFlag = 0; // 0x000
const int _blkReq = 1; // 0x004 .. (260 B -> 65 blocks)
const int _blkMcFlag = 66; // 0x108
const int _blkRsp = 67; // 0x10C .. (16 B -> 4 blocks)
const int _rspBlocks = 4;

const int _iso15693Flags = 0x02; // high data rate, single sub-carrier
const Duration _pollTimeout = Duration(seconds: 3);

class NfcFlasher {
  final void Function(FlashProgress)? onProgress;
  final bool allowUnsigned;

  NfcFlasher({this.onProgress, this.allowUnsigned = false});

  /// Start an NFC session and stream [image]. Completes when the device
  /// reports OK, or throws FlashException. Call from a button handler.
  Future<void> run(FirmwareImage image) async {
    if (image.signed == false && !allowUnsigned) {
      throw FlashException(
          'Image is unsigned; production firmware refuses unsigned images.');
    }

    final done = Completer<void>();

    await NfcManager.instance.startSession(
      onDiscovered: (NfcTag tag) async {
        final nfcV = NfcV.from(tag);
        if (nfcV == null) {
          await _stop(error: 'Tag is not ISO15693 (NfcV).');
          if (!done.isCompleted) {
            done.completeError(FlashException('Not an ISO15693 tag'));
          }
          return;
        }
        try {
          await _flash(nfcV, image);
          await _stop();
          if (!done.isCompleted) done.complete();
        } catch (e) {
          await _stop(error: e.toString());
          if (!done.isCompleted) done.completeError(FlashException('$e'));
        }
      },
    );

    return done.future;
  }

  Future<void> _stop({String? error}) async {
    try {
      await NfcManager.instance.stopSession(errorMessage: error);
    } catch (_) {/* session already gone */}
  }

  Future<void> _flash(NfcV nfcV, FirmwareImage image) async {
    _report('Handshake', 0, image.payload.length);

    // PING — discover bootloader.
    var rsp = await _exchange(nfcV, _frame(kCmdPing, 0, Uint8List(0)));
    _expect(rsp, kStReady, 'PING');

    // START — header + signature.
    final start = Uint8List(kSfuPreambleLen)
      ..setRange(0, kSfuHeaderLen, image.header)
      ..setRange(kSfuHeaderLen, kSfuPreambleLen, image.signature);
    rsp = await _exchange(nfcV, _frame(kCmdStart, 0, start));
    _expect(rsp, kStReady, 'START');

    // DATA — stream payload.
    final payload = image.payload;
    final frames = image.totalDataFrames;
    int seq = 0;
    while (seq < frames) {
      final off = seq * kMaxData;
      final end = (off + kMaxData).clamp(0, payload.length);
      final chunk = Uint8List.sublistView(payload, off, end);
      rsp = await _exchange(nfcV, _frame(kCmdData, seq, chunk));
      final status = rsp[0];
      final ctx = rsp[1] | (rsp[2] << 8);
      if (status == kStAck) {
        seq++;
        _report('Flashing', end, payload.length);
      } else if (status == kStRetry) {
        seq = ctx; // resend from expected seq
      } else {
        throw FlashException('DATA seq=$seq -> ${statusName(status)}');
      }
    }

    // FINISH — verify + commit.
    _report('Verifying', payload.length, payload.length);
    rsp = await _exchange(nfcV, _frame(kCmdFinish, 0, Uint8List(0)));
    _expect(rsp, kStOk, 'FINISH');
    _report('Done', payload.length, payload.length);
  }

  // ---- frame / mailbox helpers ------------------------------------------

  Uint8List _frame(int type, int seq, Uint8List data) {
    final f = Uint8List(4 + data.length);
    f[0] = type;
    f[1] = seq & 0xFF;
    f[2] = (seq >> 8) & 0xFF;
    f[3] = data.length;
    f.setRange(4, 4 + data.length, data);
    return f;
  }

  void _expect(Uint8List rsp, int wanted, String where) {
    if (rsp[0] != wanted) {
      throw FlashException('$where -> ${statusName(rsp[0])}');
    }
  }

  /// One request/response round-trip through the EEPROM mailbox.
  Future<Uint8List> _exchange(NfcV nfcV, Uint8List frame) async {
    // 1. clear MCU flag, 2. write request, 3. set phone flag.
    await _writeBlock(nfcV, _blkMcFlag, const [0, 0, 0, 0]);
    await _writeBlocks(nfcV, _blkReq, frame);
    await _writeBlock(nfcV, _blkPhFlag, const [1, 0, 0, 0]);

    // 4. poll MCU flag.
    final deadline = DateTime.now().add(_pollTimeout);
    while (true) {
      final flag = await _readBlock(nfcV, _blkMcFlag);
      if (flag[0] == 1) break;
      if (DateTime.now().isAfter(deadline)) {
        throw FlashException('Timeout waiting for device response');
      }
    }

    // 5. read response, 6. clear phone flag.
    final rsp = await _readBlocks(nfcV, _blkRsp, _rspBlocks);
    await _writeBlock(nfcV, _blkPhFlag, const [0, 0, 0, 0]);
    return rsp;
  }

  Future<Uint8List> _readBlock(NfcV nfcV, int block) async {
    final cmd = Uint8List.fromList([_iso15693Flags, 0x20, block & 0xFF]);
    final r = await nfcV.transceive(data: cmd);
    // r[0] = response flags; r[1..4] = block data.
    if (r.isEmpty || r[0] != 0x00) {
      throw FlashException('Read block $block failed (flags=0x${r.isEmpty ? '?' : r[0].toRadixString(16)})');
    }
    return Uint8List.sublistView(r, 1, 5);
  }

  Future<Uint8List> _readBlocks(NfcV nfcV, int firstBlock, int count) async {
    final out = Uint8List(count * 4);
    for (int i = 0; i < count; i++) {
      out.setRange(i * 4, i * 4 + 4, await _readBlock(nfcV, firstBlock + i));
    }
    return out;
  }

  Future<void> _writeBlock(NfcV nfcV, int block, List<int> data4) async {
    final cmd = Uint8List.fromList(
        [_iso15693Flags, 0x21, block & 0xFF, ...data4]);
    final r = await nfcV.transceive(data: cmd);
    if (r.isNotEmpty && r[0] != 0x00) {
      throw FlashException('Write block $block failed (flags=0x${r[0].toRadixString(16)})');
    }
    // ST25DV EEPROM page programming time (~5 ms / 4-byte page).
    await Future.delayed(const Duration(milliseconds: 5));
  }

  Future<void> _writeBlocks(NfcV nfcV, int firstBlock, Uint8List data) async {
    // pad to a multiple of 4 bytes.
    final padded = (data.length % 4 == 0)
        ? data
        : (Uint8List(((data.length + 3) ~/ 4) * 4)..setRange(0, data.length, data));
    for (int i = 0; i < padded.length; i += 4) {
      await _writeBlock(
          nfcV, firstBlock + i ~/ 4, padded.sublist(i, i + 4));
    }
  }

  void _report(String phase, int sent, int total) =>
      onProgress?.call(FlashProgress(phase, sent, total));
}
