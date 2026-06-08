# STICKER NFC Flasher

Minimal Android app that downloads a STICKER firmware image (from a direct URL
or a GitHub release) and streams it to a device over NFC, using the on-board
**ST25DV** tag. Implements the phone side of
[`doc/nfc-update-protocol.md`](../../doc/nfc-update-protocol.md) (variant B —
erase-in-place).

The app is a pure transport: it holds no keys and performs no crypto. The
firmware bootloader verifies the image signature; the phone only carries bytes.

## Bootstrap (one-time)

The platform scaffolding (`android/` Gradle project) is **not** committed — only
`lib/`, `pubspec.yaml` and a reference manifest are. Generate the rest locally:

```bash
cd tools/nfc-flasher
flutter create --org com.hardwario --project-name nfc_flasher .
flutter pub get
```

Then merge the NFC entries from the reference manifest into the generated
`android/app/src/main/AndroidManifest.xml`:

```xml
<uses-permission android:name="android.permission.NFC" />
<uses-feature android:name="android.hardware.nfc" android:required="true" />
```

## Run

```bash
flutter run            # Android device with NFC enabled
```

1. Paste a link — either a direct `.hex` / `.bin` / `.sfu` URL, or a GitHub
   release page (`…/releases/latest` or `…/releases/tag/<tag>`). For private
   repos add a GitHub token.
2. Tap **Download** — the app fetches and parses the image (Intel HEX is
   converted to a contiguous binary; `.sfu` is parsed with its signed header).
3. Tap **Flash via NFC** and hold the phone against the STICKER until it
   reports 100 %. The device verifies and reboots into the new firmware.

## Notes & limitations

- **Transport binding:** EEPROM software mailbox (baseline, ISO15693
  `Read/Write Single Block`). A 132 KB image takes a few minutes — hold the
  phone steady. The faster **FTM mailbox** binding is a planned follow-up.
- **Unsigned images:** the "Allow unsigned" switch is for debug builds only;
  production firmware refuses unsigned images.
- **NFC plugin:** targets `nfc_manager ^3.5.0` (Android `NfcV.transceive`). If
  you bump the plugin major version, adjust the session/transceive calls in
  `lib/src/nfc_flasher.dart`.
- Not yet tested end-to-end against the bootloader (Phase 2) — protocol
  constants are shared via `lib/src/protocol.dart` ⇄ firmware `nfc_proto.h`.
