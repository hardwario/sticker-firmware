# STICKER NFC bootloader (variant B — erase-in-place)

Custom Zephyr bootloader image that receives a firmware image over NFC
(phone → ST25DV → I2C) and writes it directly into the application slot.
Implements the device side of [`doc/nfc-update-protocol.md`](../doc/nfc-update-protocol.md).

> **Status: WIP scaffold — not yet build-wired or HW-tested.** The core logic
> (ST25DV EEPROM mailbox, flash erase/write, CRC verify, boot decision + jump)
> is in place; the items in *Remaining work* below must be completed before it
> runs on hardware.

## Why a custom bootloader (not MCUboot swap)

The STM32WLE5CC has 256 KB flash. mcuboot(32 KB) + app(~132 KB) leaves no room
for a second A/B slot, so MCUboot's swap-based update does not apply. Variant B
writes the new image **in place** into slot0. There is no rollback copy; this is
acceptable because NFC update is operator-present (a failed/interrupted update is
retried on the spot) and the bootloader itself is never erased, so the device is
always recoverable.

## Design

```
reset ──► read sfu_meta (flash)
            │ valid && no DFU request          │ invalid / DFU requested
            ▼                                   ▼
        jump to app (slot0)               DFU mode (NFC wait)
                                                │
                          power ST25DV (lpd) ─► poll EEPROM mailbox
                                                │
              CMD_PING/START/DATA*/FINISH  ◄────┘   (doc §3 state machine)
                                                │
                       verify CRC32 + signature ─► write sfu_meta ─► reboot
```

- **Transport:** EEPROM software mailbox (doc §4), polled over I2C. No GPO pin
  needed. ST25DV I2C primitives are ported from `app/src/app_nfc.c`.
- **Boot decision:** `sfu_meta` (a small dedicated flash record) marks slot0
  bootable. Written only after a verified `CMD_FINISH`.
- **DFU entry:** the app receives the **`enter_dfu` NFC command** (protobuf,
  NFC-only), sets a magic word in reserved retained RAM
  (`include/sticker/dfu_signal.h`) and cold-reboots; the bootloader reads it once
  and clears it. (The both-hall-magnets reset combo is *not* used — it is
  reserved for calibration mode.) The bootloader also stays in DFU automatically
  when `sfu_meta` is invalid.
- **Security:** per-frame **AES-CCM** keyed by the per-device `secret_key`
  (provisioned into `sfu_meta` by the app); an all-zero key means unkeyed
  (factory bootstrap accepts plaintext frames). See `src/auth.c`.

## Files

| File | Role |
|------|------|
| `src/main.c`        | boot decision, jump-to-app, DFU loop / state machine |
| `src/st25dv_mb.[ch]`| ST25DV I2C EEPROM mailbox (req/rsp framing, handshake) |
| `src/flash_writer.[ch]` | slot0 erase/write, sfu_meta read/write |
| `src/verify.[ch]`   | CRC32 + Ed25519 signature verification (stub → Phase 3) |
| `prj.conf`          | minimal Zephyr config (i2c, flash, no logging in release) |
| `CMakeLists.txt`    | bootloader Zephyr application |

## Remaining work (before HW bring-up)

1. **Build wiring** — adopt sysbuild so two images build together
   (`bootloader` + `app`), or build/flash the bootloader separately to
   `boot_partition`. Today the app builds as a flat image with no bootloader.
2. **DTS partition redesign** — apply the variant-B layout (doc §7) to
   `boards/sticker/sticker.dts` in **all** variants: grow `boot_partition`,
   relink the app at the new slot0 base, add the `sfu_meta` page, shrink
   `history`. The app's `chosen { zephyr,code-partition }` already points at
   slot0.
3. **App provisions `sfu_meta`** — the app must write its `secret_key` + serial
   into `sfu_meta` at boot so the bootloader has the current AES-CCM key;
   without it every device stays unkeyed (factory bootstrap).
4. ~~**DFU-entry trigger**~~ — done: the `enter_dfu` NFC command sets the
   retained-RAM flag (`include/sticker/dfu_signal.h`) and the bootloader reads it.
5. **Size budget** — confirm the bootloader (I2C + flash + PSA AES-CCM) fits the
   36 KB `boot_partition` (currently ~31 KB).
6. **HW test** — end-to-end with `tools/nfc-flasher` against a real device.

## Build (once wired)

```bash
source /home/hymbajs/.venv/bin/activate
# separate-image build (interim, before sysbuild):
west build -b sticker boot --pristine -d build/boot
west flash -d build/boot          # writes boot_partition
```
