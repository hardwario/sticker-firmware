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
the decoder and the Manager-App then address a value as `(slot, channel)`.

## Model

```
slot 0     ──► motherboard (type 1, fixed)        ─┐
slot 1..4  ──► 1-Wire type from sensorN_type       ├─► channel table (app_w1_slots.yaml)
                                                    ┘   ch → {name, label, quantity, unit, kind,
                                                          momentary, counter, cap, wire, history,
                                                          range, liveness}
rule            = (slot, channel, lo/hi/dwell | from/to)
history channel = (slot, channel)
```

- **Slot** replaces `enum app_alarm_source`:
  - `0` = the STICKER motherboard. Everything on the board is a motherboard channel:
    climate, barometer, light, both hall switches, inputs A/B (digital and analog),
    PIR, accelerometer and the battery rail.
  - `1..4` = the 1-Wire slots s1..s4.

  This removes `APP_ALARM_SRC_HALL_*`, `_INPUT_*`, `_PIR`, `_ACCEL` and `_BATTERY`.
- **Sensor type:** slot 0 is always `motherboard`. A 1-Wire slot has a **configured**
  type (`sensorN_type`, new).
- **Channel:** a type-local number.
  - The motherboard has up to **32** channels (`uint32` valid mask) and uses 21 today.
  - A 1-Wire type has up to **10** (D6).

  A channel means the same thing on every device with that type. The numbering is
  append-only: a channel is never renumbered or reused, and a removed one stays as
  `retired: true`.
- **Capability:** a motherboard channel names the `cap_*` key that enables it
  (`cap_hall_left`, `cap_input_a`, `cap_analog_a`, `cap_pir_detector`, …). A disabled
  channel is not valid: it cannot be a rule target, it is absent from telemetry, and it
  cannot be selected for history.
- **Quantity** (temperature, humidity, …) is only a *property of a channel*, used for UI
  grouping and icons. It no longer identifies anything.

### Sensor types (see the YAML for full descriptors)

Type ids are new in this release (D1): **1 = motherboard**, and 1-Wire types follow.
The legacy `app_w1_slot_type` values (1 dallas, 2 machine-probe) are dropped; id 0 means
none / empty.

| id | type | slot | ch |
|---|---|---|---|
| 1 | `motherboard` | 0 | 0 temperature (SHT4x), 1 humidity, 2 pressure, 3 illuminance, 4 temperature-baro (MPL3115A2), 5/6 hall-left state/count, 7/8 hall-right state/count, 9/10/11 input-a state/count/voltage, 12/13/14 input-b state/count/voltage, 15/16 pir motion/count, 17/18/19 accel motion/count/orientation, 20 battery-voltage |
| 2 | `dallas` (family 0x28) | 1..4 | 0 temperature |
| 3 | `machine-probe` (family 0x19) | 1..4 | 0 temperature (SHT), 1 humidity (SHT), 2 temperature-aux (TMP112), 3 illuminance, 4 magnetic-field, 5 tilt, 6..8 accel-x/y/z |

**One wire scale per channel.** A channel has a single `wire` scale, shared by telemetry
and `AlarmEvent.value`. Today the two differ for:

- humidity: telemetry ×2, alarm ×100 → **×2** everywhere,
- voltage: telemetry V×50, alarm ×100, #396 mV → **mV (×1000)** everywhere.

History keeps its own fixed-width encoding per channel (`history: [enc, scale]`).

**No fixed number of types.**

- Type ids are `uint8` in config (`sensorN_type`, `HistoryFrame.w1_types`) and a varint
  on the wire, so up to 255 types fit without any format change.
- The firmware only carries the types listed in the YAML, as a generated array.
- Adding a type = one YAML entry + its driver.
- A decoder that does not know a type yet falls back to `"t<type>c<ch>"`.
- Only the channels per type are bounded, because they size the valid masks and the
  reading arrays.

## Registry: `app_w1_slots.yaml`, generated everywhere

The channel tables are written once, in **`app/src/app_w1_slots.yaml`**. That file is the
only place they are edited.

A new west command (`west sensorgen`, alongside `configen`) generates:

- `app_sensor_types.c/.h`: descriptor tables, `APP_SENSOR_MB_CH_*` channel constants
  (`APP_SENSOR_MB_CH_HALL_LEFT_STATE`, …) for drivers to fill by name, the per-type
  channel counts, and lookups (`app_sensor_type_get(id)`, `app_sensor_type_of_slot()`,
  `app_sensor_channel_get(type, ch)`, `app_sensor_channel_by_name()`).
- A table block inside `ttn.js`. The decoder must stay one strict-ES5 file with no
  `require()` (LNS sandboxes), so the generator rewrites the region between
  `// BEGIN sensor_types` / `// END sensor_types` markers.

The **Manager-App reads `app_w1_slots.yaml` directly** (D5), pinned to the firmware
release tag it targets. It uses the file for labels, units, scales, valid rule kinds,
capability gating and the history-capable channel list. Nothing extra is generated for
it, and the device only ever sends ids.

CI (pytest, same pattern as the configen sync test):

- the generated C and the `ttn.js` block are in sync with the YAML;
- ids and channels are append-only against the previous committed copy;
- channel limits (32 motherboard / 10 per 1-Wire type), unique names per type;
- every `cap` names an existing `app_config.yml` capability;
- every `history`-capable channel has an encoding that fits its `range`.

## Firmware changes

### Readings → channels

`struct app_w1_slot_reading` and the ad-hoc fields in `struct app_sensor_data`
(temperature, humidity, pressure, illuminance, hall/input/motion counts and states,
orientation, voltage) become channel vectors. The arrays are sized from the generated
counts, not from the limits:

```c
struct app_sensor_mb {                     /* slot 0 */
	uint32_t valid;                        /* bit ch = value present */
	float v[APP_SENSOR_MB_CH_COUNT];       /* 21 today; physical units, state = 0/1 */
	uint32_t count[APP_SENSOR_MB_CNT_COUNT]; /* exact counters, indexed via descriptor */
};

struct app_sensor_w1 {                     /* slots 1..4 */
	uint8_t type;                          /* configured type id, 0 = none */
	uint16_t valid;
	float v[APP_SENSOR_W1_CH_MAX];         /* 10 */
};
```

RAM: 21×4 + 6×4 + 4×(10×4 + 4) ≈ 290 B, against today's `app_sensor_data`
≈ 190 B. The debug build is at ~99 % RAM, so the budget needs checking in step 2.

- Drivers fill channels by constant:
  - `machine_probe_read()` also reads the TMP112 into ch2;
  - the MPL3115A2 read fills motherboard ch4;
  - hall, input, PIR and accel handlers write their state/count channels.

  A sub-sensor that fails to respond leaves its bit clear, like today's NaN. A value
  outside `range` is cleared too; this generalises the DS18B20 range check.
- `read_threshold_value()`, `read_poll_state()` and `read_counter()` collapse into one
  `app_sensor_get(slot, ch, &value)`. Counters (`counter: true`) are read from the exact
  `uint32_t` store.
- `app_alarm_event(source, active)` for discrete edges becomes
  `app_alarm_event(ch, active)` on slot 0.
- Callers of the old fields need to move to the channel view:
  - `app_compose.c`,
  - `app_ats.c` (sensor names become `<channel name>` / `sN-<channel name>`),
  - `app_history.c`,
  - the `sensor` / `w1` shell,
  - `app_report.c`.

### Expected type per 1-Wire slot

- New config keys `sensor1_type..sensor4_type` (1-Wire type ids, 0 = none).
  - `teach` / `assign` set the key from the detected family.
  - Provisioning can set it before the probe is plugged in.
- Rebind uses the expected type. Slots are ROM-bound on one shared bus, so "something
  else is connected" shows up as the slot's device missing plus a foreign device appearing:
  - A slot with a configured ROM that is absent, while an unbound device of a
    **different** type is on the bus and no free slot expects that type, goes to
    **mismatch**. Today this is only flagged `replaced`, and only for a same-type device.
  - A slot with `sensorN_type` set but no ROM yet (provisioned, not taught) goes to
    mismatch when the only unbound device is of a different type.
  - `teach` / `assign` of a device whose type differs from a set `sensorN_type` is
    refused (`-EINVAL`) unless `sensorN_type` is cleared first.
  - Auto-enroll only fills a free slot whose `sensorN_type` is the detected type, or none.

### Mismatch behaviour (D4)

While a slot is in mismatch:

- **Alarm:** an `AlarmEvent` with `Type = TYPE_SENSOR_MISMATCH` (5) on that slot:
  - `source = N`, `sensor_type` = the **expected** type, `value` = the **detected** type id;
  - an ACTIVATE edge when the mismatch appears, a DEACTIVATE edge when the right type is
    back, or when the slot is re-taught / cleared;
  - it comes from the same watchdog path as `TYPE_NO_DATA`, and the no-data watchdog for
    that slot is suppressed while mismatch is active, so there is one alarm, not two.
- **Rules:** all rules on the slot are inert, because none of its channels are valid.
- **Telemetry:** the slot's `SensorReading` is still sent with `type` = the expected type,
  and every value is absent, so the decoder emits `null` for each channel.
- **History:** the slot's recorded channels store the absent sentinel, and the decoder
  emits `null`.
- **Info:** a per-slot state (ok / absent / replaced / mismatch) next to the slot type.

### Rules

- The 17 B `alarm_N` blob stays the same size:
  - byte `[1]` = **slot** (0..4, was source),
  - byte `[2]` = **channel** (was quantity).
- `app_alarm_rule_valid(slot, ch)` checks the channel against the slot's type table:
  - A rule on a 1-Wire slot needs `sensorN_type` set, otherwise `-EINVAL`. The order is:
    provision the type, then the rules.
  - A motherboard rule is accepted even while the channel's `cap` is off, as today's
    "provision before enable". It stays inert until the capability is on.
  - `kind: none` (orientation) and `alarm_only_watchdog` (battery) channels are not rule
    targets.
  - Kind, `momentary` (edge-only STATE) and `counter` come from the descriptor. This
    replaces `app_alarm_quantity_kind()`, `source_is_momentary()` and the source lists
    in `rule_state_shape_valid()` / `app_alarm_rule_valid()`.
- `alarm_scale()` becomes a lookup of the `wire` scale.
- The no-data watchdog watches every `liveness` channel of an enabled/configured slot.
  This replaces the hand-written `m_nodata_tab`: motherboard temperature, humidity,
  pressure and battery, and the primary temperature of each 1-Wire type.
- The low-battery watchdog becomes the evaluator of motherboard ch20.
- Shell: `alarm set <rule> <slot> <channel-name|number> ...` (e.g.
  `alarm set 0 mb hall-left-count ...`, `alarm set 1 s2 temperature-aux ...`).
  `sensor types` lists the tables.

## History per channel (D3)

History becomes selectable per `(slot, channel)`. It replaces the fixed
`enum app_history_sensor` (`APP_HISTORY_S1_TEMP`, `APP_HISTORY_HALL_LEFT`, …) and the
`history_sensors` bitmask.

- **Selection:** a new config key `history_channels`, a `bytes` list of up to **24**
  entries, one byte each: `slot << 5 | ch` (slot 0..4, ch 0..31). The order of the list
  is the order of values in a record. An entry is valid only if:
  - the channel has a `history` encoding,
  - its `cap` is on (motherboard),
  - `sensorN_type` is set (1-Wire slot).
- **Record layout:** the concatenation of each selected channel's history width
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
- **Manager-App:** it offers the `history`-capable, enabled channels of each slot, from
  `app_w1_slots.yaml` plus the device's caps and `sensorN_type`, and writes
  `history_channels`.
- **Budget:**
  - The worst case record is 24 × 4 B = 96 B (all counters), against today's
    19-channel maximum of 44 B.
  - A realistic selection (on-board temp/hum + 4 slots × temp/hum) is 1×2+1 + 4×(2+1)
    = 15 B, the same as today.
  - `MAX_RECORD_SIZE` is sized from 24 entries.

## Wire changes (breaking)

| Message | Change |
|---|---|
| `AlarmEvent` | `source = 1` is now the slot (0 = motherboard, 1..4 = s1..s4); `quantity = 6` → `reserved 6`; new `uint32 channel = 10`, `optional uint32 sensor_type = 11` |
| `AlarmEvent.Type` | new `TYPE_SENSOR_MISMATCH = 5` (value = detected type id) |
| `Info.AlarmStatus` | `source = 1` = slot; `quantity = 2` → `reserved 2`; new `uint32 channel = 4`, `optional uint32 sensor_type = 5` |
| `Info` | per-slot state (ok / absent / replaced / mismatch) |
| `SensorReading.type`, `ConfigDump.w1_slot_type` | new type ids (2 dallas, 3 machine-probe) |
| `SensorReading` | values per channel (D2); mismatch/absent = all values absent |
| `HistoryFrame` | new `channels = 10`, `w1_types = 11` |
| config | `alarm_N[1..2]` = slot/channel; new `sensorN_type`, `history_channels`; `history_sensors` removed |

- In alarms, `sensor_type` is sent **only for slots 1..4**. Slot 0 is always
  `motherboard`, so on-board events stay as small as today. This matters for the 11 B
  tier (US915 DR0 / AU915 DR2) found in the uplink split audit.
- The decoder stays stateless (#425 constraint): everything it needs is in the frame
  plus its generated table.

**Telemetry.** `SensorReading` (field 27) already carries `slot` and `type`, but it has
fixed per-quantity fields (3..10). There are two options:

- **(a)** Keep the typed fields and add fields per new channel. This is simple, but each
  new sensor edits the proto again.
- **(b)** `repeated sint32 value = 11 [packed]`, indexed by channel and scaled by the
  `wire` scale, with `INT32_MIN` meaning absent (`null` in the decoder). This is fully
  generic and uses the same table as alarms and history.

Recommendation: **(b)**. This is D2, still open.

- The motherboard's Telemetry groups (climate, hall, inputs, …) can keep their typed
  fields for now, because the composer packs them by group priority.
- Moving them to channel values too is a follow-up once D2 is settled.

## Delivery (PR sequence)

1. **Registry + generator.** `app_w1_slots.yaml` (this draft, frozen), `west sensorgen`,
   generated C + `ttn.js` block, CI checks. No behaviour change.
2. **Readings → channels.** `struct app_sensor_mb` / `app_sensor_w1`, drivers fill
   channels, the TMP112 and MPL3115A2 temperatures become readable, RAM check. Telemetry
   and history are still encoded from the channel view into their current formats.
3. **Expected slot type + mismatch.** `sensorN_type`, new type ids, rebind by type,
   `TYPE_SENSOR_MISMATCH`, null values in telemetry, Info slot state.
4. **Rules + alarm wire.** Blob `[1..2]` = slot/channel, source enum removed,
   validity/kind/scale/liveness/watchdogs from the registry, `AlarmEvent` /
   `AlarmStatus` changes, `ttn.js`, shell, ATS.
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
  - channel out of range, `kind: none` or `alarm_only_watchdog`,
  - momentary edge-only rule from the descriptor,
  - a motherboard rule on a cap-off channel is accepted but inert.
- `tests/alarm_eval`:
  - two temperature channels on one machine-probe slot fire independently,
  - hall/input/PIR/accel edges via `app_alarm_event(ch, …)`,
  - a mismatch raises one `TYPE_SENSOR_MISMATCH` (no no-data alarm), makes the slot's
    rules inert, and deactivates after re-teach,
  - low battery via ch20,
  - scale from the descriptor.
- `tests/compose`, `tests/cmd`:
  - `AlarmEvent` / `AlarmStatus` with and without `sensor_type`,
  - mismatched slot encoded with all values absent,
  - byte budget at the 11 B tier.
- `tests/history`:
  - layout from `history_channels` + slot types,
  - cap-off entry rejected,
  - sentinel for absent / mismatch,
  - ring restart on a layout change,
  - 24-entry worst case.
- `ttn.test.js`:
  - decode of every generated `(type, channel)` in telemetry, alarms and history,
  - `null` for absent values,
  - an unknown type or channel falls back to `"t<type>c<ch>"`.
- pytest: generator in sync, append-only numbering, channel limits, `cap` names exist.
- HIL:
  - machine-probe with TMP112 populated, both temperature channels in rules and in
    history;
  - a DS18B20 plugged into a slot taught as machine-probe raises the mismatch alarm and
    telemetry shows `null`;
  - hall/input rules on slot 0.

## Decisions

| # | Question | Decision |
|---|---|---|
| D1 | Type id numbering | **Decided:** new ids, `1 = motherboard`, `2 = dallas`, `3 = machine-probe`; all on-board sources (hall, input, PIR, accel, battery) are motherboard channels, slot 0 |
| D2 | Telemetry `SensorReading`: typed fields (a) or generic `repeated value` (b) | *Proposal:* (b) |
| D3 | History per channel | **Decided:** yes, `history_channels` (see *History per channel*) |
| D4 | Mismatch reporting | **Decided:** `TYPE_SENSOR_MISMATCH` alarm on the slot + values `null` in telemetry (and history) |
| D5 | Where the Manager-App gets the registry | **Decided:** reads `app_w1_slots.yaml` directly |
| D6 | Max channels per type | **Decided:** 10 per 1-Wire type; the motherboard has its own limit of 32 (21 used), because it now carries every on-board sensor |
