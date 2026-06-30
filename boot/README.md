# STICKER NFC bootloader (variant B — erase-in-place)

Custom Zephyr bootloader image that receives a firmware image over NFC
(phone → ST25DV → I2C) and writes it directly into the application slot.
Implements the device side of [`doc/nfc-update-protocol.md`](../doc/nfc-update-protocol.md).

> **Status: HW-validated.** End-to-end NFC firmware update works on hardware
> (keyed and unkeyed): `enter_dfu` → bootloader DFU-wait → AES-CCM frame transfer
> → CRC verify + commit → reboot. Build & flash via `make final_build` /
> `make final_flash` (see below). Open follow-ups: sysbuild (build both images in
> one step), per-variant DTS rollout, and image signing (issue #237).

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

## Build & flash

The bootloader and the variant-B app (the app relinked at `code_partition`) are
two separate images. The `app/Makefile` wraps building + flashing both. Run from
the **main checkout** (not a git worktree — the `sticker` board would otherwise be
defined twice and the build fails with "board defined multiple times").

```bash
source ~/.venv/bin/activate
cd app

make final_build     # build bootloader + variant-B app -> deploy/
                     #   sticker-bootloader.hex, sticker-app.{hex,bin,sfu}
make final_flash     # flash bootloader (@0x08000000) + app (@code_partition)
                     #   no --erase, so NVS (DevEUI/keys/config) is preserved
make final_clean     # remove build-boot / build-app-b
```

The bootloader only needs flashing once; afterwards the app is updated over NFC
(`enter_dfu` → flash the `.sfu` from the phone), no SWD/J-Link required.

### Examples

```bash
make final_build                       # 1.4.0, build_type CUSTOM(2)
make final_build BUILD_TYPE=0          # 1.4.0 MAIN (release)
make final_build BUILD_TYPE=1          # 1.4.0-dev (DEV branch build)
make final_build VERSION_MINOR=5 VERSION_PATCH=0   # 1.5.0
make final_flash JLINK_SN=822005110    # pick a specific J-Link

# Two test images differing only by version (flash one, then the other):
make final_build BUILD_TYPE=0 && cp deploy/sticker-app.sfu /tmp/fw-main.sfu
make final_build BUILD_TYPE=1 && cp deploy/sticker-app.sfu /tmp/fw-dev.sfu
```

Overridable vars: `VERSION_MAJOR/MINOR/PATCH`, `BUILD_TYPE` (0=MAIN 1=DEV 2=CUSTOM),
`LOAD_ADDR` (code_partition base, default `0x08009000`), `JLINK_SN`. The `.sfu`
header's `fw_version` = `(major<<24)|(minor<<16)|(patch<<8)|build_type`.

### Manual (without make)

```bash
west build -p always -b sticker boot -d build-boot
west build -p always -b sticker app  -d build-app-b -- -DEXTRA_CONF_FILE=nfc-boot.conf
west flash -d build-boot      # bootloader @0x08000000
west flash -d build-app-b     # variant-B app @code_partition
# package the .sfu for NFC flashing:
python3 ../scripts/mksfu.py build-app-b/zephyr/zephyr.bin out.sfu \
    --load-addr 0x08009000 --fw-version 0x01040000
```

> The variant-B app is **release-only** — the debug image is too large to fit
> `code_partition` behind the bootloader. CI builds the same images in the
> `build-nfc` job and attaches the `.sfu` to tagged releases.
