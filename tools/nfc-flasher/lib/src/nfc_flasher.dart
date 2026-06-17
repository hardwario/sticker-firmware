// ISO15693 (NfcV) firmware streaming to an ST25DV tag over the FTM (Fast
// Transfer Mode) mailbox per doc/nfc-update-protocol.md §4. Each exchange writes
// a request into the 256-byte mailbox RAM with the ST custom Write Message
// (0xAA) command and polls Read Message (0xAB/0xAC) for the bootloader's reply.
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

// ISO15693 request flags: high data rate, non-addressed.
const int _iso15693Flags = 0x02;
// ST IC manufacturer code (carried by ST custom commands).
const int _stMfgCode = 0x02;
// ST25DV custom mailbox commands.
const int _cmdWriteMessage = 0xAA;
const int _cmdReadMsgLength = 0xAB;
const int _cmdReadMessage = 0xAC;

// How long to wait for a single exchange's reply before giving up.
const Duration _pollTimeout = Duration(seconds: 3);
// The first exchange of a session must also cover the firmware's switch into
// mailbox mode (it acks EnterMailbox over NDEF first), so allow longer.
const Duration _firstPollTimeout = Duration(seconds: 8);
const Duration _pollInterval = Duration(milliseconds: 40);

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

    // PING — discover bootloader (also covers the switch into mailbox mode).
    var rsp = await _exchange(nfcV, _frame(kCmdPing, 0, Uint8List(0)),
        first: true);
    _expect(rsp, kStReady, 'PING');

    // START — session(4) + sfu_header(32). The bootloader diversifies its
    // AES-CCM nonce with the session; an unkeyed (factory) device ignores it
    // and accepts the plaintext header. The Ed25519 signature is not sent: the
    // bootloader authenticates with the per-device AES-CCM key, not the .sfu
    // signature.
    final session = DateTime.now().millisecondsSinceEpoch & 0xFFFFFFFF;
    final start = Uint8List(4 + kSfuHeaderLen);
    start.buffer.asByteData().setUint32(0, session, Endian.little);
    start.setRange(4, 4 + kSfuHeaderLen, image.header);
    rsp = await _exchange(nfcV, _frame(kCmdStart, 0, start));
    _expect(rsp, kStReady, 'START');

    // DATA — stream payload, kMaxPlaintext bytes per frame (matches the
    // bootloader's seq * kMaxPlaintext write offset).
    final payload = image.payload;
    final frames = image.totalDataFrames;
    int seq = 0;
    while (seq < frames) {
      final off = seq * kMaxPlaintext;
      final end = (off + kMaxPlaintext).clamp(0, payload.length);
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

  /// One request/response round-trip through the FTM mailbox. Writes [frame]
  /// with Write Message, then polls Read Message until the bootloader's reply
  /// lands. The request is re-written each outer pass (covers a missed write or
  /// the firmware's switch into mailbox mode). A reply is told apart from our
  /// own echoed request by its first byte: status codes start at 0x10, command
  /// codes are 0x01..0x05.
  Future<Uint8List> _exchange(NfcV nfcV, Uint8List frame,
      {bool first = false}) async {
    final deadline =
        DateTime.now().add(first ? _firstPollTimeout : _pollTimeout);
    while (DateTime.now().isBefore(deadline)) {
      await _writeMessage(nfcV, frame);

      final innerEnd = DateTime.now().add(const Duration(milliseconds: 1200));
      while (DateTime.now().isBefore(innerEnd) &&
          DateTime.now().isBefore(deadline)) {
        await Future.delayed(_pollInterval);
        final msg = await _readMessage(nfcV);
        if (msg != null && msg.isNotEmpty && msg[0] >= 0x10) return msg;
      }
    }
    throw FlashException('Timeout waiting for device response');
  }

  /// ST25DV Write Message (0xAA): place [data] in the mailbox RAM (arms RF_PUT).
  /// Frame: flags, cmd, IC-mfg(0x02), MBLength(=len-1), data.
  Future<void> _writeMessage(NfcV nfcV, Uint8List data) async {
    if (data.isEmpty || data.length > 256) {
      throw FlashException('Mailbox: bad message length ${data.length}');
    }
    final cmd = Uint8List.fromList(
        [_iso15693Flags, _cmdWriteMessage, _stMfgCode, data.length - 1, ...data]);
    await nfcV.transceive(data: cmd);
  }

  /// ST25DV Read Msg Length (0xAB) + Read Message (0xAC). Returns the mailbox
  /// payload (without the leading response-flags byte), or null if not ready.
  Future<Uint8List?> _readMessage(NfcV nfcV) async {
    final lenResp = await nfcV
        .transceive(data: Uint8List.fromList([_iso15693Flags, _cmdReadMsgLength, _stMfgCode]));
    if (lenResp.length < 2 || lenResp[0] != 0x00) return null;
    final len = lenResp[1] + 1;
    final msgResp = await nfcV.transceive(
        data: Uint8List.fromList(
            [_iso15693Flags, _cmdReadMessage, _stMfgCode, 0x00, len - 1]));
    if (msgResp.isEmpty || msgResp[0] != 0x00) return null;
    return Uint8List.fromList(msgResp.sublist(1));
  }

  void _report(String phase, int sent, int total) =>
      onProgress?.call(FlashProgress(phase, sent, total));
}
