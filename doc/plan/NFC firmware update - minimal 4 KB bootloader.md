# NFC firmware update — minimal 4 KB bootloader

Issue: #491. Previous design: #139 (removed in #239), #237.

## 1. Goal and non-goals

**Goal:** upload a new STICKER application image over NFC (ST25DV FTM mailbox) with the
smallest and simplest bootloader possible.

**Non-goals (decided 2026-10-12):**

- no image signature, no encryption, no version / rollback check in the bootloader
- no force-DFU (hall magnets, button)
- no metadata partition, no image header format (`.sfu`)
- no bootloader update over NFC (J-Link only)
- no LoRaWAN / P2P FUOTA (no space to stage an image on a 256 KB part)

A failed upload is acceptable. The device stays in update mode and the operator repeats
the upload.

## 2. Why not Zephyr / MCUboot / the #139 bootloader

| Option | Size | Why not |
|---|---|---|
| #139 variant-B bootloader (Zephyr + mbedTLS CCM + RTT) | ~31 KB | too big; most of it is crypto and logging |
| Zephyr minimal (`MULTITHREADING=n`, no crypto, no log) | ~8–11 KB | kernel init, device model, clock control, pinctrl and generic driver APIs are dead weight here |
| MCUboot single-slot / serial recovery | > 20 KB | needs its own ST25DV transport anyway |
| A/B slots | – | 2 × ~170 KB does not fit 256 KB |
| **Bare-metal, CMSIS registers** | **~1.8 KB** | chosen; 4 KB partition = 2 flash pages (page = 2 KB) |

### Size estimate (Thumb-2, `-Os`)

| Block | Estimate |
|---|---|
| startup: 16-word vector table, reset handler, `.data`/`.bss` init | ~150 B |
| boot decision + jump to app | ~80 B |
| GPIO: `lpd` PA1 (ST25DV power), one LED | ~80 B |
| I2C1 polling: write/read with 16-bit register address | ~350 B |
| ST25DV mailbox: `MB_EN`, poll `MB_CTRL`, read/write mailbox RAM | ~300 B |
| flash: unlock, page erase, double-word program, lock | ~250 B |
| CRC32 on the STM32WL CRC peripheral (no table) | ~60 B |
| protocol START / DATA / FINISH / PING | ~350 B |
| wait: STOP2 + EXTI on GPO PB12, timeout | ~200 B |
| **total** | **~1.8 KB** of 4 KB |

The build fails above 4 KB (linker region `BOOT` = 4K).

## 3. Flash layout

| Partition | Address | Size | Notes |
|---|---|---|---|
| `boot_partition` | `0x00000` | 4K | bootloader, J-Link only |
| `code_partition` | `0x01000` | 204K | app, written over NFC |
| `history_partition` | `0x34000` | 32K | unchanged |
| `storage_partition` | `0x3C000` | 16K | unchanged; never touched by the bootloader (#108) |

- Release: `CONFIG_FLASH_LOAD_OFFSET=0x1000`, `CONFIG_FLASH_LOAD_SIZE=0x33000` (ends at history).
- Debug: same offset, budget ends at storage (`0x3C000`) because debug keeps history in RAM.
  `0x3B000` = 236K, debug is ~220 KB, so it fits.
- Zephyr sets `VTOR` from the link address. The vector table (~312 B) needs 512 B
  alignment, and `0x1000` satisfies it.

## 4. Boot decision

```
reset_handler:
  if RAM[DFU_MAGIC_ADDR] == 'DFU!':  clear it; update_mode(timeout = 5 min)
  elif app_valid():                   jump_to_app()
  else:                               update_mode(timeout = none)

app_valid():
  sp = *(u32*)0x1000; pc = *(u32*)0x1004
  return sp in [0x20000000, 0x20010000] && (pc & 1) && pc in (0x1000, 0x34000)

jump_to_app():
  disable SysTick / EXTI used by the bootloader, restore GPIO
  SCB->VTOR = 0x1000; __set_MSP(sp); branch pc
```

`DFU_MAGIC_ADDR` is a word at the end of SRAM, excluded from the app's RAM region (the same
idea as the #139 `0x2000FFE0` retained-RAM signal). It does not survive a power loss, which
is intended.

## 5. Update mode

```
update_mode(timeout):
  lpd = 1 (power ST25DV), wait t_boot
  MB_CTRL_Dyn (0x2006) = MB_EN
  LED on
  loop:
    sleep in STOP2 until EXTI PB12 (GPO RF_PUT_MSG) or timeout
    timeout -> NVIC_SystemReset()   (slot untouched -> old app boots)
    read MB_LEN_Dyn (0x2007), MB RAM (0x2008) -> request
    handle request, write reply to MB RAM
```

- The ST25DV **static** configuration (`MB_MODE`, GPO mask, passwords) is written by the
  application at boot and stays in the tag EEPROM. The bootloader only touches the dynamic
  `MB_CTRL` register, which needs no I2C password.
- GPO is on PB12, which is not a WKUP pin, so Standby cannot be used and STOP2 + EXTI is used
  instead. Expected current is in the low µA range plus the ST25DV. Measure it on HIL.
- The clock stays at MSI 4 MHz after reset (flash latency 0, no clock setup). I2C runs at
  100 kHz, which adds ~25 ms per 256 B frame. If transfer time matters, switch to MSI 48 MHz
  (flash latency 2) and I2C 400 kHz.

## 6. Protocol

One request and one reply per mailbox exchange. All values are little-endian.

| Request | Layout | Reply |
|---|---|---|
| `PING` 0x01 | `cmd` | `ACK`, `boot_version(1)`, `slot_size(4)` |
| `START` 0x02 | `cmd, len(4), crc32(4)` | `ACK` after erase (~1.7 s for 204K), `NAK` if `len` > slot or not a multiple of 8 |
| `DATA` 0x03 | `cmd, offset(4), data(240)` | `ACK`; or `NAK, expected(4)` |
| `FINISH` 0x04 | `cmd` | `ACK` then reset; or `NAK, reason(1)` |

Reply: `status(1)` = `0x00` ACK / `0x01` NAK, followed by the payload.

**DATA rules:**

- `offset == expected`: program (STM32WL programs flash in 8-byte double-words, so a chunk is
  240 B), `expected += n`
- `offset < expected`: already written, reply ACK without writing (a retried frame)
- `offset > expected`: reply `NAK, expected`, and the phone resumes from `expected`
- The **first 2 KB page of the slot is buffered in RAM**, not programmed.

**FINISH:**

1. HW CRC32 over the RAM page buffer + slot `0x1800 .. 0x1000+len` == `crc32` from START
2. vector table in the RAM buffer passes `app_valid()`
3. program the first page **from its end down, the first double-word (SP + reset PC) last**
4. reply ACK, reset

If anything fails, the first page stays erased, `app_valid()` fails, and the device returns to
update mode after a reset. **Validity needs no metadata.**

## 7. Application changes

- `boards/sticker/sticker.dts`: `boot_partition` 4K + `code_partition` `0x1000`/204K.
- `prj.conf` / `debug.conf`: `CONFIG_FLASH_LOAD_OFFSET`, `CONFIG_FLASH_LOAD_SIZE`.
- New NFC command `enter_dfu` on the encrypted channel (proto_id 22 is `reserved`, or a new
  id ≥ 33): write the magic, reply, reboot. Optionally refuse when battery is low.
- RAM: reserve the magic word at the end of SRAM.

## 8. Build, CI, production

- `boot/`: plain CMake (or Makefile) with `arm-zephyr-eabi-gcc` from the Zephyr SDK, CMSIS
  device headers from `modules/hal/stm32/stm32cube/stm32wlxx/soc`, own linker script.
- CI artifacts:
  - `sticker-boot.hex`
  - `sticker-app.bin` (NFC)
  - `sticker-factory.hex` (boot + app, for production and J-Link)
- Production flashes `sticker-factory.hex`. Devices with the flat layout (v1.4.0 / current
  v1.5.0) get the bootloader only through a one-time J-Link flash.
- Local J-Link flashing of the app keeps working (`west flash` writes from `0x1000`). A
  device without a bootloader needs `sticker-factory.hex` once.

## 9. Manager-App

Simplified MR !26 flow (no CCM, no `.sfu`):

1. pick a `.bin` (file or GitHub release); check that it is a STICKER release image
2. `enter_dfu` over the encrypted command channel
3. `PING` until the bootloader answers
4. `START(len, crc32)`, then `DATA` from offset 0, resuming from `expected` on NAK, then `FINISH`
5. on any failure, **Try again** restarts from `PING` / `START`

## 10. Accepted trade-offs

- A device in update mode accepts any image from a phone at NFC distance.
- No recovery without J-Link if a new application boots but its NFC / `enter_dfu` is broken.
  An `enter_dfu` HIL check is mandatory for every release.
- No version / board check in the bootloader beyond CRC and vector validity.

## 11. HIL test plan

| # | Test | Expected |
|---|---|---|
| 1 | update release → release | new version runs, NVS/identity/LoRaWAN session kept |
| 2 | update debug → debug | same |
| 3 | phone removed mid-transfer, then retry | resumes or restarts, completes |
| 4 | power loss during DATA | update mode after power-up, update completes |
| 5 | power loss during FINISH (first page) | update mode or new app, never a hard fault |
| 6 | wrong CRC in START | FINISH NAK, device stays in update mode |
| 7 | `enter_dfu` and no phone for 5 min | old app boots again |
| 8 | current in update-mode wait (PPK2) | low µA + ST25DV |
| 9 | bootloader size | ≤ 4096 B |
| 10 | transfer time for ~170 KB | measured, documented |
