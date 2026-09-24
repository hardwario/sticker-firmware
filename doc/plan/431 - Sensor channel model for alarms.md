# 431 — Sensor channel model for alarms

Issue: #430 · PR: #431 · Target: the next **non-migratable** release. The config layout
and the alarm, telemetry and history wire formats break, and devices are re-provisioned.
There is no config migration. The release is not decided yet.

Registry draft: [`app/src/app_w1_slots.yaml`](../../app/src/app_w1_slots.yaml).

## Why

An alarm rule targets `(source, quantity)`, and `quantity` is one global enum
(`enum app_alarm_quantity`: temperature, humidity, pressure, illuminance,
magnetic-field, tilt, state, count, voltage). This has two limits:

1. **One value per quantity per source.**
   - The machine-probe has a TMP112 next to the SHT (`app_machine_probe_read_thermometer()`),
     but `struct app_w1_slot_reading` has a single `temperature`, so the TMP112 is never read.
   - The on-board MPL3115A2 temperature is unused for the same reason.
2. **Every new sensor kind touches global code.** An air-quality module (CO2 / VOC /
   PM2.5) would need new enum values plus edits to:
   - `app_alarm_rule_valid()`,
   - `alarm_scale()`,
   - `read_threshold_value()`,
   - `_ALARM_QUANTITIES` in `ttn.js`,
   - the history descriptor table,
   - the Manager-App.

The fix is to make a sensor type describe its own channels. Rules, telemetry, history,
the decoder and the Manager-App then address a value as `(source, channel)`.

## Model

```
source ──► sensor type ──► channel table (app_w1_slots.yaml)
                            ch → {name, label, quantity, unit, kind, wire, history, range, liveness}
rule            = (source, channel, lo/hi/dwell | from/to)
history channel = (source, channel)
```

- **Source:** the physical place a sensor sits. The current `enum app_alarm_source` is
  kept (onboard, s1..s4, hall-left/right, input-a/b, pir, accel, battery).
- **Sensor type:** what sits at the source.
  - A built-in source has a fixed type (onboard → `motherboard`, input-a/b → `input`, …).
  - A 1-Wire slot has a **configured** type (`sensorN_type`, new).
- **Channel:** a type-local number, `0..9` (D6: at most **10 channels per type**). A
  channel means the same thing on every device with that type. The numbering is
  append-only: a channel is never renumbered or reused, and a removed one stays as
  `retired: true`.
- **Quantity** (temperature, humidity, …) is only a *property of a channel*, used for UI
  grouping and icons. It no longer identifies anything.

### Sensor types (see the YAML for full descriptors)

| id | type | ch |
|---|---|---|
| 1 | `dallas` (family 0x28) | 0 temperature |
| 2 | `machine-probe` (family 0x19) | 0 temperature (SHT), 1 humidity (SHT), 2 temperature-aux (TMP112), 3 illuminance, 4 magnetic-field, 5 tilt, 6..8 accel-x/y/z |
| 64 | `motherboard` | 0 temperature (SHT4x), 1 humidity, 2 pressure, 3 illuminance, 4 temperature-baro (MPL3115A2) |
| 65 | `hall` | 0 state, 1 count |
| 66 | `input` | 0 state, 1 count, 2 voltage (#396) |
| 67 | `pir` | 0 motion (momentary), 1 count |
| 68 | `accel` | 0 motion (momentary), 1 count, 2 orientation |
| 69 | `battery` | 0 voltage (low-battery watchdog only) |

- 1-Wire ids 1..63 keep the legacy `app_w1_slot_type` values.
- Built-in ids start at 64 (D1).

**One wire scale per channel.** A channel has a single `wire.scale`, shared by
telemetry and `AlarmEvent.value`. Today the two differ for:

- humidity: telemetry ×2, alarm ×100 → **×2** everywhere,
- voltage: telemetry V×50, alarm ×100, #396 mV → **mV (×1000)** everywhere.

History keeps its own fixed-width encoding per channel (`history.enc` + `history.scale`).

## Registry: `app_w1_slots.yaml`, generated everywhere

The channel tables are written once, in **`app/src/app_w1_slots.yaml`**. That file is the
only place they are edited.

A new west command (`west sensorgen`, alongside `configen`) generates:

- `app_sensor_types.c/.h`: `const struct app_sensor_type m_sensor_types[]` with the
  channel descriptors, plus lookup helpers (`app_sensor_type_get(id)`,
  `app_sensor_type_of_source()`, `app_sensor_channel_get(type, ch)`,
  `app_sensor_channel_by_name()`).
- A table block inside `ttn.js`. The decoder must stay one strict-ES5 file with no
  `require()` (LNS sandboxes), so the generator rewrites the region between
  `// BEGIN sensor_types` / `// END sensor_types` markers.

The **Manager-App reads `app_w1_slots.yaml` directly** (D5), pinned to the firmware
release tag it targets. It uses the file for labels, units, scales, valid rule kinds and
the history-capable channel list. Nothing extra is generated for it, and the device only
ever sends ids.

CI (pytest, same pattern as the configen sync test):

- the generated C and the `ttn.js` block are in sync with the YAML;
- ids and channels are append-only against the previous committed copy;
- a type has at most `max_channels` channels and at most one `liveness` channel;
- every `history`-capable channel has an encoding that fits its `range`.

## Firmware changes

### Readings → channels

`struct app_w1_slot_reading` and the ad-hoc on-board fields in `struct app_sensor_data`
become one channel vector per source:

```c
#define APP_SENSOR_CH_MAX 10               /* D6, = app_w1_slots.yaml max_channels */

struct app_sensor_channels {
	uint8_t type;                       /* sensor type id, 0 = none */
	uint16_t valid;                     /* bit ch = value present */
	float v[APP_SENSOR_CH_MAX];         /* physical units; state = 0/1 */
};
```

- Drivers fill channels by number. `machine_probe_read()` also reads the TMP112 into
  ch2, and the MPL3115A2 read fills motherboard ch4.
  - A sub-sensor that fails to respond leaves its bit clear, like today's NaN.
  - A value outside `range` is cleared too; this generalises the DS18B20 range check.
- `read_threshold_value()`, `read_poll_state()` and `read_counter()` collapse into one
  `app_sensor_get(source, ch, &value)`.
- Counters (`counter: true`) stay `uint32_t` at the source, so rate rules and history
  keep the exact integer value. The float vector is only the threshold/state view.
- Callers of the old fields need to move to the channel view:
  - `app_compose.c`,
  - `app_ats.c` (`sN-<quantity>` names become `sN-<channel name>`),
  - `app_history.c`,
  - the `sensor` / `w1` shell.

### Expected type per 1-Wire slot

- New config keys `sensor1_type..sensor4_type` (enum of the 1-Wire type ids, 0 = none).
  - `teach` / `assign` set the key from the detected family.
  - Provisioning can set it before the probe is plugged in.
- Rebind uses the expected type:
  - A bound ROM whose family does not match `sensorN_type` puts the slot in **mismatch**.
  - Auto-enroll only fills a free slot whose `sensorN_type` is the detected type, or none.

### Mismatch behaviour (D4)

While a slot is in mismatch:

- **Alarm:** an `AlarmEvent` with `Type = TYPE_SENSOR_MISMATCH` (5) on that slot:
  - `source = sN`, `sensor_type` = the **expected** type, `value` = the **detected** type id;
  - an ACTIVATE edge when the mismatch appears, a DEACTIVATE edge when the right type is
    back, or when the slot is re-taught / cleared;
  - it comes from the same watchdog path as `TYPE_NO_DATA`, and the no-data watchdog for
    that slot is suppressed while mismatch is active, so there is one alarm, not two.
- **Rules:** all rules on the slot are inert, because none of its channels are valid.
- **Telemetry:** the slot's `SensorReading` is still sent with `type` = the expected type,
  and every value is absent, so the decoder emits `null` for each channel.
- **History:** the slot's recorded channels store the absent sentinel, and the decoder
  emits `null`.
- **Info:** a per-slot state (ok / absent / replaced / mismatch) next to `w1_slot_type`.

### Rules

- The 17 B `alarm_N` blob stays the same size. Byte `[2]` becomes **channel** (was
  quantity).
- `app_alarm_rule_valid(source, ch)` checks the channel against the source's type table:
  - A rule on a 1-Wire slot needs `sensorN_type` set, otherwise `-EINVAL`. The order is:
    provision the type, then the rules.
  - `kind: none` channels (e.g. orientation) are not rule targets.
  - Kind and the momentary edge-only constraint come from the descriptor. This replaces
    `app_alarm_quantity_kind()`, `source_is_momentary()` and `rule_state_shape_valid()`'s
    source list.
- `alarm_scale()` becomes a lookup of `wire.scale`.
- The no-data watchdog watches the `liveness` channel of every present or configured
  source. This replaces the hand-written `m_nodata_tab`.
- Shell: `alarm set <rule> <source> <channel-name|number> ...` with names from the
  registry. `sensor types` lists the tables.

## History per channel (D3)

History becomes selectable per `(source, channel)`. It replaces the fixed
`enum app_history_sensor` (`APP_HISTORY_S1_TEMP`, `APP_HISTORY_S1_HUM`, …) and the
`history_sensors` bitmask.

- **Selection:** a new config key `history_channels`, a `bytes` list of up to **24**
  entries, one byte each: `source << 4 | ch` (source ≤ 15, ch ≤ 9). The order of the list
  is the order of values in a record. An entry is valid only if the channel has a
  `history` encoding and, for a slot source, `sensorN_type` is set.
- **Record layout:** the concatenation of each selected channel's `history.enc` width
  (`i16` / `u8` / `u16` / `u32`). The layout is derived from the selection plus the
  expected slot types, not from what happens to be plugged in, so it stays stable when a
  probe is absent or mismatched.
  - A change of `history_channels` or of any selected slot's `sensorN_type` changes the
    layout. The ring restarts, as it does today when the sample size changes (page-header
    `sample_size` guard).
- **Absent value:** the encoding's sentinel (`INT16_MAX` for `i16`, all-ones for
  unsigned). This covers a missing sub-sensor, a probe that is not present, and a
  mismatched slot, and the decoder emits `null` for all three.
- **`HistoryFrame` stays self-describing for the stateless decoder:**
  - new `bytes channels = 10`: the selection list (1 B per entry);
  - new `bytes w1_types = 11`: the expected type of s1..s4 (4 B), needed to resolve slot
    entries;
  - `present` (bit i = list entry i has data in this frame) and the sample format are
    unchanged in spirit.
  - The decoder resolves each entry to `(type, channel)` → name, encoding and scale from
    its generated table.
- **Manager-App:** it offers the `history`-capable channels of each source, from
  `app_w1_slots.yaml` plus the device's `sensorN_type`, and writes `history_channels`.
- **Budget:**
  - The worst case record is 24 × 4 B = 96 B (all counters), against today's
    19-channel maximum of 44 B.
  - A realistic selection (on-board temp/hum + 4 slots × 2) is 1×2+1 + 4×(2+1) = 15 B,
    the same as today.
  - `MAX_RECORD_SIZE` is sized from 24 entries.

## Wire changes (breaking)

| Message | Change |
|---|---|
| `AlarmEvent` | `quantity = 6` → `reserved 6`; new `uint32 channel = 10`, `optional uint32 sensor_type = 11` |
| `AlarmEvent.Type` | new `TYPE_SENSOR_MISMATCH = 5` (value = detected type id) |
| `Info.AlarmStatus` | `quantity = 2` → `reserved 2`; new `uint32 channel = 4`, `optional uint32 sensor_type = 5` |
| `Info` | per-slot state (ok / absent / replaced / mismatch) |
| `SensorReading` | values per channel (D2); mismatch/absent = all values absent |
| `HistoryFrame` | new `channels = 10`, `w1_types = 11` |
| config | `alarm_N[2]` = channel; new `sensorN_type`, `history_channels`; `history_sensors` removed |

- `sensor_type` in alarms is sent **only for 1-Wire slot sources**. The decoder maps a
  built-in source to its fixed type, so on-board and discrete events stay as small as
  today. This matters for the 11 B tier (US915 DR0 / AU915 DR2) found in the uplink split
  audit.
- The decoder stays stateless (#425 constraint): everything it needs is in the frame
  plus its generated table.

**Telemetry `SensorReading` (field 27)** already carries `slot` and `type`, but it has
fixed per-quantity fields (3..10). There are two options:

- **(a)** Keep the typed fields and add fields per new channel. This is simple, but each
  new sensor edits the proto again.
- **(b)** `repeated sint32 value = 11 [packed]`, indexed by channel and scaled by
  `wire.scale`, with `INT32_MIN` meaning absent (`null` in the decoder). This is fully
  generic and uses the same table as alarms and history.

Recommendation: **(b)**. This is D2, still open.

## Delivery (PR sequence)

1. **Registry + generator.** `app_w1_slots.yaml` (this draft, frozen), `west sensorgen`,
   generated C + `ttn.js` block, CI checks. No behaviour change.
2. **Readings → channels.** `struct app_sensor_channels`, drivers fill channels, the
   TMP112 and MPL3115A2 temperatures become readable. Telemetry and history are still
   encoded from the channel view into their current formats.
3. **Expected slot type + mismatch.** `sensorN_type`, rebind by type,
   `TYPE_SENSOR_MISMATCH`, null values in telemetry, Info slot state.
4. **Rules + alarm wire.** Blob byte `[2]` = channel, validity/kind/scale/liveness from
   the registry, `AlarmEvent` / `AlarmStatus` changes, `ttn.js`, shell, ATS.
5. **Telemetry `SensorReading` per channel** (D2).
6. **History per channel.** `history_channels`, derived layout, `HistoryFrame`
   `channels` / `w1_types`, decoder.

Parallel work outside this repo:

- Manager-App MR (rule editor + history selection from `app_w1_slots.yaml`) after step 4;
- the ProXimos/Hub decoder with the same table.

Each step lands with its own tests, and `doc/` updates are the last commit of each PR.

## Verification

- `tests/alarm_rules`:
  - validity per type table,
  - slot rule rejected without `sensorN_type`,
  - channel out of range or `kind: none`,
  - momentary edge-only rule from the descriptor.
- `tests/alarm_eval`:
  - two temperature channels on one machine-probe slot fire independently,
  - a mismatch raises one `TYPE_SENSOR_MISMATCH` (no no-data alarm), makes the slot's
    rules inert, and deactivates after re-teach,
  - scale from the descriptor.
- `tests/compose`, `tests/cmd`:
  - `AlarmEvent` / `AlarmStatus` with and without `sensor_type`,
  - mismatched slot encoded with all values absent,
  - byte budget at the 11 B tier.
- `tests/history`:
  - layout from `history_channels` + slot types,
  - sentinel for absent / mismatch,
  - ring restart on a layout change,
  - 24-entry worst case.
- `ttn.test.js`:
  - decode of every generated `(type, channel)` in telemetry, alarms and history,
  - `null` for absent values,
  - an unknown type or channel falls back to `"t<type>c<ch>"`.
- pytest: generator in sync, append-only numbering, `max_channels`, one liveness channel
  per type.
- HIL:
  - machine-probe with TMP112 populated, both temperature channels in rules and in
    history;
  - a DS18B20 plugged into a slot taught as machine-probe raises the mismatch alarm and
    telemetry shows `null`.

## Decisions

| # | Question | Decision |
|---|---|---|
| D1 | Built-in type ids: separate range (≥ 64) or one flat list | *Proposal:* ≥ 64, keeps 1-Wire ids unchanged on the wire |
| D2 | Telemetry `SensorReading`: typed fields (a) or generic `repeated value` (b) | *Proposal:* (b) |
| D3 | History per channel | **Decided:** yes, `history_channels` (see *History per channel*) |
| D4 | Mismatch reporting | **Decided:** `TYPE_SENSOR_MISMATCH` alarm on the slot + values `null` in telemetry (and history) |
| D5 | Where the Manager-App gets the registry | **Decided:** reads `app_w1_slots.yaml` directly |
| D6 | Max channels per type | **Decided:** 10 |
