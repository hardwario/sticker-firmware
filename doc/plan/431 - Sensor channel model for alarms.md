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

History keeps its own fixed-width encoding per channel (`history: [enc, scale]`,
enc = `u8 | i16 | u16 | i32 | u32`). `i32` is new, for high-resolution channels whose
`range × scale` overflows `i16`.

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

The west command `west sensorgen app/src/app_w1_slots.yaml`
(`scripts/west_commands/sensorgen.py`, alongside `configen`) validates the YAML and
generates two things. Both are committed, like the configen output.

- **`app_sensor_types.h` / `app_sensor_types.c`:**
  - `enum app_sensor_type_id` (`APP_SENSOR_TYPE_MOTHERBOARD = 1`, …);
  - one channel enum per type (`APP_SENSOR_CH_MOTHERBOARD_HALL_LEFT_STATE`,
    `APP_SENSOR_CH_MACHINE_PROBE_TEMPERATURE_AUX`, …, `APP_SENSOR_CH_<TYPE>_COUNT`),
    which drivers use to fill channels by name;
  - the descriptor tables (`struct app_sensor_channel`: kind, `APP_SENSOR_F_*` flags,
    wire type/scale, history encoding/scale, range, `cap_off` =
    `offsetof(struct app_config, cap_*)`);
  - lookups: `app_sensor_type_get(id)`, `app_sensor_type_by_family()`,
    `app_sensor_type_by_name()`, `app_sensor_channel_get(type, ch)`,
    `app_sensor_channel_by_name()`.

  The slot → type mapping (`sensorN_type`) is runtime config and comes in step 3.
- **The `// BEGIN GENERATED SENSOR_TYPES` region of `ttn.js`:** `_SENSOR_TYPES` (type
  id → name + channels `{n, u, k, s, h}`) and `_SENSOR_MB_TYPE`. The decoder must stay
  one strict-ES5 file with no `require()` (LNS sandboxes), so the table is written into
  it rather than imported. It is exported as `sensorTypes` for tests.

**`pending_caps`:** a capability that a channel references but that does not exist in
`app_config.yml` yet (`cap_analog_a` / `_b` until #396 / PR #407 lands).
- The generator accepts it and emits the channel ungated.
- Validation fails once the cap appears in `app_config.yml`, so the entry is removed in
  the PR that adds it.

**Worktrees:** the west workspace resolves extension commands from the shared main
checkout, so in a worktree the command is driven directly from Python, as the pytest
suite does (`sensorgen.Sensorgen().do_run(...)`).

The **Manager-App reads `app_w1_slots.yaml` directly** (D5), pinned to the firmware
release tag it targets. It uses the file for labels, units, scales, valid rule kinds,
capability gating and the history-capable channel list. Nothing extra is generated for
it, and the device only ever sends ids.

CI (`scripts/west_commands/tests/test_sensorgen.py`, same pattern as the configen sync
test; plus the `tests/sensor_types` native ztest suite for the generated lookups):

- the generated C and the `ttn.js` block are in sync with the YAML;
- ids and channels are append-only: the generator parses the committed
  `app_sensor_types.h` and refuses a renumbered or removed type/channel;
- channel limits (32 motherboard / 10 per 1-Wire type), unique names per type;
- every `cap` names an existing `app_config.yml` capability;
- `range × scale` fits the channel's `wire` type and its `history` encoding, with the
  sentinel value kept free. For example, `range [-200, 850]` at ×1000 needs `i32` in
  history, and CI rejects `i16`;
- every channel has a `unit`, and a `range` if it is `kind: threshold`.

## Firmware changes

### Readings → channels (step 2, done)

`struct app_w1_slot_reading` and the ad-hoc fields in `struct app_sensor_data`
(temperature, humidity, pressure, altitude, illuminance, hall/input/motion counts and
states, orientation, voltage) are replaced by channel vectors (`app_sensor.h`):

```c
union app_sensor_value { float f; uint32_t u; };   /* u for counter channels */

struct app_sensor_mb {                     /* slot 0 */
	uint32_t valid;                        /* bit ch = value present */
	union app_sensor_value v[APP_SENSOR_CH_MOTHERBOARD_COUNT]; /* 22 */
};

struct app_sensor_w1 {                     /* slots 1..4 */
	uint8_t type;                          /* registry type id, 0 = none */
	bool present;
	uint32_t valid;
	union app_sensor_value v[APP_SENSOR_W1_CH_MAX]; /* 10 */
};

struct app_sensor_data { struct app_sensor_mb mb; struct app_sensor_w1 w1[4]; };
```

**Channel values**
- A counter channel (`counter: true`) uses `.u`, an exact `uint32_t`; a float would
  round counts above 2^24.
- Every other channel uses `.f` in the channel's unit. It is NaN when absent, and the
  NaN always matches a cleared `valid` bit, so the existing `isnan()` checks keep working.
- State channels are `0.0f` / `1.0f`.
- Motherboard accessors: `APP_SENSOR_MB_F(d, NAME)` / `APP_SENSOR_MB_U(d, NAME)`.

**`app_sensor_channels.c`** holds the helpers, separate from the sampling loop so the
host tests can link them:
- `app_sensor_put_f()` stores a value and applies the registry range. Out of range or
  non-finite → NaN.
- `app_sensor_w1_clear()` resets a slot vector.
- `app_sensor_w1_f()` is transitional: it reads a 1-Wire slot by its **machine-probe**
  channel number for the readers that are still quantity-based (alarm rules, history,
  ATS). Steps 4 and 6 remove it.
  - A dallas slot answers only ch 0, the shared temperature;
    `BUILD_ASSERT(DALLAS_TEMPERATURE == MACHINE_PROBE_TEMPERATURE)`.
  - Every other channel reads NaN.

**Unit change: pressure is now hPa everywhere inside the firmware.**
- The MPL3115A2 reports kPa; `app_sensor_sample()` converts once when it fills the
  channel.
- The compose (×100 → ×10), alarm (×10 dropped) and history (×100 → ×10) conversions
  were adjusted, so the wire values are unchanged.

**New readings**
- The machine-probe TMP112 is read into `temperature-aux` (ch 2).
  - After 3 consecutive failures (older probe revisions have no TMP112) it is skipped
    for that probe until the next rebind. This saves ~60 ms and an error log per sample.
  - It is not on the wire yet: telemetry needs step 5, alarms step 4.
- The MPL3115A2 temperature fills motherboard `temperature-baro` (ch 4).
- Altitude, which telemetry already carries, was missing from the registry and was
  appended as motherboard ch 21.

**Ranges.** The registry ranges were aligned with the drivers' own plausibility gates:
- SHT4x / machine-probe SHT: −45…130 °C;
- battery: 0–6 V;
- pressure: 0–2000 hPa;
- altitude: unbounded, as before.

So `app_sensor_put_f()` never drops a value that passed before.

**Readers moved to the channel view:** `app_alarm.c`, `app_compose.c`, `app_history.c`
(offsets into the channel vectors), `app_cmd.c`, the `w1` shell and `app_ats.c`.

**ATS keeps its names and values.** The factory tester uses them, so `pressure` stays
kPa. The only visible change is that the pressure label in `tester sensors print` now
reads "kPa" (it used to say "Pa", which was wrong). Renaming the ATS sensors to
channel names is left for a later step.

**Sizes vs `v1.5.0`:**

| build | flash | RAM |
|---|---|---|
| release | +2 448 B (165 840 B, 77.86 %) | +128 B |
| debug | +1 536 B (223 692 B, 91.02 %) | +64 B (94.43 %) |

The flash growth is the descriptor tables (now referenced) and the TMP112 read.
`app_compose.c` copies `app_sensor_data` onto the `m_work_q` stack as before; the struct
grew by ~100 B.

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
  - `slot = N`, `rule` absent (watchdog event, today's `0xFF`),
    `sensor_type` = the **expected** type, `value` = the **detected** type id;
  - an ACTIVATE edge when the mismatch appears, a DEACTIVATE edge when the right type is
    back, or when the slot is re-taught / cleared;
  - it comes from the same watchdog path as `TYPE_NO_DATA`, and the no-data watchdog for
    that slot is suppressed while mismatch is active, so there is one alarm, not two.
- **Rules:** all rules on the slot are inert, because none of its channels are valid.
- **Telemetry:** the slot's `SensorReading` is still sent with `type` = the expected type
  and `valid = 0`, so the decoder emits `null` for each channel.
- **History:** the slot's recorded channels store the absent sentinel, and the decoder
  emits `null`.
- **Info:** a per-slot state (ok / absent / replaced / mismatch) next to the slot type.

### Rules

An alarm is defined by three things:

| Term | Meaning | Range |
|---|---|---|
| **rule** | the rule's storage position and stable identity (`alarm_0..alarm_15`) | 0..15 |
| **slot** | where the sensor sits | 0 = motherboard, 1..4 = s1..s4 |
| **channel** | which value of the slot's sensor type | per type table |

The rule also carries the parameters for its channel's kind: lo/hi/dwell (threshold),
from/to/dwell (state) or hi/dwell (rate). Kind and scale are not stored; they come from
the channel descriptor.

**Blob layout: 18 B** (was 17 B), little-endian:

```
[0]      flags        bit0 = present (slot occupied), bit1 = enabled
[1]      slot         0 = motherboard, 1..4 = s1..s4
[2]      channel      type-local channel number
[3]      sensor_type  type the rule was written for (1 = motherboard, 2 = dallas, ...)
[4]      from_state   STATE only
[5]      to_state     STATE only
[6..9]   lo           float
[10..13] hi           float
[14..17] dwell        float, seconds
```

An empty rule is 36 zero hex characters (`present = 0`).

**Why `sensor_type` is stored in the rule:**

- A channel number means different things on different types: ch2 is the TMP112
  temperature on a machine-probe, and does not exist on a dallas.
- With the type stored, the firmware can tell when a rule was written for a different
  type than the slot now expects. Without it, such a rule would silently watch something
  else after a `sensorN_type` change.
- The Manager-App can show a rule ("s1 machine-probe temperature-aux") without also
  reading `sensorN_type`.
- `AlarmEvent.sensor_type` is taken straight from the rule.
- The cost is 16 B of config (1 B per rule).

For slot 0, `sensor_type` must be 1 (motherboard).

**Validation.** `app_alarm_rule_valid(slot, ch, sensor_type)` checks the channel against
the table of `sensor_type`:

- Slot 0 needs `sensor_type = 1`; slots 1..4 need a 1-Wire type.
- A rule whose `sensor_type` differs from the slot's current `sensorN_type` is
  **stale**:
  - it is kept (not cleared) but is inert;
  - it counts as a sanitized/invalid rule in `app_alarm_rules_reload_from_config()`, so
    a SetParam that changes `sensorN_type` under existing rules reports a fault instead
    of a silent ACK;
  - the shell lists it as `stale`, and the Manager-App flags it for the user to fix or
    delete.
- A rule on a 1-Wire slot whose `sensorN_type` is not set is stale in the same way.
  The order is: provision the type, then the rules.
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
- Shell: `alarm set <rule> <slot> <channel-name|number> ...`. The shell fills
  `sensor_type` from the slot's current type.
  `sensor types` lists the tables.

**Examples:**

| rule | slot | channel | type | meaning |
|---|---|---|---|---|
| 0 | 0 | 0 | 1 | on-board SHT4x temperature outside 2–8 °C |
| 1 | 0 | 6 | 1 | hall left: more than 100 pulses per interval |
| 2 | 0 | 9 | 1 | input A: edge 0→1 |
| 3 | 1 | 0 | 3 | s1 machine-probe: SHT temperature |
| 4 | 1 | 2 | 3 | s1 machine-probe: TMP112 temperature (second temperature on the same probe) |
| 5 | 2 | 0 | 2 | s2 dallas: temperature |

Shell equivalents:

```
alarm set 0 mb temperature lo 2 hi 8 dwell 60
alarm set 1 mb hall-left-count hi 100 dwell 0
alarm set 2 mb input-a-state from 0 to 1 dwell 5
alarm set 4 s1 temperature-aux lo -10 hi 60 dwell 30
```

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
| `AlarmEvent` | field **renames** (numbers kept): `source = 1` → **`slot = 1`** (0 = motherboard, 1..4 = s1..s4), `slot = 7` → **`rule = 7`** (rule index, `0xFF` = watchdog); `quantity = 6` → `reserved 6`; new `uint32 channel = 10`, `optional uint32 sensor_type = 11` |
| `AlarmEvent.Type` | new `TYPE_SENSOR_MISMATCH = 5` (value = detected type id) |
| `Info.AlarmStatus` | `source = 1` → **`slot = 1`**; `quantity = 2` → `reserved 2`; new `uint32 channel = 4`, `optional uint32 sensor_type = 5` |
| `Info` | per-slot state (ok / absent / replaced / mismatch) |
| `SensorReading.type`, `ConfigDump.w1_slot_type` | new type ids (2 dallas, 3 machine-probe) |
| `SensorReading` | `reserved 3 to 10`; new `valid = 11` + `repeated sint32 value = 12 [packed]` (D2 = c); mismatch/absent = `valid = 0` |
| `HistoryFrame` | new `channels = 10`, `w1_types = 11` |
| config | `alarm_N` 17 → **18 B** (slot, channel, sensor_type); new `sensorN_type`, `history_channels`; `history_sensors` removed |

- **Why the renames:** the old `AlarmEvent` had `source` (where) and `slot` (which rule),
  so "slot" would have meant two different things. Renaming keeps every field number,
  so the bytes are unchanged. Only the decoder's output keys change (`source` → `slot`,
  `slot` → `rule`), which is fine in a non-migratable release.
- In alarms, `sensor_type` is sent **only for slots 1..4**. Slot 0 is always
  `motherboard`, so on-board events stay as small as today. This matters for the 11 B
  tier (US915 DR0 / AU915 DR2) found in the uplink split audit.
- The decoder stays stateless (#425 constraint): everything it needs is in the frame
  plus its generated table.

### Telemetry `SensorReading` (D2 = c)

`SensorReading` (field 27) keeps `slot = 1` and `type = 2`. The fixed per-quantity fields
3..10 (temperature, humidity, flags, illuminance, magnetic_field, accel_x/y/z) are
replaced by a valid mask plus packed values:

```proto
message SensorReading {
    reserved 3 to 10;                         // typed fields, replaced by the channel model (#430)
    uint32 slot            = 1;               // 1..4 = s1..s4
    uint32 type            = 2;               // sensor type id (app_w1_slots.yaml)
    uint32 valid           = 11;              // bit ch = channel ch has a value
    repeated sint32 value  = 12 [packed = true]; // values of the set bits only, ascending ch,
                                              // each round(phys * wire.scale)
}
```

- **Absent costs nothing.** A channel without a value (sub-sensor not responding, cap
  off, out of range) has its bit clear and no entry in `value`.
- **Mismatch or absent probe:** `valid = 0` and no values. The decoder emits `null` for
  every channel of `type`.
- **Signedness:** all values are `sint32` (zigzag) on the wire, whatever the channel's
  `wire` type. The decoder uses `wire` only for the scale and the range check.

Options considered:

- **(a)** Typed fields, plus a new field per new channel. This is the smallest on the
  wire, but every sensor edits the proto and the decoder by hand. It also cannot carry a
  per-channel scale (below).
- **(b)** `repeated value` indexed by channel with an `INT32_MIN` "absent" sentinel. It
  is generic, but the sentinel costs a 5 B varint per absent value, so a mismatched
  machine-probe would send ~45 B of nulls. That is not usable at the 11 B tier.
- **(c)** Valid mask + only the present values. **Chosen.**

Approximate encoded size of one `SensorReading` (typical values: 23.45 °C, 45 %RH,
300 lx, ~9.81 m/s²):

| Case | (a) typed | (b) sentinel | (c) mask |
|---|---|---|---|
| dallas, temperature | ~9 B | ~10 B | ~12 B |
| machine-probe, all 9 channels | ~30 B | ~24 B | ~27 B |
| machine-probe, TMP112 missing | ~27 B | ~28 B | ~24 B |
| slot in mismatch | ~6 B | ~51 B | ~8 B |
| new sensor type | proto + decoder change | YAML only | YAML only |

The motherboard's Telemetry groups (climate, hall, inputs, …) keep their typed fields in
this release, because the composer packs them by group priority. Moving them to the same
`valid` + `value` shape (one group per channel block) is a follow-up.

### Units and scale per channel

The unit and resolution are defined **per channel in the YAML**, not per quantity. A
sensor that is atypical just declares a different `unit` / `scale`. For example:

```yaml
# high-resolution thermometer, 0.001 degC
- {ch: 0, name: temperature, quantity: temperature, unit: degC, kind: threshold,
   wire: [sint32, 1000], history: [i32, 1000], range: [-200.0, 850.0]}
# sensor reporting in kelvin
- {ch: 0, name: temperature, quantity: temperature, unit: K, kind: threshold,
   wire: [uint32, 100], history: [u16, 100], range: [0.0, 600.0]}
```

- **Firmware:** keeps the value in the channel's physical unit and puts
  `round(value × scale)` on the wire. Alarm `lo` / `hi` are in the channel's unit (K in
  the second example). The Manager-App shows the unit from the YAML.
- **Decoder:** divides by the channel's scale and **includes the unit** in its output:
  ```json
  {"slot": 1, "type": "machine-probe",
   "values": {"temperature": 23.45, "temperature-aux": null},
   "units":  {"temperature": "degC", "temperature-aux": "degC"}}
  ```
  The same quantity can now come in different units, so a bare number would be
  ambiguous.
- **Convention:** drivers convert to the **canonical unit of the quantity** unless there
  is a reason not to:

  | quantity | canonical unit |
  |---|---|
  | temperature | degC |
  | humidity | %RH |
  | pressure | hPa |
  | voltage | V |
  | illuminance | lx |
  | magnetic_field | mT |
  | acceleration | m/s2 |

  Extra resolution is a matter of `scale`, not of the unit. A non-canonical unit (like K)
  is allowed, but the Manager-App then cannot overlay that channel with other
  temperatures without converting.
- **Cost of resolution:** 23.456 °C at ×1000 is a 3 B varint instead of 2 B at ×100.

## Delivery (steps in this PR)

The implementation lands **incrementally in this PR (#431)**, one step after another.
Each step is its own commit series with its own tests, and CI must be green at the end
of every step.

1. ✅ **Registry + generator.**
   - `app_w1_slots.yaml`, `west sensorgen`, the generated `app_sensor_types.{c,h}` and
     `ttn.js` region.
   - `test_sensorgen.py` (sync, validation, append-only guard) and the
     `tests/sensor_types` ztest.
   - No behaviour change: nothing references the tables yet, so release and debug
     images are byte-identical in size to `v1.5.0` (release 163 392 B flash /
     52 812 B RAM, debug 222 156 B / 61 820 B).
2. ✅ **Readings → channels.** Channel vectors, `app_sensor_channels.c`, hPa pressure,
   TMP112 + MPL3115A2 temperatures, altitude channel, all readers moved. Telemetry,
   history and alarm wire formats are unchanged. Tests: `alarm_eval` (+4: machine-probe
   / dallas slot channels, hPa pressure) and `sensor_types` (+4: channel helpers).
3. **Expected slot type + mismatch.** `sensorN_type`, new type ids, rebind by type,
   `TYPE_SENSOR_MISMATCH`, null values in telemetry, Info slot state.
4. **Rules + alarm wire.** Blob `[1..2]` = slot/channel, source enum removed,
   18 B blob with `sensor_type`, stale-rule detection, validity/kind/scale/liveness/
   watchdogs from the registry, `AlarmEvent` / `AlarmStatus` changes (incl. the
   `slot` / `rule` renames), `ttn.js`, shell, ATS.
5. **Telemetry `SensorReading` per channel** (D2 = c). `valid` + packed values, decoder
   output with units, `i32` history encoding.
6. **History per channel.** `history_channels`, derived layout, `HistoryFrame`
   `channels` / `w1_types`, decoder.

Parallel work outside this repo:

- Manager-App MR (rule editor + history selection from `app_w1_slots.yaml`) after step 4;
- the ProXimos/Hub decoder with the same table.

Each step lands with its own tests. The `doc/` updates (release notes, `doc/` guides) are
the last commit before the PR leaves draft.

## Verification

- `tests/alarm_rules`:
  - validity per type table,
  - slot rule rejected without `sensorN_type`,
  - channel out of range, `kind: none` or `alarm_only_watchdog`,
  - momentary edge-only rule from the descriptor,
  - 18 B blob pack/unpack round-trip,
  - a `sensorN_type` change turns the slot's rules stale (inert, reported by reload),
  - slot 0 with `sensor_type != 1` rejected,
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
  - byte budget at the 11 B tier,
  - `SensorReading` with a partial `valid` mask, `valid = 0` for mismatch, and a ×1000
    channel.
- `tests/history`:
  - layout from `history_channels` + slot types,
  - cap-off entry rejected,
  - sentinel for absent / mismatch,
  - ring restart on a layout change,
  - 24-entry worst case.
- `ttn.test.js`:
  - decode of every generated `(type, channel)` in telemetry, alarms and history,
  - `null` for absent values,
  - units in the output, including a non-canonical unit (K),
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
| D2 | Telemetry `SensorReading` encoding | **Decided:** (c) valid mask + packed present values; unit and scale per channel in the YAML, decoder outputs units, `i32` history encoding, CI range check |
| D3 | History per channel | **Decided:** yes, `history_channels` (see *History per channel*) |
| D4 | Mismatch reporting | **Decided:** `TYPE_SENSOR_MISMATCH` alarm on the slot + values `null` in telemetry (and history) |
| D5 | Where the Manager-App gets the registry | **Decided:** reads `app_w1_slots.yaml` directly |
| D6 | Max channels per type | **Decided:** 10 per 1-Wire type; the motherboard has its own limit of 32 (21 used), because it now carries every on-board sensor |
| D7 | Alarm rule identity | **Decided:** `(rule, slot, channel)`; `AlarmEvent` fields renamed `source` → `slot`, `slot` → `rule` (numbers kept) |
| D8 | Store the sensor type in the rule | **Decided:** yes, blob 17 → 18 B; a rule with a type that differs from `sensorN_type` is stale (inert, reported) |
