# HARDWARIO STICKER — Firmware v1.4.0 — What's New

This document lists **only the changes introduced in firmware v1.4.0** relative to the v1.3.x series — new features, new messages, new commands and new configuration options. Existing v1.3.x behaviour (basic telemetry, LoRaWAN keys, alarm thresholds, counters, corrections, calibration mode) is unchanged unless noted.

---

## Overview of changes

| Area | Change |
|---|---|
| Remote control | **New** bidirectional command protocol over LoRaWAN downlinks (fPort 85) |
| Telemetry | **New** protobuf telemetry on **fPort 2** (legacy fPort 1 bitmap no longer emitted); large reports split across frames; absent analog readings sent as a `null` sentinel |
| Alarms | **New** alarm-detail batch on **fPort 3**; **new** global alarm rate-limit; **new** PIR motion alarm; **new** no-data watchdog; sensor-reading validation hardening |
| Device info | **New** automatic device-info uplink on every join |
| Time | **New** real-time clock, synced from the network |
| History | **New** sensor history store-and-forward with on-request replay |
| Shell | **New** `clock` and `history` commands; test command renamed `tester` → `ats` |
| Payload formatter | `ttn.js` extended to decode fPorts 2/3/85 and to **encode downlink commands** |
| Config keys | Threshold-alarm and motion keys renamed to a **sensor-centric** scheme (see §9) |
| Counters | **New** — hall/input pulse totalizers now persist across reboot & power loss (see §14) |

---

## 1. Remote control over LoRaWAN (NEW)

The device now accepts commands as **LoRaWAN downlinks on fPort 85** and replies on fPort 85. Author commands with the payload formatter (`encodeDownlink`, §8) or paste raw hex.

| Command | Purpose | Reply |
|---|---|---|
| `get_info` | Firmware version, serial, uptime, wall-clock, build type | `info` (no ack) |
| `set_param` | Change any configuration parameters (LoRaWAN/application/sensors/alarms) | `ack` |
| `get_param` | Read back selected parameters (paged) | `config_dump` |
| `get_config` | Dump the whole configuration (paged) | `config_dump` |
| `settings_save` | Persist staged changes (**reboots**) | `ack` |
| `reboot` | Cold reboot | `ack` |
| `factory_reset` | Reset config to defaults but **keep device identity + LoRaWAN keys** (stays provisioned/connected); clears dynamic alarm rules; **reboots** | `ack` |
| `force_send` | Send a telemetry report immediately | **none** — the report itself is the reply |
| `reset_counters` | Clear selected hall/input counters | `ack` |
| `clock_sync` | Sync the wall-clock. **Empty** (LoRaWAN): request a network time sync. **With `unix_time`** (NFC): set the RTC directly from the phone's clock (UTC seconds) | LoRaWAN: **`info`, deferred** — sent once the network time lands. NFC: **`info`** immediately — carries the new `unix_time` |
| `req_history` | Replay stored history for a time window | `history_frame` (multiple) |
| `w1_scan` | Enumerate the 1-Wire bus; returns the discovered ROMs so you can teach a slot via `set_param sensorN_rom` | `w1_scan` |
| `lrw_reset` | Reset the LoRaWAN NVM — frame counters + `DevNonce` + session (same as `ats lrw reset`); **reboots** so the MAC re-initialises clean | `ack` |
| `lrw_join` | Trigger a forced (re)join immediately instead of waiting for the next scheduled attempt; **no reboot** | `ack` |
| `enter_calibration` | Persist `calibration=true` + **reboot** into calibration mode (same end state as `set_param calibration=true` + `settings_save`); the flag is cleared on entry, so the device returns to normal after the calibration window | `ack` |
| `enter_mailbox` | **NFC only.** Switch into mailbox (ST25DV Fast-Transfer-Mode) serving mode for a bounded window — high-throughput config streaming / firmware update. `ack` is written over NDEF first, then the device serves the mailbox until the phone goes quiet (or `timeout_s` elapses) and returns to the low-power NDEF poll. See §10 | `ack` |
| `exit_mailbox` | **NFC only.** End mailbox serving immediately (sent as the last mailbox message when the phone is done streaming) so the device drops straight back to the low-power NDEF poll | `ack` |

> After `set_param`, send `settings_save` to persist (it reboots). If a command fails, the device returns an `error` with a code (1 = BAD_REQUEST, 2 = OUT_OF_RANGE, 3 = NOT_READY, 4 = HISTORY_UNAVAILABLE, 5 = UNSUPPORTED_FIELD, 6 = PERSIST_FAILED), an optional `fault_field`, and a `detail` string. `fault_field` encodes **`group × 100 + tag`** (group 1 = lorawan, 2 = application, 3 = sensors, 4 = alarms; 0 = not group-scoped) so one value identifies the offending field unambiguously across groups — e.g. `203` = application `interval_report`, `102` = lorawan `sub_band`. `ttn.js` splits it back into `fault_group` + `fault_field`.
>
> **No redundant acks:** commands whose real answer is the data they produce (`get_info`, `force_send`, `clock_sync`, `req_history`, `w1_scan`) do **not** also send an `ack`, to save an uplink.
>
> **Per-command transport restrictions:** each command is gated to the transports that make sense for it and rejected elsewhere with `NOT_READY` "transport not allowed". `force_send` and `req_history` are **LoRaWAN-only** (their answer is an uplink) → rejected over NFC. `enter_mailbox` / `exit_mailbox` are **NFC-only** (the mailbox is an NFC/Fast-Transfer-Mode channel) → rejected over a LoRaWAN downlink (previously such NFC-only commands were wrongly accepted over LoRaWAN with a misleading `Ack`).
>
> **Setting the clock over NFC:** `clock_sync` with a `unix_time` field sets the RTC directly from a phone, bootstrapping wall-clock time before/without a network (epoch sanity-bounded to 2024-01-01 … 2100-01-01; out-of-range → `BAD_REQUEST` "bad epoch"). A later network `DeviceTimeReq`/`DeviceTimeAns` stays authoritative and may refine it. Empty `clock_sync` over NFC just confirms (no network to query).
>
> **Commissioning a device with just a phone (NFC):** the LoRaWAN identifiers/keys are written through the encrypted NFC config channel (`set_param`/config ingest covers every `lorawan` field — DevEUI, JoinEUI, AppKey, NwkKey, DevAddr, NwkSKey, AppSKey, region, sub-band, network, activation, ADR, link-check). The typical order is **set params → `lrw_reset` → `lrw_join`**: write the keys, reset the counters/`DevNonce` (so a re-keyed or relocated device starts a clean session), then force the join. `lrw_reset` reboots; the `ack` is written back to the tag first, then the reset + reboot run after a short delay (the phone can read the reply before the device restarts). `lrw_reset` and `lrw_join` work over both NFC and a LoRaWAN downlink.
>
> **`get_param` is paged** like `get_config`: the reply carries `page_index`/`page_count`, and an optional `page` field in the request selects which page (omit = 0). When the selected fields don't all fit one data-rate frame they are split across pages — fetch the rest by re-sending with the next `page`. A page out of range returns `OUT_OF_RANGE`. If any response still doesn't fit the buffer it is replaced by an `error` (same `seq`) rather than dropped silently.

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
| `set_param`: ADR on, `interval_report`=120 s, `cap_barometer` on | `0801120c0a0220011202187822023001` |
| `alarm_rule` SET slot 0 = s1 temperature 15–25 °C, hyst 0.5 | `08016a15100120012d00007041350000c8413d0000003f5000` |
| `alarm_rule` CLEAR slot 0 | `08016a0408015000` |
| `alarm_rule` CLEAR_ALL | `08016a020802` |
| `req_alarm_rules` (all rules, page 0) | `08017a00` |
| `req_alarm_rules` (slot 0 only) | `08017a022000` |
| `req_alarm_rules` (all slots on onboard temperature) | `08017a0408001000` |
| `lrw_reset` | `0801820100` |
| `lrw_join` | `08018a0100` |
| `enter_calibration` | `0801920100` |

(The leading byte is the `seq` you chose; it is echoed in the reply.)

> **Single source of truth (dev note):** the command list, wire ids, dispatch routing and per-command availability live in `app/src/app_config.yml` (`commands:`). `west configen` generates the proto `Command` oneof, the `ttn.js` `_CMD_NAMES` map and the firmware `app_cmd_dispatch()` switch from it, so the three never drift. Adding/removing a command = editing that list (proto ids are append-only and guarded).

---

## 2. New telemetry format on fPort 2 (protobuf)

Periodic and event reports now use a compact, extensible **protobuf** format on **fPort 2**. The legacy bitmap format is **no longer emitted** — the device transmits only the new ports (2 telemetry, 3 alarm, 10 calibration, 85 response/history). `decodeUplink` still understands a legacy fPort 1 frame so historical captures decode, but no current firmware produces one.

Key differences from the v1.3.x bitmap:
- **Extensible** — new sensors can be added in future firmware without breaking older decoders.
- **Capability-gated, whole-group** — a sensor's group is sent in full **every report** whenever its capability is enabled, even when the values are `0`/`false` (e.g. a hall counter at 0, all states inactive). The **system group** (voltage, `boot`) is **always** present, so `boot=false` is reported explicitly rather than by omission. Digital fields carry their real value (0 is valid).
- **Absent readings → `null` sentinel (changed)** — an **enabled analog sensor is now always present on the wire** so the configured-sensor list stays stable across reports; a missing or faulty reading (NaN) is sent as a **sentinel** value the decoder maps to **`null`** instead of dropping the field. This lets the backend tell *"configured but no data right now"* from *"not configured"*. (Previously an absent analog scalar was simply omitted.) The onboard temperature/humidity **and the 1-Wire sensors** follow this rule — a disconnected or faulted DS18B20 / machine-probe reports `temperature: null` rather than dropping its slot from the report.
- **No-data watchdog** — a configured sensor that stops producing samples (reads NaN for ≥ 5 s) raises a no-data alarm (fPort 3) **and** the device blinks the **red LED** every `BLINK_INTERVAL_SECONDS` (3 s) while any sensor is in the no-data state, until it recovers — so a silently dead sensor is surfaced instead of reporting `null` forever.
- **Multi-frame split** — if a report is larger than the current data rate allows, it is split across several fPort-2 frames sent a few seconds apart. Each frame carries whole sensor groups from the **same snapshot**, so the network server can merge them; nothing is lost. The 1-Wire list (below) splits **per reading** — a single frame may carry only some of the slots, the rest follow in the next frame.

The decoded field set (voltage, temperature, humidity, pressure/altitude, illuminance, orientation, motion_count, 1-Wire sensors, hall/input counters and states, `boot`) matches the familiar v1.3.x fields — see the payload formatter output (§8). Decode with the updated `ttn.js`.

### Reading validation & robustness

Readings are range-checked before they reach telemetry, history and alarms, so a glitchy sample no longer fires a false alarm or skews stored data:

- **DS18B20** — values outside −55…125 °C are rejected; the +85.0 °C power-on/brown-out glitch (a valid-CRC sentinel) is **debounced** (a lone spike is suppressed, a sustained real +85 °C still passes after one extra sample). This is the root cause of the earlier spurious low-temperature alarms.
- **Onboard SHT4x** — temperature/humidity outside the sensor's spec window are rejected.
- **Accelerometer** — free-fall detection is armed/torn down together with any-motion (no spurious free-fall while motion detection is off); accel **and orientation** are still read into the periodic sample even when motion detection is off.
- **Alarms** — a minimum threshold hysteresis is enforced (no chattering when `hst` is 0); momentary STATE rules (PIR/accel) reject a `from==to` shape; the 1-Wire bus scan caps the number of recorded ROMs so a noisy bus can't exhaust memory.

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

The frame is prefixed with the same **1-byte format version** (`APP_PROTO_VERSION = 0x01`) as fPort 2 and fPort 85, so every protobuf uplink shares the rule *"byte 0 = version, protobuf starts at byte 1"* (#165). `decodeUplink` strips and validates it. *(fPort 1 legacy bitmap and fPort 10 calibration remain unversioned.)*

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
    uint32 rel_s  = 4;               // seconds since base_time
    optional sint32 value = 5;       // current reading, scaled; absent for discrete sources
    uint32 quantity = 6;             // which quantity (temperature/humidity/.../state/count)
    uint32 slot     = 7;             // alarm rule slot index that fired (stable identity)
    Type   type     = 9;             // TYPE_LOW / TYPE_HIGH / TYPE_TRIGGER / TYPE_NO_DATA
}
```

> **Changed in #212 (BREAKING):** the former `side` (field 3) and `no_data` (field 8) are replaced by a single **`type`** enum (`none`, `low`, `high`, `trigger`, `no_data`). Fields 3 and 8 are now `reserved`. `type` says *what* fired, `edge` says *rising/falling* — they stay orthogonal, so a recovery still names the condition that cleared (e.g. `type=high, event=deactivate`).

Each event is described by **two orthogonal enums** (`type` + `edge`) plus its `source`/`quantity` — an integration that only wants "did `hall-left` trigger?" or "did `temperature` cross its high bound?" reads one field instead of unpacking a combined flag.

### Decoded fields (`ttn.js`)

- `base_time` — Unix timestamp the events are relative to.
- `total` — total alarms that occurred in the window. `truncated` is `true` when `total` exceeds the events actually carried (some were dropped to fit the data rate).
- `alarms[]` — each with:
  - `slot` — the alarm rule slot index that fired (`0` when omitted on the wire); lets a host map the edge back to the exact rule, including which level of a multi-level alarm
  - `source` — `temperature` / `humidity` / `pressure` / `t1-temperature` / `t2-temperature` / `hall-left` / `hall-right` / `pir` / `input-a` / `input-b` / `accel-motion`
  - `event` — `activate` / `deactivate`
  - `type` — **what** fired: `high` / `low` for threshold rules (which bound was crossed), `trigger` for discrete rules (state/count/PIR/accel), `no_data` for the no-data watchdog. The **deactivate** edge keeps the `type` that was raised on activation (e.g. a temperature that fell back inside the band reports `type:"high", event:"deactivate"`).
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
           { total:1, alarms:[{ source:"temperature", event:"activate", type:"high", value:26.5, time:base }] }
```

When the temperature later falls back below the band, the deactivate edge behaves the same way (immediate fPort-2 if the rate-limit has elapsed, then `event:"deactivate", type:"high"` in the next window's fPort-3).

**Example — a hall sensor is activated then deactivated within one window:**

```
t = 0 s    magnet applied   → hall-left activate   → fPort 2 (immediate); window opens
t = 8 s    magnet removed   → hall-left deactivate → batched (fPort-2 suppressed by rate-limit)
t ≈ 20 s   window flushes   → fPort 3 AlarmReport {
             total:2, alarms:[
               { source:"hall-left", event:"activate",   type:"trigger", value:null, time:base+0 },
               { source:"hall-left", event:"deactivate", type:"trigger", value:null, time:base+8 } ] }
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
| `claim_token` | 128-bit device claim token, hex (#170) — **omitted** until the device is commissioned |

The same message is returned on demand by the `get_info` command (over LoRaWAN **and** NFC), so a backend can read the claim token over either channel.

### Claim token (#170)

The **claim token** is a 128-bit device-identity value, on the same footing as the serial number, used by the backend to claim/associate the unit. It is **set once during commissioning** with the shell command **`config claim-token <32-hex>`** (then `settings save`), and is afterwards **immutable** — any further write (shell, `SetParam`, NFC, LoRaWAN) is rejected (`claim-token already set`). It survives `settings reset` and is cleared only by a full NVS erase. Unlike the LoRaWAN/secret keys it is **not** confidential: it is reported in the `Info` message and readable over both NFC and LoRaWAN. Locally, **`ats device info`** prints it (and the secret key) next to the serial number, firmware and uptime.

---

## 5. Real-time clock (NEW)

The device now keeps wall-clock time, synchronised from the network via LoRaWAN `DeviceTimeReq` (requested automatically on join). It timestamps history records and alarm events. A phone can also bootstrap the time over NFC (`clock_sync` with `unix_time`, §1) before/without a network — the network sync stays authoritative once joined.

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

Two configuration parameters control alarm uplink frequency:

| Shell key | Type | Default | Range | Description |
|---|---|---|---|---|
| `alarm-limit` | int (s) | `0` | 0–3600 | Minimum interval between alarm uplinks (**0 = disabled**). The first alarm goes out immediately; further alarms within the window are suppressed (counters/LED still update). Prevents message floods from a chattering sensor. |
| `alarm-notif-time` | int (s) | `10` | 1–60 | Red-LED hold time for pulse/momentary alarms (PIR, accel) and the one-shot re-arm interval |

> **PIR / motion alarms** are set as **dynamic alarm rules** (below), not a config flag: `alarm set <i> pir state 0 1` (or `accel state 0 1`). The old per-source `*-notify-act` flags were removed with the move to dynamic rules.

### Dynamic alarm rules — set / change / delete / deactivate

Per-sensor alarm thresholds are **dynamic rules** in 16 fixed **slots** (`0…15`). The **slot index is the rule's stable identity** — `(source, quantity)` is an attribute of the slot, not a key, so **several slots may share the same `(source, quantity)`** (the multi-level case: e.g. a *warning* band and a separate *critical* band on one sensor as two independent rules). Clearing a slot empties it without renumbering the others, so a host (and `AlarmEvent.slot`) can refer to a rule by slot reliably.

Each slot is a `bytes` **config parameter `alarm_0 … alarm_15`** (proto fields `3…18` in the `alarms` submessage), so rules are written and read like any other configuration — over **SetParam / GetParam** (the *identical* message works on **fPort 85 (LoRaWAN)** and **NFC**) or with the local **`alarm` shell** command. There is no separate alarm command.

> **Note on "slot":** the alarm rule *slot* (`0…15`) is distinct from the 1-Wire sensor *slot* (`s1…s4`); the shell calls the alarm one the rule **index**.

**Sources** (`source`): `0` onboard · `1–4` s1–s4 (1-Wire) · `5` hall-left · `6` hall-right · `7` input-a · `8` input-b · `9` pir · `10` accel.
**Quantities** (`quantity`): `0` temperature · `1` humidity · `2` pressure · `3` illuminance · `4` magnetic-field · `5` tilt · `6` state · `7` count.
**Kind** (follows the quantity):
- **threshold** (analog) — `lo`/`hi`/`hst` band with hysteresis; alarm when the value leaves `[lo, hi]`.
- **state** (digital: tilt + discrete inputs) — `from_state`/`to_state`. **`from != to` = edge** (fires once on that transition: `0→1` rising, `1→0` falling); **`from == to` = level** (active while the line equals `to`: `1→1` active-high, `0→0` active-low). PIR and accel are *momentary* sources (they only pulse), so any state rule there fires a **per-pulse one-shot** that re-arms after `alarm-notif-time` (edge and level behave alike).
- **count / rate** (counters: hall, input) — `hi` = maximum events allowed per report interval. The increase is assessed once per **`interval_report` window** (a tumbling window that re-baselines each report), not on every internal poll, so the rate is counted consistently regardless of the sample cadence.

**Packed slot format** — each `alarm_N` is **17 bytes, little-endian**:

| Offset | Field | Notes |
|---|---|---|
| 0 | flags | bit0 = **present** (slot occupied), bit1 = **enabled** |
| 1 | source | enum above |
| 2 | quantity | enum above |
| 3 | from_state | state kind only |
| 4 | to_state | state kind only |
| 5–8 | lo | float32 (threshold) |
| 9–12 | hi | float32 (threshold; rate limit for count) |
| 13–16 | hst | float32 (threshold hysteresis) |

#### How to …

| Action | `alarm` shell (local, no reboot) | SetParam (LoRaWAN / NFC) |
|---|---|---|
| **Set / change** a slot | `alarm set <i> <source> <quantity> <args>` | write `alarm_<i>` = packed hex, flags = present+enabled (`03…`) |
| **Add** to the first free slot | `alarm new <source> <quantity> <args>` | — |
| **Delete** a slot | `alarm clear <i>` | write `alarm_<i>` = 34 zero hex chars (present=0) |
| **Delete all** | `alarm clear all` | clear each `alarm_<i>` |
| **Deactivate** (keep the rule, stop evaluating) | — *(the shell always enables)* | write `alarm_<i>` packed with flags = present only (`01…`, enabled bit clear) |
| **Read** | `alarm list [<i>]` | `get_param.alarms_field = [3+i]` → `config_dump.alarms.alarm_<i>` |

Shell `<args>` by kind: threshold `<lo> <hi> [hst]` · state `<from> <to>` · count `<N>`.

> The `alarm_<i>` slots are intentionally **not** in the `config` shell tree (no `config alarm-0`); edit them locally with the readable `alarm` command, or over the air with SetParam.

#### Examples — `alarm` shell
```
alarm set 0 onboard temperature 5 30 1   # threshold: alarm < 5 °C or > 30 °C, 1° hysteresis
alarm set 1 input-a state 0 1            # binary edge:  fire when input-a goes 0 → 1
alarm set 2 input-a state 1 1            # binary level: active while input-a = 1
alarm new hall-left count 10             # rate: ≥ 10 pulses per report interval
alarm clear 1                            # delete slot 1
alarm clear all
alarm list                               # [i] <source> <quantity> …  en=<0|1>  active=<0|1>
```

#### Examples — SetParam over LoRaWAN / NFC
Author with `ttn.js` `encodeDownlink`: the formatter takes the packed rule as a 34-char hex string and encodes it as native bytes (the same message is written to the NFC tag). Add `"save": true` to persist across reboot (reboots, like any config save); without it the rule still applies live but is not persisted.
```jsonc
// Set slot 0: onboard temperature 5–30 °C, hysteresis 1 °C, enabled
{ "command": "set_param", "set_param": { "alarms": { "alarm_0": "03000000000000a0400000f0410000803f" } } }
// Deactivate slot 0 — keep the definition, stop evaluating (flags = present only, 01…)
{ "command": "set_param", "set_param": { "alarms": { "alarm_0": "01000000000000a0400000f0410000803f" } } }
// Delete slot 0 (all zeros → present=0)
{ "command": "set_param", "set_param": { "alarms": { "alarm_0": "0000000000000000000000000000000000" } } }
// Multi-level: a second, critical band on the same sensor in slot 1
{ "command": "set_param", "set_param": { "alarms": { "alarm_1": "0300000000000000400000084200008040" } } }
// Read slots 0 and 1
{ "command": "get_param", "get_param": { "alarms_field": [54, 55] } }
```
`get_param` replies with a `config_dump` whose `alarms.alarm_<i>` are the packed hex strings; decode each with the table above. `alarm list` prints the same rules in a readable form.

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
{ "command": "get_param", "seq": 3, "get_param": { "lorawan_field": [5, 6, 9], "page": 1 } }
```
```json
{ "command": "reset_counters", "reset_counters": { "hall_left": true, "input_a": true } }
```
```json
{ "command": "req_history", "req_history": { "from_unix": 1780000000, "to_unix": 1780003600 } }
```

`encodeDownlink` returns `{ bytes, fPort: 85, warnings, errors }`.

**Binary config fields are native `bytes` (v1.4.0).** DevEUI, the LoRaWAN keys, DevAddr and the 1-Wire ROMs travel as native protobuf `bytes` on the wire — raw bytes, half the size of the previous hex-string encoding — which trims fPort-85 config payloads and NFC-tag usage. No change for formatter users: `encodeDownlink` still accepts these as hex strings and `decodeUplink`/`decodeDownlink` still present them as hex. Only clients that build the protobuf directly (e.g. the NFC manager app) must send and read raw bytes.

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
| `config claim-token <32-hex>` | **New** — set the 128-bit claim token once at commissioning; **write-once** (immutable after first set, see §4) |
| `settings reset` | **Changed** — now keeps device identity + LoRaWAN credentials (see §13); restores only application config + alarm rules to defaults |
| `settings erase` | **New** — full NVS wipe incl. identity + LoRaWAN credentials; shell-only, destructive (see §13) |

Existing commands (`config`, `settings save`, `join`, `send`) are unchanged.

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

> **Accelerometer power (#90).** The LIS2DH is now power-managed on demand: it idles in power-down (ODR=0) and is only resumed for an orientation read or while motion detection is armed — saving ~30 µA of continuous idle current when the accelerometer would otherwise run free. **Consequence:** free-fall detection is active only when `accel-motion-sensitivity != off` (it shares the single motion-sensitivity knob); with sensitivity `off` the accelerometer is fully powered down.

This key rename kept the protobuf field numbers; the user-facing keys and code identifiers changed and devices must be reprovisioned with the new shell keys (the NVS settings keys moved).

> **Wire-format note (#166):** the config protobuf field numbers were later **renumbered** to a contiguous `1..N` per submessage (`lorawan`/`application`/`sensors`/`alarms`) — a one-off **breaking** change to the over-the-air `SetParam`/`GetParam`/`ConfigDump` framing for v1.4.0. Any client that builds/parses the config protobuf directly (the NFC manager app, external decoders) must re-sync to the field numbers in `app_config.proto`. The flat NVS settings keys are by name and are unaffected.

> **Access model (dev note):** each parameter in `app_config.yml` declares **who may read and write it** with two transport lists — `readable` and `writable`, each a subset of `{shell, nfc, lrw}` (omitted ⇒ all three). `west configen` derives the internal generator flags from them (shell command vs not, ConfigDump inclusion, NFC-only keys, off-wire identity blobs), so the read/write matrix is the single source of truth and `proto_group` is purely a layout choice (root vs submessage) with no access side-effect. Example: `secret_key` is `readable/writable: [shell]` (local only); `claim_token` is `readable: [shell, nfc, lrw]` + `writable: [shell]` (public, write-once); LoRaWAN keys are `readable: [shell, nfc]` (never dumped over LoRaWAN). This is an authoring-side change only — no wire-format impact.

---

## 10. Local NFC access (NEW)

The device now uses its **ST25DV NFC tag** as a local, phone-tappable channel — for reading the sticker's identity and for the same command protocol available over LoRaWAN, without a network connection.

**Identity record (always present).** When idle, the firmware keeps a small plaintext **info record** on the tag, so a phone learns the sticker identity and config-schema version the moment it taps — no decryption, no app required. Payload (11 bytes): format version, serial number, firmware version, build type, config-schema version, debug flag. The record is self-healing: it is laid down whenever the tag is empty, and it is **not** rewritten while it is already present (no needless EEPROM wear). After a command/response exchange the response record is left on the tag (restoring the info record *immediately* would race the phone reading the reply, #144); instead the info record is **restored once the RF field has been quiet for ~10 s** (the phone has left), so a later tap always reads valid info rather than a stale response (#164).

**Command/response over NFC.** A phone can drive the **same commands as over LoRaWAN** (`get_info`, `set_param`, `get_config`, `reboot`, …; see §1) by writing a command record to the tag. The firmware processes it with the transport-agnostic command engine and replaces it with a **response record** for the phone to read back. Deferred actions (reboot / save / factory-reset) run *after* the response is written, so the phone always reads the acknowledgement first. **Config provisioning** over NFC is applied through the same path.

**Encryption (`CONFIG_APP_NFC_ENCRYPTION`, default on).** Both the **command** (`hio.stck:cmd`) and **config** (`hio.stck:cfg`) records — and the **response** (`hio.stck:rsp`) written back — are **AES-CCM** encrypted with the device `secret_key`. The 8-byte header is `serial (4) + nonce_counter (4)`; the serial must match and the nonce must be strictly greater than the last accepted one (anti-replay), after which it is persisted. The response reuses the request's `nonce_counter`, so the phone (which chose it) can decrypt the reply. Without the key a phone can therefore only read the plaintext **info** record (`hio.stck:inf`) — it can neither run a command nor write config.

> **Validation builds.** `CONFIG_APP_NFC_ENCRYPTION=n` turns the channel to **plaintext** — command/config records are accepted with no key, serial check or nonce, and the response is written back in the clear. This is **bench-only**: such a build logs a loud boot banner (`NFC ENCRYPTION DISABLED - VALIDATION BUILD ONLY`) and must never be shipped. Build it with `west build … -- -DCONFIG_APP_NFC_ENCRYPTION=n`.

**Tag format.** Records are NFC Forum **Type 5**: a 4-byte Capability Container (`E1 40 40 01`) followed by an NDEF message. Each record uses a short **NFC Forum external type** to keep the 512-byte user memory free for payload:

| Record | NDEF type |
|---|---|
| Info | `hio.stck:inf` |
| Config (encrypted) | `hio.stck:cfg` |
| Command (phone → device) | `hio.stck:cmd` |
| Response (device → phone) | `hio.stck:rsp` |

A phone's Web NFC reader sees each as `record.recordType === "hio.stck:…"`.

**Mailbox (fast transfer).** For high-throughput exchanges (config streaming, and later firmware update) the device can switch to the ST25DV **mailbox** (Fast-Transfer-Mode, a dual-port RAM that works while the RF field stays on — no field-off gaps). The phone sends an **`enter_mailbox`** command over the normal NDEF channel; the device acks over NDEF, then serves the mailbox in a single hold until the phone goes quiet or a bounded `timeout_s` elapses, then drops back to the low-power NDEF poll. **`exit_mailbox`** ends serving immediately. A `set_param` written over the mailbox queues its save exactly like the NDEF path. The mailbox carries the **same encrypted command frames** as the NDEF channel — encryption is not bypassed.

**Power.** The poll thread sleeps on the ST25DV **GPO interrupt** (wired to the STM32, EXTI) and only wakes to read the tag when RF activity occurs — so an untouched tag costs almost nothing and the CPU stays asleep when no phone is present. A periodic fallback timer (30 s) is a safety net in case an edge is missed; each wake still does the cheap gated check (one `IT_STS_Dyn` byte; full read + NDEF parse only on RF activity).

**Robustness.** The command/response round-trip hardens the ST25DV write path (RF_WRITE_EN enable timing after present-password, an "unrecognized data" debounce so a poll that catches a half-written record doesn't clobber it, and an always-read poll). The earlier *immediate* auto-restore of the info record over a just-written response was dropped because it raced the phone reading the reply (#144); it is now done **on a ~10 s field-loss debounce** instead — once no RF (GPO) activity has occurred for the debounce window the phone has left, so restoring the info record can no longer clobber an in-flight read (#164).

**Reading the LoRaWAN keys back (NFC only, #162).** So an operator can verify which keys a device holds, `get_config` / `get_param` now return the LoRaWAN crypto keys (`nwkkey`, `appkey`, `nwkskey`, `appskey`) — but **only over NFC**, never over LoRaWAN:

- The NFC command/config channel is AES-CCM encrypted with the device `secret_key`, so a key read-back is only decryptable by a phone that already holds `secret_key` (the provisioning operator). A device with no `secret_key` knowledge is unreachable over NFC beyond the plaintext info record.
- The keys are flagged `dump_nfc_only`: the dump path emits them only when the transport is NFC. A `get_config`/`get_param` arriving as a **LoRaWAN downlink never selects the key tags**, so they can never leave in an uplink — important because the fPort-85 payload is plain protobuf (the LoRaWAN MAC layer would expose the keys to the network server). The DevEUI/JoinEUI/DevAddr identifiers remain readable over both transports as before.
- **`secret_key` itself is never readable** on any transport (it is the master key for the whole NFC channel).

**Provisioning while powered off (boot-staged config).** Because the ST25DV is powered by the phone's RF field, a STICKER can be configured **while it is unpowered** — on the shelf, before first power-on, or with the battery removed. A phone (Manager-App) writes a config record (`hio.stck:cfg`) or a command record (`hio.stck:cmd`, e.g. `set_param … save=true`) into the tag's user EEPROM over RF; the write lands and persists with no MCU power. On the **next boot** the firmware applies it:

- The boot sequence runs a synchronous NFC check (`app_nfc_check()` in `main.c`) **early — before the LED boot carousel and before the LoRaWAN stack starts** — so a staged config (including LoRaWAN keys) takes effect *before the first join attempt*, not one poll cycle later.
- A staged config is decrypted and **nonce-checked** (same anti-replay as the runtime channel: `nonce_counter` must be strictly greater than the last accepted, then it is persisted), applied through the same config-ingest path, persisted (`settings_save`, which reboots), and the staged record is **cleared** (the plaintext info record is restored) so it is not re-applied on the following boot.
- A **stale or replayed** record left on the tag is rejected by the nonce check on every boot; the firmware logs it and **continues booting** (it never bricks into a reboot loop).
- The yellow NFC LED carousel blinks when a staged config is applied, as operator feedback.
- If the device is already running when the tag is written, the same record is picked up by the normal low-power poll instead (on the GPO interrupt, or within the ~30 s fallback).

Notes: the payload budget is the same ~400–450 B as the runtime config channel; `secret_key` is **not** applied via a config record (the ingest ignores it — key provisioning is handled separately); and the phone must supply the correct `nonce_counter` (it is not exposed in the plaintext info record today). HW feasibility — RF read/write with the MCU fully unpowered, and auto-apply on the next boot — is confirmed on hardware (see #147); an end-to-end pass of the **encrypted** boot-staging path is still pending.
---

## Debug auto-suspend / deep sleep (NEW)

The **Debug** build runs with `CONFIG_PM=n` (the CPU never sleeps) so SWD/RTT stay reachable — but that means a debug unit forgotten on the bench drains its battery. To avoid that, the Debug firmware now **auto-suspends after an idle timeout**.

- **Idle timeout** — `CONFIG_APP_DEBUG_AUTOSUSPEND_S` (default **7200 s** = 2 h). After this long with no **RTT/shell interaction**, the device enters deep sleep. Any shell input resets the timer. Set to `0` to disable. Debug-only (no effect on the Release build, which already sleeps via PM). During an active debug session you keep the device awake simply by issuing shell commands; an idle unit (e.g. after the J-Link is unplugged) suspends once the timeout elapses.
- **Deep sleep** = STM32WL **Shutdown** (`sys_poweroff()`): lowest practical quiescent current, all peripherals off. Before powering down it stops the LoRaWAN TX/join timers and the sensor sample timer and turns the LEDs off.
- **Wake** = **NRST / power-cycle** only — a clean boot. NVS identity and LoRaWAN keys live in flash and survive; the RAM state and RTC wall-clock are lost and re-established on boot (time re-syncs from the network). (NFC-field / wakeup-pin wake is a possible future addition, not in this version.)
- **On demand** — a `power suspend` shell command enters deep sleep immediately (bench/test hook). On-demand suspend over LoRaWAN/NFC is not wired yet.

---

## 11. Footprint & build notes (internal)

Not user-facing, but worth recording: v1.4.0 grew enough that the debug image RAM/flash budgets got tight, so several footprint optimizations landed. The debug image budget is hard-capped at `CONFIG_FLASH_LOAD_SIZE=0x3C000` (the `storage_partition` offset) — it already eats the history region and cannot grow past the NVS storage without moving it and breaking release NVS keys, so the only way to free debug space is to cut content.

- **Minimal logging in debug** — the debug build uses `CONFIG_LOG_MODE_MINIMAL=y`: `LOG_*` route straight to `printk` (→ RTT console via `CONFIG_RTT_CONSOLE`) with no deferred log thread, no internal buffer and no backend layer. Frees **~14 KB flash + ~11 KB RAM**. Trade-offs: bare log lines (single-letter level prefix `E:`/`W:`/`I:`, no timestamp), build-time level filtering only (`CONFIG_LOG_MAX_LEVEL` still applies, but the `log` shell command can no longer change levels at runtime), and synchronous printk-style logging (changes timing). The deferred-logging config is kept as a commented block in `debug.conf` with a restore note. This headroom is what lets the debug build keep the full shell help (`CONFIG_SHELL_HELP=y`) alongside the v1.4.0 feature set.
- **mbedtls AES tables in flash** — `CONFIG_MBEDTLS_AES_ROM_TABLES` + `MBEDTLS_AES_FEWER_TABLES`. mbedtls otherwise generates the AES T-tables in RAM at runtime (~8 KB of `.bss`); these keep them `const` in flash. Frees **~8.8 KB RAM** (debug 99 % → 86 %) for ~2 KB flash. AES-CCM (NFC config decrypt) and LoRaWAN crypto are functionally unchanged. The LoRaWAN MAC uses its own (already-flash) soft-SE AES, so only the PSA path is affected.
- **Integer log formatting** — all `%f`/`%g` in `LOG_*`/`shell_print`/`snprintf` were converted to scaled-integer output via the `APP_FP0/1/2/3` helpers in `app_log.h` (e.g. `"%s%d.%02d"`, sign + integer + zero-padded fraction). With no float format specifiers left, the debug build sets `CONFIG_CBPRINTF_FP_SUPPORT=n`, freeing **~3.5 KB debug flash**. Float arithmetic is unchanged; only the printed representation differs (e.g. `21.91`, `-5.50`).

---

## 12. LoRaWAN connection management (NEW)

`app_lrw.c` was refactored to a single, explicitly-defined state machine (`IDLE → JOINING → HEALTHY ⇄ WARNING → RECONNECT → JOINING`, plus `DISABLED`). All state changes go through one `state_transition()` with entry/exit actions, and the link-check-timeout and rejoin-backoff timers each have a single purpose (the report-cadence timer moved out to `app_report`, see §13). This removes a long-standing failure mode where a late link-check answer in RECONNECT could cancel the rejoin and leave the device wedged with TX stopped (the *"TX stops after 4–5 messages"* bug) — validated fixed on hardware over both TTN and ChirpStack.

**New configuration keys** (LoRaWAN link supervision, runtime-tunable):

| Key | Default | Meaning |
|---|---|---|
| `lrw-link-check-interval` | 5 | Request a LinkCheckReq every N-th uplink (0 = disabled). |
| `lrw-link-check-fail-rejoin` | 5 | Link-check failures while degraded before an OTAA rejoin is attempted. |

Behaviour notes:
- **Tolerant supervision** — a single missed LinkCheckAns does not escalate; WARNING needs 3 consecutive failures, RECONNECT then needs `lrw-link-check-fail-rejoin` more. Some networks (e.g. TTN) do not always answer `LinkCheckReq`; the device correctly stays HEALTHY rather than rejoining spuriously.
- **OTAA rejoin** uses exponential backoff (60 s → ×2 → capped 3600 s); **ABP** cannot rejoin and stays in WARNING (it never had a join).
- **Radio-silent mode (#98, #175)** — if the configured **DevEUI is all-zero** (an un-provisioned device), the firmware enters `DISABLED` instead of looping on join requests that can never succeed, saving power. It stays DISABLED until reprovisioned and rebooted. As of #175 the **entire LoRaWAN bring-up is skipped** in this mode: `lorawan_start()` is never called, so the SubGHz radio is never powered and there is **no boot radio burst** at all (previously the radio was started once at boot before the device went DISABLED). `ats lrw status` reports `DISABLED`.
- Debug builds expose `ats lrw lc ok|fail` to drive the state machine deterministically on the bench (no real RF outage needed).
- **Watchdog-liveness wedge recovery (#182)** — the hardware watchdog (IWDG) is no longer fed unconditionally from `main()`; the feed is **gated on a per-work-queue liveness heartbeat**. The TX work queue (`m_work_q`) and the report queue each ping a liveness channel every few seconds; `main()` feeds the IWDG only while both are fresh. If a queue wedges — e.g. `lorawan_send()` blocking forever on the MAC confirm semaphore — its heartbeat goes stale, `main()` withholds the feed, and the SoC **resets (~40 s) and rejoins on a clean boot**, recovering a wedged TX path without a manual power-cycle (HW-validated). The timeout (30 s) sits far above the worst-case legitimate single-send blocking, so a healthy device is never reset. The TX/report work-queue stacks were also grown (4096 / 3072 B) to remove a marginal-overflow that could mimic the same wedge (#187).

---

## 13. Report orchestration split (internal, #126)

Follow-up to §12 with no behaviour change: the report *orchestration* was lifted out of `app_lrw.c` into a new `app_report` module, leaving `app_lrw` as pure LoRaWAN transport.

- **`app_report`** owns *when* to measure & send — the `interval_report` cadence timer, the lazy sample trigger (`interval_sample == 0`) and the per-cycle history capture — and runs on its **own work queue**, so the sensor read and the history flash write no longer execute on the LoRaWAN TX work queue (or the system work queue that also drives the MAC). Each cycle it samples, captures, then calls `app_lrw_send_telemetry()`.
- **`app_lrw`** still owns *how* it reaches the network: it composes the snapshot, splits it into per-DR-budget frames, piggybacks the `LinkCheckReq`, retries on duty-cycle backoff and drains the response/alarm queues. On a link-ready edge (join success / history-replay finish) it kicks `app_report` to resume the cadence.
- History capture now self-skips while a replay is streaming (`app_history` owns that guard), instead of `app_lrw` gating it.

The report cadence is unified with `interval_report` (there is no separate `interval_history`). No configuration, wire-format or shell change. Validated end-to-end on hardware over ChirpStack on both debug and release builds (join → cadence, force_send, LC piggyback, multi-frame split, history capture; release `f_cnt_up` sustained well past the historical 4–5-message stall).

---

## 14. Identity & provisioning preservation (NEW)

A firmware update or a reset must **never** un-provision a field device. The device identity
(`serial-number`, `secret-key`, `nonce-counter`) and the LoRaWAN provisioning (`lrw-deveui`,
`lrw-joineui`, all keys, `lrw-devaddr`, session keys, `region`, `sub-band`, `network`,
`activation`, `adr`) are a **protected set** that survives every reset and migration path
(issue #108). These parameters are flagged `preserve_on_reset` in `app_config.yml`, and `configen`
generates the restore logic from that single source of truth — adding a new protected field is a
one-line YAML change, no hand-maintained C list.

**Reset semantics**

| Path | Reaches | Effect on identity + LoRaWAN credentials |
|---|---|---|
| `settings reset` (shell) | local | **Kept** — only app config + alarm rules go back to defaults |
| `factory_reset` command | LoRaWAN **and** NFC | **Kept** — same as `settings reset`; this is the only reset reachable remotely, so no command can un-provision a device |
| NFC config-tag reset | NFC | **Kept** |
| schema migration (config-version bump) | automatic on boot | **Kept** — a firmware update that bumps the config schema restores the protected set after applying new defaults |
| `settings erase` (shell) | local **only** | **Wiped** — full NVS erase incl. identity; the deliberate "return to blank" escape hatch, never wired to LoRaWAN/NFC |

**Partition-map contract** (256 KB internal flash, single flat image, no bootloader yet):

| Partition | Offset | Size | Contents |
|---|---|---|---|
| `code` | `0x00000` | 160 KB | application image (`CONFIG_FLASH_LOAD_SIZE` link-time budget) |
| `history` | `0x28000` | 80 KB | sensor history buffer (future FUOTA DFU slot) |
| `storage` | `0x3C000` | 16 KB | **NVS — the protected set lives here** |

The contract every update path must honour: **never erase the `storage` region at `0x3C000`.**

- **J-Link / SWD** — a plain `west flash` (no `--erase`) writes only the sectors covered by the
  image, and the image can never overflow into `storage` (the linker fails the build on overflow).
  A full-chip `--erase` / mass-erase **does** wipe `storage` and must not be used on a provisioned
  unit. (Hardware write-protect on `storage` is not an option — NVS must write/erase it at runtime.)
- **NFC firmware update** (future erase-in-place bootloader) and **LoRaWAN FUOTA** (future) must
  erase/program only the image region and skip `storage`.

See manual-test-plan G6 / G6b / G6c for the regression checks (reset keeps identity, erase wipes
it, re-flash preserves it).

---

## 15. Counter persistence across power loss (NEW, #49)

The hall (`hall-left` / `hall-right`) and input (`input-a` / `input-b`) pulse totalizers used to live **only in RAM**, so any reset (watchdog, command, FUOTA) or power loss (battery swap, brownout) silently reset a metering total to zero. They are now **persisted to flash (NVS)** and **restored on boot**.

- **Where:** a single small blob in the `storage` partition via the Zephyr Settings API (`counters/totals`), alongside config and the alarm-rule blob. (Not the `history` partition — a debug image is linked over that region.) No new configuration key; the `reset_counters` command is unchanged.
- **Enable flags grouping:** the per-channel enable toggles (`hall-left-counter` / `hall-right-counter` / `input-a-counter` / `input-b-counter`) are **sensor** configuration and live in the `sensors` SetParam/GetParam/ConfigDump submessage (they were moved out of `alarms`, which now holds only the rate-limit params and the dynamic-rule slots).
- **Save cadence (decoupled from sending and from the LoRaWAN join state):**
  - When `interval_sample > 0`, the totals are written on **every sensor sample** (`interval_sample` cadence). This timer is armed at boot, so backups run even on a device that **never joins** (no gateway in range) — and the backup interval can be far tighter than the report/send interval (e.g. back up every 5 min while sending once an hour).
  - When `interval_sample == 0` (sensors are read inside the report cycle), the totals are written **once per `interval_report` cycle**.
  - Both paths write only when a value actually changed since the last save (dirty flag — an idle channel writes nothing). A `reset_counters` command flushes the cleared value immediately, so a reboot cannot resurrect it.
- **Guarantee / trade-off:** the worst-case lost-pulse window is **one `interval_sample`** when sampling is enabled, otherwise **one `interval_report`** — pulses counted between the last periodic save and an abrupt power loss are lost. A tighter interval narrows the window at the cost of more flash wear. Saving runs on the sensor / report work queue (not the system work queue), so it never delays LoRaWAN RX-window timing.
- **Flash wear:** the dirty-flagged, per-report write costs at most one tiny NVS record per cycle. Worst case (continuous counting at the 900 s default) is well within the storage partition's ~10 k erase budget for the life of the device; an idle counter costs nothing. A future brownout/PVD flush (to capture the last window's pulses on supply collapse) is possible but needs HW hold-up verification and is **not** in this version.
- **Width:** counters stay **`uint32_t`** (max 4 294 967 295, then wraps), matching the telemetry/history encoding.

---

*Applies to firmware v1.4.0. Reflects the v1.4.0 source. For full configuration parameters and base behaviour, see the device datasheet / v1.3.x documentation.*
