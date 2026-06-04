# HARDWARIO STICKER — Firmware v1.4.0 User Guide

This guide describes how to configure the STICKER, what data it sends over LoRaWAN, and how to control it remotely. It is written for integrators and operators (not firmware developers).

---

## Contents

1. [What's new in v1.4.0](#1-whats-new-in-v140)
2. [Configuring the device (serial shell)](#2-configuring-the-device-serial-shell)
3. [Configuration parameters](#3-configuration-parameters)
4. [Shell command reference](#4-shell-command-reference)
5. [Uplink messages (device → network)](#5-uplink-messages-device--network)
6. [Downlink commands (network → device)](#6-downlink-commands-network--device)
7. [TTN / ChirpStack payload formatter](#7-ttn--chirpstack-payload-formatter)
8. [Calibration mode](#8-calibration-mode)

---

## 1. What's new in v1.4.0

| Feature | Summary |
|---|---|
| **Bidirectional command protocol** | Configure and control the device over LoRaWAN downlinks on **fPort 85** (set/get parameters, get info, get config, reboot, factory reset, force send, reset counters, clock sync, request history). |
| **Protobuf telemetry (fPort 2)** | New compact, extensible telemetry format. The legacy bitmap format stays on **fPort 1** for backward compatibility. Large reports are split losslessly across several frames. |
| **Device info on join** | After every join the device announces its firmware version, serial number, uptime and wall-clock time on **fPort 85** — before the first telemetry. |
| **Wall-clock time (RTC)** | The device keeps real time, synchronised from the network (LoRaWAN `DeviceTimeReq`). Used to timestamp history and alarms. |
| **Sensor history (store-and-forward)** | Optionally records sensor readings to flash and replays a chosen time window on request — survives power loss. |
| **Alarm rate-limiting + PIR** | A global minimum interval between alarm uplinks prevents message floods from a chattering sensor; PIR motion can now raise alarms. |
| **Alarm-detail batch (fPort 3)** | Alarm events (activation/deactivation, source, threshold, value, timestamp) are reported as a batch. |

---

## 2. Configuring the device (serial shell)

The device exposes a command shell over its debug interface. You read and change settings there.

- **Read** a value: type the parameter name with no value — e.g. `config interval-report`
- **Set** a value: type the parameter name followed by the value — e.g. `config interval-report 600`
- **Show everything:** `config show`
- **Save changes:** `settings save` — **persists all changes and reboots.** Changes made with `config …` are kept in RAM only until you save.
- **Restore defaults:** `settings reset` — **resets everything to factory defaults and reboots.**

> ⚠️ Settings changed with `config …` are **not** persistent until you run `settings save`. Both `settings save` and `settings reset` reboot the device.

**Example — set a 10-minute report interval and a temperature alarm:**
```
config interval-report 600
config alarm-temperature-enabled true
config alarm-temperature-hi 28.0
config alarm-temperature-lo 5.0
settings save
```

The same parameters can also be set remotely over LoRaWAN (see [Downlink commands](#6-downlink-commands-network--device)) or written over NFC.

---

## 3. Configuration parameters

Use the **shell key** with the `config` command. To set over LoRaWAN, use the matching field in a `set_param` downlink (see §6).

### LoRaWAN

| Shell key | Type | Default | Values / range | Description |
|---|---|---|---|---|
| `lrw-region` | enum | — | `eu868`, `us915`, `au915` | Radio region |
| `lrw-sub-band` | int | 2 | 0–8 (0 = all) | US915/AU915 sub-band (2 matches TTN/Helium/ChirpStack) |
| `lrw-network` | enum | `public` | `public`, `private` | Network sync word |
| `lrw-adr` | bool | — | `true`/`false` | Adaptive Data Rate |
| `lrw-activation` | enum | — | `otaa`, `abp` | Activation method |
| `lrw-deveui` | hex | — | 16 hex digits | DevEUI |
| `lrw-joineui` | hex | — | 16 hex digits | JoinEUI (OTAA) |
| `lrw-nwkkey` | hex | — | 32 hex digits | NwkKey (OTAA) |
| `lrw-appkey` | hex | — | 32 hex digits | AppKey (OTAA) |
| `lrw-devaddr` | hex | — | 8 hex digits | DevAddr (ABP) |
| `lrw-nwkskey` | hex | — | 32 hex digits | NwkSKey (ABP) |
| `lrw-appskey` | hex | — | 32 hex digits | AppSKey (ABP) |

### Reporting

| Shell key | Type | Default | Range | Description |
|---|---|---|---|---|
| `interval-report` | int (s) | 900 | 60–86400 | How often a telemetry report is sent |
| `interval-sample` | int (s) | — | 5–3600 (0 = sample just before report) | How often sensors are sampled (for alarm detection) |

### Alarm thresholds (analog sensors)

Each analog source has four parameters: `…-enabled`, `…-lo`, `…-hi`, `…-hst` (hysteresis). An alarm fires when the value crosses outside `[lo, hi]` (with hysteresis) and clears when it returns inside.

| Source prefix | Defaults (lo / hi / hst) | Range |
|---|---|---|
| `alarm-temperature-*` | 15.0 / 25.0 / 0.5 °C | −30…70 °C |
| `alarm-humidity-*` | 30.0 / 75.0 / 5.0 % | 0…100 % |
| `alarm-pressure-*` | 700.0 / 1060.0 / 10.0 hPa | 500…1200 hPa |
| `alarm-t1-temperature-*` | 15.0 / 25.0 / 0.5 °C | −30…70 °C |
| `alarm-t2-temperature-*` | 15.0 / 25.0 / 0.5 °C | −30…70 °C |

Example: `config alarm-humidity-enabled true`, `config alarm-humidity-hi 80`.

### Alarm behaviour (discrete sources + rate-limit)

| Shell key | Type | Default | Range | Description |
|---|---|---|---|---|
| `alarm-limit` | int (s) | 0 | 0–3600 | Minimum interval between alarm uplinks (**0 = disabled**). The first alarm goes out immediately; further alarms within the window are suppressed (counters/LED still update). |
| `alarm-notif-time` | int (s) | 10 | 1–60 | Red-LED hold time for both-mode and pulse (PIR) alarms |
| `pir-notify-act` | bool | false | `true`/`false` | Raise an alarm on PIR motion |
| `hall-left-notify-act` / `…-deact` | bool | — | | Notify when hall-left activates / deactivates |
| `hall-right-notify-act` / `…-deact` | bool | — | | Notify when hall-right activates / deactivates |
| `input-a-notify-act` / `…-deact` | bool | — | | Notify when input A activates / deactivates |
| `input-b-notify-act` / `…-deact` | bool | — | | Notify when input B activates / deactivates |

### Counters

| Shell key | Type | Description |
|---|---|---|
| `hall-left-counter`, `hall-right-counter` | bool | Count hall-switch events |
| `input-a-counter`, `input-b-counter` | bool | Count input events |

### Corrections

| Shell key | Type | Range | Description |
|---|---|---|---|
| `corr-temperature` | float | −5.0…+5.0 °C | Internal temperature offset |
| `corr-t1-temperature` | float | −5.0…+5.0 °C | External T1 offset |
| `corr-t2-temperature` | float | −5.0…+5.0 °C | External T2 offset |

### Capabilities

These flags declare which peripherals are fitted; a sensor's data is only sampled/reported when its capability is on.

`cap-hall-left`, `cap-hall-right`, `cap-input-a`, `cap-input-b`, `cap-light-sensor`, `cap-barometer`, `cap-pir-detector`, `cap-1w-thermometer`, `cap-1w-machine-probe` — all `bool`.

### Sensor history

| Shell key | Type | Default | Description |
|---|---|---|---|
| `history-enable` | bool | false | Master switch for store-and-forward history |
| `history-sensors` | uint32 | 0 | Bitmask selecting which sensors to record (0 = all available) |

### Device identity

| Shell key | Type | Description |
|---|---|---|
| `serial-number` | uint32 | Device serial number (10 decimal digits) |
| `secret-key` | hex (32) | Device secret key |
| `nonce-counter` | uint32 | Join nonce counter |
| `calibration` | bool | Enter calibration mode on next boot (see §8) |

---

## 4. Shell command reference

### `config` — read/set configuration
`config show` · `config <key>` (read) · `config <key> <value>` (set). See §3 for all keys.

### `settings` — persistence
| Command | Description |
|---|---|
| `settings save` | Save all settings and **reboot** |
| `settings reset` | Reset to factory defaults and **reboot** |

### `clock` — real-time clock
| Command | Description |
|---|---|
| `clock get` | Read RTC time (unix timestamp) |
| `clock set <unix>` | Set RTC time manually |
| `clock sync` | Request network time (LoRaWAN `DeviceTimeReq`) |

### `history` — sensor history buffer
| Command | Description |
|---|---|
| `history info` | Buffer summary |
| `history count` | Number of stored records |
| `history read [N]` | List records (optionally last N) |
| `history clear` | Erase the buffer |
| `history sensors [<name> on\|off]` | List or toggle recorded sensors |
| `history enable on\|off` | Master on/off switch |
| `history stats` | Per-sensor min/max/avg |

### `ats` — automated test system (service/diagnostics)
| Command | Description |
|---|---|
| `ats led cycle [count]` | Cycle LED R/Y/G/off (`count`: default 1, 0 = stop, 1–99 cycles) |
| `ats led switch <red\|yellow\|green> <on\|off>` | Force one LED channel |
| `ats sensors sample` | Print all sensor values |
| `ats sensors reset` | Reset sensor counters |
| `ats sensors serial` | Print sensor serial numbers |
| `ats sensors check <sensor> [timeout]` | Watch a sensor for changes |
| `ats lrw status` | Print LoRaWAN status |
| `ats lrw check` | Send data with a link-check request |
| `ats lrw reset` | Clear LoRaWAN frame counters + DevNonce (**reboots**) |
| `ats cmd lrw <hex>` | (debug) Inject a command as if received over LoRaWAN |
| `ats cmd nfc <hex>` | (debug) Inject a command as if received over NFC |

### Direct LoRaWAN
| Command | Description |
|---|---|
| `join` | Join the LoRaWAN network |
| `send` | Send a telemetry report now |

---

## 5. Uplink messages (device → network)

The device uses several LoRaWAN **fPorts**, each with a distinct payload:

| fPort | Contents |
|---|---|
| **1** | Legacy bitmap telemetry (backward compatibility with older firmware) |
| **2** | **Telemetry** (protobuf) — the current periodic/event report |
| **3** | **Alarm-detail batch** — list of alarm events |
| **10** | Calibration payload (only in calibration mode, §8) |
| **85** | **Command responses** + device info on join + history replay frames |

Decode all of these with the supplied payload formatter (§7), which converts them into readable JSON.

### fPort 2 — Telemetry

A report carries only the sensors that are fitted (capability on) and have valid data. The payload formatter outputs these fields:

| Field | Meaning |
|---|---|
| `boot` | `true` on the first report after start-up |
| `voltage` | Supply voltage (V) |
| `temperature`, `humidity` | Internal SHT4x temperature (°C) / humidity (%RH) |
| `pressure`, `altitude` | Barometer (hPa / m) — if fitted |
| `illuminance` | Light sensor (lux) — if fitted |
| `orientation` | Device orientation 1–6 |
| `motion_count` | Cumulative PIR motion events — if fitted |
| `ext_temperature_1`, `ext_temperature_2` | External 1-Wire thermometers (°C) |
| `machine_probe_temperature_1/2`, `machine_probe_humidity_1/2` | Machine probe(s) |
| `machine_probe_tilt_alert_1/2` | Tilt alert flag(s) |
| `hall_left_count`, `hall_right_count` | Hall event counters |
| `hall_*_notify_act/deact`, `hall_*_is_active` | Hall notify config + current state |
| `input_a_count`, `input_b_count` | Input event counters |
| `input_*_notify_act/deact`, `input_*_is_active` | Input notify config + current state |

**Multi-frame split:** if a report is larger than the current data rate allows, it is split into several fPort-2 frames sent a few seconds apart. Each frame carries whole sensor groups from the **same snapshot**, so the network server can merge them. No data is lost.

### fPort 3 — Alarm-detail batch

Sent when alarms occur. Decoded fields:

| Field | Meaning |
|---|---|
| `total` | Total alarms in the window (may exceed the records present if rate-limited) |
| `truncated` | `true` if `total` > number of records carried |
| `base_time` | Unix timestamp the records are relative to |
| `alarms[]` | List of events |

Each entry in `alarms[]`:
- `source` — `temperature`, `humidity`, `pressure`, `t1-temperature`, `t2-temperature`, `hall-left`, `hall-right`, `pir`, `input-a`, `input-b`
- `event` — `activate` / `deactivate`
- `side` — `lo` / `hi` / `na` (threshold side crossed; `na` for discrete sources)
- `threshold`, `value`, `hysteresis` — sensor units (`null` for discrete sources)
- `time` — Unix timestamp of the event

### fPort 85 — Device info on join

After each join the device sends a `Response.info` (see §6 for fields) so the network knows its firmware and identity before any telemetry.

---

## 6. Downlink commands (network → device)

Send commands as **LoRaWAN downlinks on fPort 85**. The easiest way is to use the payload formatter's `encodeDownlink` (§7): you queue a JSON command and the formatter turns it into bytes. You can also paste raw hex.

> After changing parameters with `set_param`, send `settings_save` to persist them (it also reboots).

### Command list

| Command | Purpose | Response |
|---|---|---|
| `get_info` | Read firmware version, serial, uptime, wall-clock, build type | `info` |
| `set_param` | Change LoRaWAN and/or Application parameters | `ack` |
| `get_param` | Read back selected parameters (by field) | `config_dump` |
| `get_config` | Dump the full configuration (paged) | `config_dump` |
| `settings_save` | Persist staged changes (**reboots**) | `ack` |
| `reboot` | Cold reboot | `ack` |
| `factory_reset` | Reset to defaults (**reboots**) | `ack` |
| `force_send` | Send a telemetry report immediately | `ack` |
| `reset_counters` | Clear selected hall/input counters | `ack` |
| `clock_sync` | Request a network time sync | `ack` |
| `req_history` | Replay stored history for a `[from_unix, to_unix]` window | `history_frame` (multiple) |

### `info` response fields

| Field | Meaning |
|---|---|
| `fw_version` (`fw_major`/`minor`/`patch`) | Firmware version |
| `build_type_name` | `main` (release), `dev` (CI), `custom` (local) |
| `debug` | `true` for debug builds |
| `serial_number` | Device serial |
| `uptime_s` | Seconds since boot |
| `unix_time` | Wall-clock UTC (0 until the RTC is synced) |

### Error response

If a command fails, the device replies with an `error`: `code` (1 = BAD_REQUEST, 2 = OUT_OF_RANGE, 3 = NOT_READY, 4 = HISTORY_UNAVAILABLE, 5 = UNSUPPORTED_FIELD, 6 = PERSIST_FAILED), an optional `fault_field`, and a `detail` string.

### Ready-to-use hex downlinks (fPort 85)

Paste these as raw payload in the TTN/ChirpStack downlink scheduler:

| Command | Hex |
|---|---|
| `get_info` | `08032200` |
| `settings_save` | `08043200` |
| `clock_sync` | `08056200` |
| `force_send` | `08064a00` |
| `reboot` | `08083a00` |
| `reset_counters` (hall-left + input-a) | `0807520408011801` |
| `set_param` ADR on, `interval_report`=120 s, `alarm_temperature_hi`=50 °C | `0801120d0a021801120720783d00004842` |

(These come from the codec test vectors; the leading byte is the `seq` you chose.)

---

## 7. TTN / ChirpStack payload formatter

Paste `app/decoder/ttn.js` into your network server as the device's payload formatter (it implements `decodeUplink`, `encodeDownlink`, and `decodeDownlink`). It handles all uplink ports (1, 2, 3, 85) and lets you author downlink commands as JSON.

### Decoding uplinks
`decodeUplink` returns a JSON object whose fields depend on the fPort — see §5 for the field lists (telemetry, alarm batch, info, history). It automatically routes by `fPort`.

### Sending a downlink command
Provide a JSON object under `data`; the formatter encodes it to bytes on fPort 85.

**Examples:**
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
    "application": { "interval_report": 120, "alarm_temperature_hi": 50.0 }
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

`encodeDownlink` returns `{ bytes, fPort: 85, warnings, errors }`. An unknown command name produces an error. `decodeDownlink` reverses the process so the network server can display a queued command.

---

## 8. Calibration mode

Calibration mode is a factory/service mode that streams raw sensor values for reference measurement.

**Entering:** trigger both hall sensors with magnets simultaneously (the device reboots into calibration), or set `config calibration true` followed by `settings save`.

**Behaviour:**
- Transmits a **24-byte payload on fPort 10** every **30 seconds** (unconfirmed).
- Uses fixed factory ABP credentials shared by all devices (so a calibration gateway can receive any unit without provisioning).
- The payload carries: serial number, uptime, internal temperature/humidity (SHT40), external temperatures (DS18B20 T1/T2), and machine-probe temperatures/humidities — each as a little-endian `int16` in 0.01-unit steps (`0x7FFF` when a sensor is absent).

**Exiting:** power-cycle / reboot the device out of calibration (clear the flag, or boot without the magnets).

---

*Document generated for firmware v1.4.0. Command and parameter details reflect the v1.4.0 source (`app/src/app_config.yml`, `app_config.proto`, `app_cmd.c`, `app_compose.c`, `app_lrw.c`, `app_calibration.c`, `app/decoder/ttn.js`).*
