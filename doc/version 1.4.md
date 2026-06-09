# HARDWARIO STICKER — Firmware v1.4.0 — What's New

This document lists **only the changes introduced in firmware v1.4.0** relative to the v1.3.x series — new features, new messages, new commands and new configuration options. Existing v1.3.x behaviour (basic telemetry, LoRaWAN keys, alarm thresholds, counters, corrections, calibration mode) is unchanged unless noted.

---

## Overview of changes

| Area | Change |
|---|---|
| Remote control | **New** bidirectional command protocol over LoRaWAN downlinks (fPort 85) |
| Telemetry | **New** protobuf telemetry on **fPort 2** (legacy bitmap stays on fPort 1); large reports split across frames |
| Alarms | **New** alarm-detail batch on **fPort 3**; **new** global alarm rate-limit; **new** PIR motion alarm |
| Device info | **New** automatic device-info uplink on every join |
| Time | **New** real-time clock, synced from the network |
| History | **New** sensor history store-and-forward with on-request replay |
| Shell | **New** `clock` and `history` commands; test command renamed `tester` → `ats` |
| Payload formatter | `ttn.js` extended to decode fPorts 2/3/85 and to **encode downlink commands** |

---

## 1. Remote control over LoRaWAN (NEW)

The device now accepts commands as **LoRaWAN downlinks on fPort 85** and replies on fPort 85. Author commands with the payload formatter (`encodeDownlink`, §8) or paste raw hex.

| Command | Purpose | Reply |
|---|---|---|
| `get_info` | Firmware version, serial, uptime, wall-clock, build type | `info` |
| `set_param` | Change LoRaWAN and/or Application parameters | `ack` |
| `get_param` | Read back selected parameters | `config_dump` |
| `get_config` | Dump the whole configuration (paged) | `config_dump` |
| `settings_save` | Persist staged changes (**reboots**) | `ack` |
| `reboot` | Cold reboot | `ack` |
| `factory_reset` | Reset to defaults (**reboots**) | `ack` |
| `force_send` | Send a telemetry report immediately | `ack` |
| `reset_counters` | Clear selected hall/input counters | `ack` |
| `clock_sync` | Request a network time sync | `ack` |
| `req_history` | Replay stored history for a time window | `history_frame` (multiple) |

> After `set_param`, send `settings_save` to persist (it reboots). If a command fails, the device returns an `error` with a code (1 = BAD_REQUEST, 2 = OUT_OF_RANGE, 3 = NOT_READY, 4 = HISTORY_UNAVAILABLE, 5 = UNSUPPORTED_FIELD, 6 = PERSIST_FAILED), an optional `fault_field`, and a `detail` string.

**Ready-to-use hex downlinks (fPort 85):**

| Command | Hex |
|---|---|
| `get_info` | `08032200` |
| `settings_save` | `08043200` |
| `clock_sync` | `08056200` |
| `force_send` | `08064a00` |
| `reboot` | `08083a00` |
| `reset_counters` (hall-left + input-a) | `0807520408011801` |
| `set_param`: ADR on, `interval_report`=120 s, `temperature_alarm_hi`=50 °C | `0801120d0a021801120720783d00004842` |

(The leading byte is the `seq` you chose; it is echoed in the reply.)

---

## 2. New telemetry format on fPort 2 (protobuf)

Periodic and event reports now use a compact, extensible **protobuf** format on **fPort 2**. The previous bitmap format is **still emitted on fPort 1** for backward compatibility with existing integrations.

Key differences from the v1.3.x bitmap:
- **Extensible** — new sensors can be added in future firmware without breaking older decoders.
- **Capability-gated, whole-group** — a sensor's group is sent in full **every report** whenever its capability is enabled, even when the values are `0`/`false` (e.g. a hall counter at 0, all states inactive). The **system group** (voltage, `boot`) is **always** present, so `boot=false` is reported explicitly rather than by omission. Digital fields carry their real value (0 is valid); an analog scalar is omitted only when there is no valid sample yet.
- **Multi-frame split** — if a report is larger than the current data rate allows, it is split across several fPort-2 frames sent a few seconds apart. Each frame carries whole sensor groups from the **same snapshot**, so the network server can merge them; nothing is lost.

The decoded field set (voltage, temperature, humidity, pressure/altitude, illuminance, orientation, motion_count, external temperatures, machine-probe values, hall/input counters and states, `boot`) matches the familiar v1.3.x fields — see the payload formatter output (§8). Decode with the updated `ttn.js`.

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
}
```

Each event is described by **three orthogonal enums** — an integration that only wants "did `hall-left` activate?" or "did `temperature` cross its high bound?" reads one field instead of unpacking a combined flag.

### Decoded fields (`ttn.js`)

- `base_time` — Unix timestamp the events are relative to.
- `total` — total alarms that occurred in the window. `truncated` is `true` when `total` exceeds the events actually carried (some were dropped to fit the data rate).
- `alarms[]` — each with:
  - `source` — `temperature` / `humidity` / `pressure` / `t1-temperature` / `t2-temperature` / `hall-left` / `hall-right` / `pir` / `input-a` / `input-b`
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
| `history sensors [<name> on\|off]` | List or toggle recorded sensors |
| `history enable on\|off` | Master on/off switch |
| `history stats` | Per-sensor min/max/avg |

Replay over LoRaWAN with the `req_history` downlink command (§1): the device streams the matching records back as `history_frame` messages on fPort 85.

---

## 7. Alarm rate-limiting & PIR alarm (NEW)

New configuration parameters control alarm uplink frequency and add a PIR motion alarm:

| Shell key | Type | Default | Range | Description |
|---|---|---|---|---|
| `alarm-limit` | int (s) | `0` | 0–3600 | Minimum interval between alarm uplinks (**0 = disabled**). The first alarm goes out immediately; further alarms within the window are suppressed (counters/LED still update). Prevents message floods from a chattering sensor. |
| `alarm-notif-time` | int (s) | `10` | 1–60 | Red-LED hold time for both-mode and pulse (PIR) alarms |
| `pir-notify-act` | bool | `false` | | Raise an alarm on PIR motion (previously PIR was indication-only) |

---

## 8. Payload formatter updates (`ttn.js`)

The TTN/ChirpStack payload formatter (`app/decoder/ttn.js`) was extended for v1.4.0:

- **`decodeUplink`** now decodes the new ports — fPort 2 (protobuf telemetry), fPort 3 (alarm batch), and fPort 85 (command responses: `info`, `ack`, `config_dump`, `history_frame`, `error`) — in addition to the legacy fPort 1.
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
| `config history-enable` / `history-sensors` / `alarm-limit` / `alarm-notif-time` / `pir-notify-act` | **New** parameters (see §6, §7) |

Existing commands (`config`, `settings save`/`reset`, `join`, `send`) are unchanged.

---

*Applies to firmware v1.4.0. Reflects the v1.4.0 source. For full configuration parameters and base behaviour, see the device datasheet / v1.3.x documentation.*
