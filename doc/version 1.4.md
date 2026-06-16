# HARDWARIO STICKER — Firmware v1.4.0 — What's New

This document lists **only the changes introduced in firmware v1.4.0** relative to the v1.3.x series — new features, new messages, new commands and new configuration options. Existing v1.3.x behaviour (basic telemetry, LoRaWAN keys, alarm thresholds, counters, corrections, calibration mode) is unchanged unless noted.

---

## Overview of changes

| Area | Change |
|---|---|
| Remote control | **New** bidirectional command protocol over LoRaWAN downlinks (fPort 85) |
| Telemetry | **New** protobuf telemetry on **fPort 2** (legacy fPort 1 bitmap no longer emitted); large reports split across frames |
| Alarms | **New** alarm-detail batch on **fPort 3**; **new** global alarm rate-limit; **new** PIR motion alarm |
| Device info | **New** automatic device-info uplink on every join |
| Time | **New** real-time clock, synced from the network |
| History | **New** sensor history store-and-forward with on-request replay |
| Shell | **New** `clock` and `history` commands; test command renamed `tester` → `ats` |
| Payload formatter | `ttn.js` extended to decode fPorts 2/3/85 and to **encode downlink commands** |
| Config keys | Threshold-alarm and motion keys renamed to a **sensor-centric** scheme (see §9) |

---

## 1. Remote control over LoRaWAN (NEW)

The device now accepts commands as **LoRaWAN downlinks on fPort 85** and replies on fPort 85. Author commands with the payload formatter (`encodeDownlink`, §8) or paste raw hex.

| Command | Purpose | Reply |
|---|---|---|
| `get_info` | Firmware version, serial, uptime, wall-clock, build type | `info` (no ack) |
| `set_param` | Change any configuration parameters (LoRaWAN/application/sensors/alarms) | `ack` |
| `get_param` | Read back selected parameters | `config_dump` |
| `get_config` | Dump the whole configuration (paged) | `config_dump` |
| `settings_save` | Persist staged changes (**reboots**) | `ack` |
| `reboot` | Cold reboot | `ack` |
| `factory_reset` | Reset config to defaults but **keep device identity + LoRaWAN keys** (stays provisioned/connected); clears dynamic alarm rules; **reboots** | `ack` |
| `force_send` | Send a telemetry report immediately | **none** — the report itself is the reply |
| `reset_counters` | Clear selected hall/input counters | `ack` |
| `clock_sync` | Request a network time sync | **`info`, deferred** — sent once the network time lands (carries the synced `unix_time`) |
| `req_history` | Replay stored history for a time window | `history_frame` (multiple) |
| `w1_scan` | Enumerate the 1-Wire bus; returns the discovered ROMs so you can teach a slot via `set_param sensorN_rom` | `w1_scan` |
| `alarm_rule` | Set / clear a dynamic alarm rule in a `slot`, or clear-all (SET/CLEAR require `slot`; the slot index is the rule's stable identity) | `ack` |
| `req_alarm_rules` | Read back the stored dynamic alarm rules (paged); optional `slot` filter selects one slot, or `source`+`quantity` selects every slot on that pair | `alarm_rules_dump` |

> After `set_param`, send `settings_save` to persist (it reboots). If a command fails, the device returns an `error` with a code (1 = BAD_REQUEST, 2 = OUT_OF_RANGE, 3 = NOT_READY, 4 = HISTORY_UNAVAILABLE, 5 = UNSUPPORTED_FIELD, 6 = PERSIST_FAILED), an optional `fault_field`, and a `detail` string.
>
> **No redundant acks:** commands whose real answer is the data they produce (`get_info`, `force_send`, `clock_sync`, `req_history`, `w1_scan`) do **not** also send an `ack`, to save an uplink.
>
> **LoRaWAN-only commands:** `force_send`, `clock_sync` and `req_history` answer only via an uplink, so they are rejected (`NOT_READY` "lrw only") if sent over NFC.

**Ready-to-use hex downlinks (fPort 85):**

| Command | Hex |
|---|---|
| `get_info` | `08032200` |
| `settings_save` | `08043200` |
| `clock_sync` | `08056200` |
| `force_send` | `08064a00` |
| `reboot` | `08083a00` |
| `w1_scan` | `08097200` |
| `reset_counters` (hall-left + input-a) | `0807520408011801` |
| `set_param`: ADR on, `interval_report`=120 s, `cap_barometer` on | `0801120d0a021801120220782203e80201` |
| `alarm_rule` SET slot 0 = s1 temperature 15–25 °C, hyst 0.5 | `08016a15100120012d00007041350000c8413d0000003f5000` |
| `alarm_rule` CLEAR slot 0 | `08016a0408015000` |
| `alarm_rule` CLEAR_ALL | `08016a020802` |
| `req_alarm_rules` (all rules, page 0) | `08017a00` |
| `req_alarm_rules` (slot 0 only) | `08017a022000` |
| `req_alarm_rules` (all slots on onboard temperature) | `08017a0408001000` |

(The leading byte is the `seq` you chose; it is echoed in the reply.)

> **Single source of truth (dev note):** the command list, wire ids, dispatch routing and per-command availability live in `app/src/app_config.yml` (`commands:`). `west configen` generates the proto `Command` oneof, the `ttn.js` `_CMD_NAMES` map and the firmware `app_cmd_dispatch()` switch from it, so the three never drift. Adding/removing a command = editing that list (proto ids are append-only and guarded).

---

## 2. New telemetry format on fPort 2 (protobuf)

Periodic and event reports now use a compact, extensible **protobuf** format on **fPort 2**. The legacy bitmap format is **no longer emitted** — the device transmits only the new ports (2 telemetry, 3 alarm, 10 calibration, 85 response/history). `decodeUplink` still understands a legacy fPort 1 frame so historical captures decode, but no current firmware produces one.

Key differences from the v1.3.x bitmap:
- **Extensible** — new sensors can be added in future firmware without breaking older decoders.
- **Capability-gated, whole-group** — a sensor's group is sent in full **every report** whenever its capability is enabled, even when the values are `0`/`false` (e.g. a hall counter at 0, all states inactive). The **system group** (voltage, `boot`) is **always** present, so `boot=false` is reported explicitly rather than by omission. Digital fields carry their real value (0 is valid); an analog scalar is omitted only when there is no valid sample yet.
- **Multi-frame split** — if a report is larger than the current data rate allows, it is split across several fPort-2 frames sent a few seconds apart. Each frame carries whole sensor groups from the **same snapshot**, so the network server can merge them; nothing is lost. The 1-Wire list (below) splits **per reading** — a single frame may carry only some of the slots, the rest follow in the next frame.

The decoded field set (voltage, temperature, humidity, pressure/altitude, illuminance, orientation, motion_count, 1-Wire sensors, hall/input counters and states, `boot`) matches the familiar v1.3.x fields — see the payload formatter output (§8). Decode with the updated `ttn.js`.

### 1-Wire sensors — repeated, self-describing (changed)

The fixed `ext1/ext2` and `mp1/mp2` telemetry fields are replaced by a single **repeated `SensorReading`** list, one entry per ROM-bound slot. The slot's **type travels with the reading**, so a heterogeneous mix (and future sensor types) needs no new fields.

```proto
message SensorReading {
    uint32 slot              = 1;   // 1-based slot index (matches sensorN / `sensor list`)
    uint32 type              = 2;   // 1 = dallas (temperature-only), 2 = machine-probe (temp + humidity + tilt)
    optional sint32 temperature = 3; // °C ×100
    optional uint32 humidity    = 4; // %RH ×2  (machine-probe only)
    optional uint32 flags       = 5; // bit0 = tilt (machine-probe only)
}
// Telemetry: repeated SensorReading w1_sensors = 27;   // replaces old fields 10–17
```

`ttn.js` decodes this into `w1_sensors[]`, each `{ slot, type, type_name, temperature?, humidity?, tilt_alert }`. A slot keeps its identity across reboots/rescans (ROM-bound), so `slot 2` is always the same physical sensor — not a function of bus enumeration order.

A runtime `w1_scan` / enroll re-binds the slots and re-arms machine-probe tilt detection on the spot — **no reboot is needed** for either the slot→sensor mapping or tilt alerts to stay correct after a rescan. Machine-probe readbacks are integrity-checked (SHT serial CRC; the tilt status bit is rejected if the 1-Wire readback is corrupt) so a noisy bus can't bind the wrong sensor or raise a false tilt.

---

## 3. Alarm-detail batch on fPort 3 (NEW)

When alarms occur, the device sends a batch of alarm events on **fPort 3** as a **protobuf `AlarmReport`** (same wire family as the fPort-2 telemetry and the fPort-85 responses), so it decodes with the generated codec rather than a packed byte layout.

### Message

```proto
message AlarmReport {
    uint32 base_time = 1;            // Unix time the events are relative to (uptime-s until the clock syncs)
    uint32 total     = 2;            // every alarm in the window (may exceed events[] if trimmed)
    repeated AlarmEvent events = 3;  // up to 8, further trimmed to the data-rate budget
}
message AlarmEvent {
    Source source = 1;               // which sensor
    Edge   edge   = 2;               // ACTIVATE / DEACTIVATE
    Side   side   = 3;               // SIDE_NONE / SIDE_LO / SIDE_HI
    uint32 rel_s  = 4;               // seconds since base_time
    optional sint32 value = 5;       // current reading, scaled; absent for discrete sources
    uint32 quantity = 6;             // which quantity (temperature/humidity/.../state/count)
    uint32 slot     = 7;             // alarm rule slot index that fired (stable identity)
}
```

Each event is described by **three orthogonal enums** — an integration that only wants "did `hall-left` activate?" or "did `temperature` cross its high bound?" reads one field instead of unpacking a combined flag.

### Decoded fields (`ttn.js`)

- `base_time` — Unix timestamp the events are relative to.
- `total` — total alarms that occurred in the window. `truncated` is `true` when `total` exceeds the events actually carried (some were dropped to fit the data rate).
- `alarms[]` — each with:
  - `slot` — the alarm rule slot index that fired (`0` when omitted on the wire); lets a host map the edge back to the exact rule, including which level of a multi-level alarm
  - `source` — `temperature` / `humidity` / `pressure` / `t1-temperature` / `t2-temperature` / `hall-left` / `hall-right` / `pir` / `input-a` / `input-b` / `accel-motion`
  - `event` — `activate` / `deactivate`
  - `side` — `hi` / `lo` for threshold sources, `none` for discrete sources (hall/input/PIR). The **deactivate** edge keeps the side that was crossed on activation.
  - `value` — the current reading at the edge (temperature/humidity in °C/%RH, pressure in hPa); `null` for discrete sources. *(Threshold and hysteresis are not carried — they are the device's own configuration.)*
  - `time` — `base_time + rel_s` (per-event Unix time)

### When the messages are sent (fPort 2 vs fPort 3)

An alarm edge produces **two independent uplinks**:

| Port | Content | Timing |
|---|---|---|
| **fPort 2** | a normal telemetry snapshot (current sensor values) | **immediately** on the first edge, so the backend sees fresh values at once |
| **fPort 3** | the `AlarmReport` with the list of edges collected during the window | **at the end of the collection window** (`alarm-limit` seconds after the first edge) |

The collection window is **shared across all sources**: the first edge opens it, and every edge during the window (any sensor, both activate and deactivate) accumulates into **one** `AlarmReport`. The immediate fPort-2 telemetry is **rate-limited to once per window** (`alarm-limit`, see §7) — the first edge sends it, later edges in the same window only add to the batch. Periodic telemetry continues on its own `interval_report` cadence.

**Example — temperature crosses its high threshold** (`alarm-limit = 20 s`):

```
t = 0 s    threshold crossed → fPort 2 telemetry (immediate); 20 s window opens
t ≈ 20 s   window flushes → fPort 3 AlarmReport
           { total:1, alarms:[{ source:"temperature", event:"activate", side:"hi", value:26.5, time:base }] }
```

When the temperature later falls back below the band, the deactivate edge behaves the same way (immediate fPort-2 if the rate-limit has elapsed, then `event:"deactivate", side:"hi"` in the next window's fPort-3).

**Example — a hall sensor is activated then deactivated within one window:**

```
t = 0 s    magnet applied   → hall-left activate   → fPort 2 (immediate); window opens
t = 8 s    magnet removed   → hall-left deactivate → batched (fPort-2 suppressed by rate-limit)
t ≈ 20 s   window flushes   → fPort 3 AlarmReport {
             total:2, alarms:[
               { source:"hall-left", event:"activate",   side:"none", value:null, time:base+0 },
               { source:"hall-left", event:"deactivate", side:"none", value:null, time:base+8 } ] }
```

> Hall/input/PIR edges are only raised (and only collected) when their `*-notify-act` / `*-notify-deact` configuration is enabled and the sensor's `cap-*` capability is on. With `alarm-limit = 0` there is no window: each edge is sent immediately as its own one-event fPort-3 frame and the fPort-2 telemetry is not rate-limited.

---

## 4. Device info on join (NEW)

After every LoRaWAN join the device automatically sends a device-info message on **fPort 85** — before the first telemetry — so the network knows the unit's identity and firmware up front. Fields:

| Field | Meaning |
|---|---|
| `fw_version` (`fw_major`/`minor`/`patch`) | Firmware version |
| `build_type_name` | `main` (release), `dev` (CI), `custom` (local) |
| `debug` | `true` for debug builds |
| `serial_number` | Device serial |
| `uptime_s` | Seconds since boot |
| `unix_time` | Wall-clock UTC (0 until the clock is synced) |

The same message is returned on demand by the `get_info` command.

---

## 5. Real-time clock (NEW)

The device now keeps wall-clock time, synchronised from the network via LoRaWAN `DeviceTimeReq` (requested automatically on join). It timestamps history records and alarm events.

New `clock` shell command:

| Command | Description |
|---|---|
| `clock get` | Read RTC time (Unix timestamp) |
| `clock set <unix>` | Set RTC time manually |
| `clock sync` | Request network time |

The `clock_sync` downlink command (§1) triggers the same sync remotely.

---

## 6. Sensor history — store-and-forward (NEW)

The device can record sensor readings to flash and replay a chosen time window on request. Records survive power loss.

New configuration:

| Shell key | Type | Default | Description |
|---|---|---|---|
| `history-enable` | bool | `false` | Master switch for history recording |
| `history-sensors` | uint32 | `0` | Bitmask selecting which sensors to record (0 = all available) |

New `history` shell command:

| Command | Description |
|---|---|
| `history info` | Buffer summary |
| `history count` | Number of stored records |
| `history read [N]` | List records (optionally last N) |
| `history clear` | Erase the buffer |
| `history capture` | Sample sensors and store one record now (bench/test) |
| `history sensors [<name> on\|off]` | List or toggle recorded sensors |
| `history enable on\|off` | Master on/off switch |
| `history stats` | Per-sensor min/max/avg |

Recordable channels (the `history-sensors` bitmask, one bit each): `temperature`, `humidity`, the per-slot 1-Wire channels `s1-temp`/`s1-hum` … `s4-temp`/`s4-hum` (ROM-bound slots, mirror the telemetry slot model; a Dallas slot has no humidity), `hall-left`, `hall-right`, `input-a`, `input-b`, `motion`. The mask is **32-bit** (room for future channels). A Dallas slot's humidity, or any channel whose capability is off, is simply not recorded.

Replay over LoRaWAN with the `req_history` downlink command (§1): the device streams the matching records back as `history_frame` messages on fPort 85. Each frame carries a shared `present` mask + `interval_s` once; samples are fixed-size values-only records, time(j) = `t0_unix + j*interval_s`. The replay splits across as many frames as the data rate needs and terminates when the window is exhausted (a data-rate change mid-replay only changes records-per-frame; the consumer concatenates by `frame_index`).

> **Note on `interval-report`:** the LoRaWAN stack persists frame counters to NVS on every uplink; at the 60 s minimum interval with multi-frame reports the storage partition reaches its ~10 k erase budget in roughly 1–2 years. The default (900 s) is decades. History flash wear is fine even at 60 s.

---

## 7. Alarm rate-limiting & PIR alarm (NEW)

New configuration parameters control alarm uplink frequency and add a PIR motion alarm:

| Shell key | Type | Default | Range | Description |
|---|---|---|---|---|
| `alarm-limit` | int (s) | `0` | 0–3600 | Minimum interval between alarm uplinks (**0 = disabled**). The first alarm goes out immediately; further alarms within the window are suppressed (counters/LED still update). Prevents message floods from a chattering sensor. |
| `alarm-notif-time` | int (s) | `10` | 1–60 | Red-LED hold time for both-mode and pulse (PIR) alarms |
| `pir-notify-act` | bool | `false` | | Raise an alarm on PIR motion (previously PIR was indication-only) |

### Dynamic alarm rules — read / write over LoRaWAN & NFC

Per-sensor alarm thresholds are **dynamic rules** (`app_alarm_rules`, 16 fixed **slots** `0…15`). The **slot index is the rule's stable identity**: `(source, quantity)` is an attribute of the slot, not a key, so **several slots may carry the same `(source, quantity)`** — that is the multi-level case (e.g. a *warning* band and a separate *critical* band on one sensor as two independent rules, each latching and reporting on its own). Clearing a slot empties it without renumbering the others, so a host (and `AlarmEvent.slot`) can refer to a rule by slot reliably. A rule's *kind* follows its quantity: **threshold** (`lo`/`hi`/`hst`) for analog quantities, **state** (`from_state`/`to_state`) for discrete inputs, **rate** (`hi` = max events per report) for counters.

> **Note on "slot":** the alarm rule *slot* (`0…15`, this section) is a distinct concept from the 1-Wire sensor *slot* (`s1…s4`); the shell calls the alarm one the rule **index** (`alarm set <index> …`).

Locally they are managed with the `alarm` shell command: `alarm set <index> <source> <quantity> <args…>` (write a specific slot), `alarm new <source> <quantity> <args…>` (write the first free slot, prints the chosen index), `alarm clear <index>|all`, `alarm list` (prints each rule with its `[index]`; `alarm list <index>` prints just that one slot). Remotely they go through the same transport-agnostic command channel, so **the identical message works on both fPort 85 (LoRaWAN) and NFC**:

- **Write** — `alarm_rule` command (`op`: `0`=SET, `1`=CLEAR, `2`=CLEAR_ALL). SET and CLEAR **require `slot`**; SET also carries `source`, `quantity` and the kind's fields. Reply: `ack`.
- **Read back** — `req_alarm_rules` command; reply: **`alarm_rules_dump`**, paged like `get_config` (`page_index`/`page_count`). An optional `slot` narrows it to one slot; a `source`+`quantity` pair selects every slot on that pair; omit all to list everything. Each `RuleEntry` carries `slot`, `source`, `quantity`, `enabled`, `kind` and only the fields valid for that kind. Read-back over LoRaWAN needs a data rate above DR0 (a rule does not fit a DR0 frame); NFC returns everything in one page.

```jsonc
// SET slot 0: sensor s1, temperature, alarm below 15 °C or above 25 °C, 0.5° hysteresis
{ "command": "alarm_rule", "seq": 1,
  "alarm_rule": { "op": 0, "slot": 0, "source": 1, "quantity": 0, "enabled": true, "lo": 15, "hi": 25, "hst": 0.5 } }
// SET slot 1: a second, critical band on the SAME sensor (multi-level)
{ "command": "alarm_rule", "seq": 2,
  "alarm_rule": { "op": 0, "slot": 1, "source": 1, "quantity": 0, "enabled": true, "lo": 5, "hi": 35, "hst": 0.5 } }
// CLEAR slot 0
{ "command": "alarm_rule", "seq": 3, "alarm_rule": { "op": 1, "slot": 0 } }
// CLEAR_ALL
{ "command": "alarm_rule", "seq": 4, "alarm_rule": { "op": 2 } }
// READ all rules
{ "command": "req_alarm_rules", "seq": 5, "req_alarm_rules": {} }
// READ one slot
{ "command": "req_alarm_rules", "seq": 5, "req_alarm_rules": { "slot": 0 } }
// READ every slot on onboard temperature
{ "command": "req_alarm_rules", "seq": 5, "req_alarm_rules": { "source": 0, "quantity": 0 } }
```

`alarm_rules_dump` reply decoded by `ttn.js` (two rules — a threshold and a state rule, each with its `slot`):

```jsonc
{ "seq": 5, "alarm_rules_dump": { "page_count": 1, "rules": [
  { "slot": 0, "source": 1, "quantity": 0, "enabled": true, "kind": 0, "kind_name": "threshold", "lo": 15, "hi": 25, "hst": 0.5 },
  { "slot": 3, "source": 8, "quantity": 6, "enabled": true, "kind": 1, "kind_name": "state", "from_state": 0, "to_state": 1 }
] } }
```

---

## 8. Payload formatter updates (`ttn.js`)

The TTN/ChirpStack payload formatter (`app/decoder/ttn.js`) was extended for v1.4.0:

- **`decodeUplink`** now decodes the new ports — fPort 2 (protobuf telemetry), fPort 3 (alarm batch), and fPort 85 (command responses: `info`, `ack`, `config_dump`, `history_frame`, `w1_scan`, `alarm_rules_dump`, `error`) — in addition to the legacy fPort 1.
- **`encodeDownlink`** (new) lets you author commands as JSON; the formatter encodes them to bytes on fPort 85.
- **`decodeDownlink`** (new) lets the network server display a queued command.

**Downlink command JSON examples:**
```json
{ "command": "get_info", "seq": 1 }
```
```json
{ "command": "force_send", "seq": 6 }
```
```json
{
  "command": "set_param",
  "seq": 5,
  "set_param": {
    "lorawan": { "adr": true },
    "application": { "interval_report": 120, "temperature_alarm_hi": 50.0 }
  }
}
```
```json
{ "command": "get_param", "seq": 2, "get_param": { "lorawan_field": [3], "application_field": [4, 7] } }
```
```json
{ "command": "reset_counters", "reset_counters": { "hall_left": true, "input_a": true } }
```
```json
{ "command": "req_history", "req_history": { "from_unix": 1780000000, "to_unix": 1780003600 } }
```

`encodeDownlink` returns `{ bytes, fPort: 85, warnings, errors }`.

---

## 9. Shell command summary (v1.4.0 changes)

| Command | Change |
|---|---|
| `clock get` / `set` / `sync` | **New** — real-time clock |
| `history …` | **New** — sensor history buffer |
| `ats …` | **Renamed** from `tester` — diagnostics/test commands |
| `ats lrw reset` | **New** — clear LoRaWAN frame counters + DevNonce (reboots) |
| `ats cmd lrw \| nfc <hex>` | **New** (debug) — inject a command over the LoRaWAN/NFC transport for bench testing |
| `nfc dump` / `read` / `write` / `clear` / `check` / `autocheck` / `reg` / `regw` | **New** (debug) — direct ST25DV tag access; `nfc check` prints a readable trace of what it read, decoded and wrote back (see §10) |
| `config history-enable` / `history-sensors` / `alarm-limit` / `alarm-notif-time` / `pir-notify-act` | **New** parameters (see §6, §7) |

Existing commands (`config`, `settings save`/`reset`, `join`, `send`) are unchanged.

---

## 10. Configuration key naming (sensor-centric)

v1.4.0 organizes configuration keys **by sensor/source** rather than under a global `alarm-` tree. Threshold-alarm keys now use an `<source>-alarm-<param>` form (leaving room for future non-alarm params on the same sensor), and the accelerometer's motion sensitivity moves under the `accel-` sensor. A global `alarm-` prefix is reserved for cross-cutting params only.

| Old key (v1.3.x) | New key (v1.4.0) |
|---|---|
| `alarm-temperature-{enabled,lo,hi,hst}` | `temperature-alarm-{enabled,lo,hi,hst}` |
| `alarm-humidity-*` | `humidity-alarm-*` |
| `alarm-pressure-*` | `pressure-alarm-*` |
| `alarm-t1-temperature-*` | `t1-alarm-*` |
| `alarm-t2-temperature-*` | `t2-alarm-*` |
| `motion-sensitivity` | `accel-motion-sensitivity` |

**Unchanged:** discrete sources (`hall-*`, `input-*`, `pir-notify-act`) and global params (`alarm-limit`, `alarm-notif-time`). `accel-motion-sensitivity` defaults to `off` (no accelerometer detection).

The protobuf field numbers are **unchanged**, so the over-the-air wire format stays compatible; only the user-facing keys and code identifiers change. Devices must be reprovisioned with the new shell keys (the NVS settings keys moved).

---

## 10. Local NFC access (NEW)

The device now uses its **ST25DV NFC tag** as a local, phone-tappable channel — for reading the sticker's identity and for the same command protocol available over LoRaWAN, without a network connection.

**Identity record (always present).** When idle, the firmware keeps a small plaintext **info record** on the tag, so a phone learns the sticker identity and config-schema version the moment it taps — no decryption, no app required. Payload (11 bytes): format version, serial number, firmware version, build type, config-schema version, debug flag. The record is self-healing: it is restored whenever the tag is empty or after a written config/command has been consumed, and it is **not** rewritten while it is already present (no needless EEPROM wear).

**Command/response over NFC.** A phone can drive the **same commands as over LoRaWAN** (`get_info`, `set_param`, `get_config`, `reboot`, …; see §1) by writing a command record to the tag. The firmware processes it with the transport-agnostic command engine and replaces it with a **response record** for the phone to read back. Deferred actions (reboot / save / factory-reset) run *after* the response is written, so the phone always reads the acknowledgement first. Encrypted **config provisioning** over NFC (AES-CCM, serial + monotonic nonce, anti-replay) is also applied through the same path.

**Tag format.** Records are NFC Forum **Type 5**: a 4-byte Capability Container (`E1 40 40 01`) followed by an NDEF message. Each record uses a short **NFC Forum external type** to keep the 512-byte user memory free for payload:

| Record | NDEF type |
|---|---|
| Info | `hio.stck:inf` |
| Config (encrypted) | `hio.stck:cfg` |
| Command (phone → device) | `hio.stck:cmd` |
| Response (device → phone) | `hio.stck:rsp` |

A phone's Web NFC reader sees each as `record.recordType === "hio.stck:…"`.

**Power.** The periodic check is gated on the tag's `IT_STS_Dyn` register — the firmware reads a single byte each cycle and only performs the full read + NDEF parse when RF activity occurred since the last poll, so an untouched tag costs almost nothing.

---

## 11. Footprint & build notes (internal)

Not user-facing, but worth recording: v1.4.0 grew enough that the debug image RAM/flash budgets got tight, so two footprint optimizations landed.

- **mbedtls AES tables in flash** — `CONFIG_MBEDTLS_AES_ROM_TABLES` + `MBEDTLS_AES_FEWER_TABLES`. mbedtls otherwise generates the AES T-tables in RAM at runtime (~8 KB of `.bss`); these keep them `const` in flash. Frees **~8.8 KB RAM** (debug 99 % → 86 %) for ~2 KB flash. AES-CCM (NFC config decrypt) and LoRaWAN crypto are functionally unchanged. The LoRaWAN MAC uses its own (already-flash) soft-SE AES, so only the PSA path is affected.
- **Integer log formatting** — all `%f`/`%g` in `LOG_*`/`shell_print`/`snprintf` were converted to scaled-integer output via the `APP_FP0/1/2/3` helpers in `app_log.h` (e.g. `"%s%d.%02d"`, sign + integer + zero-padded fraction). With no float format specifiers left, the debug build sets `CONFIG_CBPRINTF_FP_SUPPORT=n`, freeing **~3.5 KB debug flash**. Float arithmetic is unchanged; only the printed representation differs (e.g. `21.91`, `-5.50`).

---

## 12. LoRaWAN connection management (NEW)

`app_lrw.c` was refactored to a single, explicitly-defined state machine (`IDLE → JOINING → HEALTHY ⇄ WARNING → RECONNECT → JOINING`, plus `DISABLED`). All state changes go through one `state_transition()` with entry/exit actions, and three timers each have a single purpose (report cadence / link-check timeout / rejoin backoff). This removes a long-standing failure mode where a late link-check answer in RECONNECT could cancel the rejoin and leave the device wedged with TX stopped (the *"TX stops after 4–5 messages"* bug) — validated fixed on hardware over both TTN and ChirpStack.

**New configuration keys** (LoRaWAN link supervision, runtime-tunable):

| Key | Default | Meaning |
|---|---|---|
| `lrw-link-check-interval` | 5 | Request a LinkCheckReq every N-th uplink (0 = disabled). |
| `lrw-link-check-fail-rejoin` | 5 | Link-check failures while degraded before an OTAA rejoin is attempted. |

Behaviour notes:
- **Tolerant supervision** — a single missed LinkCheckAns does not escalate; WARNING needs 3 consecutive failures, RECONNECT then needs `lrw-link-check-fail-rejoin` more. Some networks (e.g. TTN) do not always answer `LinkCheckReq`; the device correctly stays HEALTHY rather than rejoining spuriously.
- **OTAA rejoin** uses exponential backoff (60 s → ×2 → capped 3600 s); **ABP** cannot rejoin and stays in WARNING (it never had a join).
- **Radio-silent mode (#98)** — if the configured **DevEUI is all-zero** (an un-provisioned device), the firmware enters `DISABLED` instead of looping on join requests that can never succeed, saving power. It stays DISABLED until reprovisioned and rebooted.
- Debug builds expose `ats lrw lc ok|fail` to drive the state machine deterministically on the bench (no real RF outage needed).

---

*Applies to firmware v1.4.0. Reflects the v1.4.0 source. For full configuration parameters and base behaviour, see the device datasheet / v1.3.x documentation.*
