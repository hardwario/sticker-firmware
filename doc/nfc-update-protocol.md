# NFC firmware update protocol (variant B — erase-in-place)

Shared contract between the firmware NFC bootloader and the `tools/nfc-flasher` Flutter app.
All multi-byte fields are **little-endian** unless stated otherwise.

> Status: draft v2. The logical frame layer is transport-agnostic; the physical binding is the
> **ST25DV FTM mailbox** (§4). The earlier EEPROM software-mailbox binding was dropped (slow,
> wears the EEPROM, manual RF↔I2C arbitration). Numeric constants are mirrored in firmware
> (`include/sticker/nfc_proto.h`) and the flasher (`tools/nfc-flasher/lib/src/protocol.dart`).

## 1. Design summary

- STM32WLE5CC has no native NFC. NFC comes from an **ST25DV** dynamic tag (ISO15693 / NfcV on
  the RF side, I2C on the MCU side, addr `0x53`, powered through the `lpd` power-switch PA1).
- The phone is the NFC reader/writer; the bootloader is the I2C host. The image never travels
  over LoRa.
- **Erase-in-place, no fallback** (256 KB part has no room for an A/B slot). Acceptable because
  the operation is operator-present: a failed/interrupted update is simply retried on the spot.
  The bootloader is never erased, so the device is always recoverable via NFC.
- **Security = pre-signed image**: CI signs the image with a private key; the bootloader verifies
  the signature against a baked-in **public key**. The phone app is a pure transport — it holds
  no secret and performs no crypto. DFU entry is requested by the authenticated `enter_dfu` NFC
  command (§6), so a stray NFC field cannot push a device into DFU.

## 2. Update image format (`.sfu`)

Produced by CI (Phase 3), transported verbatim by the app, verified by the bootloader.

```
+---------------------+  offset 0
|  sfu_header (32 B)   |
+---------------------+  offset 32
|  signature (64 B)    |  Ed25519 over header[0..31] || payload
+---------------------+  offset 96
|  payload (firmware)  |  raw slot0 image, payload_len bytes
+---------------------+  offset 96 + payload_len
```

```c
struct sfu_header {        /* 32 bytes, little-endian */
    uint8_t  magic[4];     /* "SNFU" = 0x53 0x4E 0x46 0x55 */
    uint16_t hdr_version;  /* = 1 */
    uint16_t flags;        /* bit0 SIGNED, bit1 CRC_PRESENT */
    uint32_t fw_version;   /* (major<<24)|(minor<<16)|(patch<<8)|build_type */
    uint32_t payload_len;  /* firmware byte count (<= slot0 size) */
    uint32_t load_addr;    /* target flash base, e.g. 0x08008000 (slot0) */
    uint32_t payload_crc32;/* CRC-32/IEEE of payload */
    uint8_t  reserved[8];  /* zero */
};
```

- The app may also accept a **raw `.bin`** or **Intel `.hex`** with no header — in that case it
  builds a header itself (no signature, `flags=CRC_PRESENT`). The bootloader rejects unsigned
  images unless built in a `DEV` configuration (debug builds only).

## 3. Logical frame layer (transport-agnostic)

Every exchange is a request frame (phone → MCU) followed by a response frame (MCU → phone).

### Request frame
```
byte 0      type      (CMD_*)
byte 1..2   seq       u16 (DATA only; else 0)
byte 3      len       payload byte count (0..MAX_DATA)
byte 4..    data      len bytes
```

### Response frame
```
byte 0      status    (ST_*)
byte 1..2   ctx        u16 (e.g. acked/expected seq)
byte 3..    detail     optional
```

### Commands
| Name        | Code | Payload                          | MCU action |
|-------------|------|----------------------------------|------------|
| `CMD_START` | 0x01 | `session`(4) + `sfu_header`(32) [AES-CCM] | Set the CCM session, decrypt/validate the header, erase slot0. Reply `ST_READY` or `ST_ERR_*`. (Unkeyed factory devices accept a plaintext header.) |
| `CMD_DATA`  | 0x02 | `seq`, `data[len]`               | Write `data` at `seq * MAX_DATA` into slot0. Reply `ST_ACK(seq)` or `ST_RETRY(expected_seq)`. |
| `CMD_FINISH`| 0x03 | —                                | Verify payload CRC32, then signature over header+payload. On success write the **valid metadata** block, reply `ST_OK`, reboot into app. On failure reply `ST_ERR_VERIFY` (slot0 stays invalid → DFU-wait). |
| `CMD_ABORT` | 0x04 | —                                | Drop session; slot0 stays erased/partial → DFU-wait. |
| `CMD_PING`  | 0x05 | —                                | Reply `ST_READY` + bootloader version + max_data + slot0 size. Handshake/discovery. |

### Status codes
| Name           | Code | Meaning |
|----------------|------|---------|
| `ST_READY`     | 0x10 | Ready / handshake ok |
| `ST_ACK`       | 0x11 | Chunk written (ctx = seq) |
| `ST_RETRY`     | 0x12 | Out-of-order; resend from ctx = expected_seq |
| `ST_OK`        | 0x13 | Image verified & committed |
| `ST_ERR_MAGIC` | 0x20 | Bad magic / header |
| `ST_ERR_SIZE`  | 0x21 | payload_len > slot0 |
| `ST_ERR_FLASH` | 0x22 | Erase/write failure |
| `ST_ERR_VERIFY`| 0x23 | CRC or signature mismatch |
| `ST_ERR_STATE` | 0x24 | Command not valid in current state |

### State machine (bootloader, DFU mode)
```
IDLE --CMD_START(ok)--> ERASED --CMD_DATA*--> RECEIVING --CMD_FINISH(ok)--> COMMIT -> reboot
  ^                                                |                          |
  +----- CMD_ABORT / error / timeout -------------+--------------------------+
```
Each `CMD_DATA` frame carries up to `MAX_DATA` (240) frame bytes, of which up to **232 are
plaintext firmware** (`MAX_PLAINTEXT`; the remaining 8 are the AES-CCM tag when keyed). The
bootloader writes frame `seq` at `seq * MAX_PLAINTEXT`, so the phone chunks the payload by 232
regardless of keyed/unkeyed. The phone must honour `ST_RETRY` and resend from `expected_seq`. A
bootloader DFU-session timeout (e.g. 30 s with no frame) returns to DFU-wait without bricking.

## 4. Physical binding — FTM mailbox

The ST25DV 256-byte **Fast-Transfer-Mode mailbox** (volatile RAM, no EEPROM wear, no 5 ms page
programming) carries the logical frames of §3, half-duplex over a single shared buffer.

**RF side (phone, ISO15693 custom commands, manufacturer code `0x02`):**
- **Write Message (0xAA)** — `[flags 0x02][0xAA][mfg 0x02][MBLength = len-1][data…]`: writes the
  request frame into the mailbox, arming `RF_PUT_MSG`.
- **Read Msg Length (0xAB)** — `[0x02][0xAB][0x02]` → `[resp_flags][MB_LEN_Dyn = len-1]`.
- **Read Message (0xAC)** — `[0x02][0xAC][0x02][pointer 0x00][NbBytes = len-1]` →
  `[resp_flags][data…]`: reads the response frame.

**MCU side (I2C, mirrors `app_nfc.c`):** enable the mailbox once — present the I2C password,
set the static `MB_MODE` allow-bit (E1 `0x000D`), set `MB_EN` in `MB_CTRL_Dyn` (E0 `0x2006`).
Then poll `MB_CTRL_Dyn` for `RF_PUT_MSG` (bit2); on a request read `MB_LEN_Dyn` (E0 `0x2007`) and
the mailbox RAM (E0 `0x2008`), process, and write the response back into the RAM (which arms
`HOST_PUT_MSG`, bit1, for the phone to read). GPO interrupt is optional — polling is sufficient.

Handshake per exchange: phone Write Message → MCU sees `RF_PUT`, reads + processes → MCU writes
response (arms `HOST_PUT`) → phone Read Message. A reply is distinguished from the phone's own
echoed request by its first byte (status codes ≥ 0x10; command codes 0x01..0x05). The phone
re-writes the request each poll window, which also covers the firmware's switch into mailbox mode
(it acks the `enter_dfu`/EnterMailbox request over NDEF first). Target ~40–60 s for a 132 KB image.

> The earlier EEPROM software-mailbox binding (polled flags + Write Single Block) was dropped: ~3–5
> min per image, EEPROM wear from thousands of chunk writes, and manual RF↔I2C arbitration. The FTM
> mailbox gets HW handshake (`RF_PUT`/`HOST_PUT`) for free.

## 6. DFU entry & boot decision

**Entry (into DFU-wait):** the bootloader enters DFU-wait when **either**
1. the running app received the **`enter_dfu` command over NFC** (protobuf, NFC-only). The app
   sets a magic word in a reserved retained-RAM slot (`include/sticker/dfu_signal.h`) and
   cold-reboots; the bootloader reads the word once and clears it. The word survives a software
   reset but is lost on power loss, so an aborted request cannot wedge the device in DFU. **or**
2. slot0 has **no valid metadata** (blank / failed / interrupted update).

(No physical/magnet gesture: the both-hall-magnets reset combo is reserved for calibration mode.)

**Valid metadata block:** a small dedicated flash record (own page) written only after a
successful `CMD_FINISH`:
```c
struct sfu_meta { uint32_t magic; uint32_t payload_len; uint32_t payload_crc32; uint32_t valid; };
```
**Boot path:** on reset the bootloader reads `sfu_meta`. If `valid == VALID_MAGIC` (and optionally
the slot0 CRC matches), it jumps to the app at `load_addr`. Otherwise it powers ST25DV via `lpd`
and stays in DFU-wait, polling the mailbox.

## 7. Flash layout (variant B)

No second slot. Indicative sizes (tuned once bootloader size is known):
```
mcuboot/boot   ~48 KB   0x00000000   NFC bootloader (read-only)
image-0/slot0  ~132 KB  0x0000C000   application (relinked here; code-partition)
sfu_meta        ~2 KB    ...          valid-metadata page
history         remainder              sensor store-and-forward (reduced)
storage          16 KB   0x0003C000   NVS (DevEUI/keys/config — preserved)
```

## 8. Open hardware items

- Is ST25DV populated on **all** HW revisions? (today NFC SW exists only in `sticker-1wire`).
- ST25DV variant 04K/16K/64K (512 B / 2 KB / 8 KB) — affects FTM availability and chunking.
- Is the GPO pin routed? (only needed for the FTM fast-follow; baseline polling does not need it).
