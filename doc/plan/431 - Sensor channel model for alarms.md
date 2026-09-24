# 431 — Sensor channel model for alarms

Issue: #430 · PR: #431 · Target: the next **non-migratable** release (config and alarm
wire format break; devices are re-provisioned). The release is not decided yet.

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
   - the Manager-App.

The fix is to make a sensor type describe its own channels, and to let rules, the wire and
the decoder address a channel by number.

## Model

```
source ──► sensor type ──► channel table
                            ch → {name, quantity, unit, scale, kind, liveness}
rule = (source, channel, lo/hi/dwell | from/to)
```

- **Source:** the physical place a sensor sits. The current `enum app_alarm_source` is
  kept (onboard, s1..s4, hall-left/right, input-a/b, pir, accel, battery).
- **Sensor type:** what sits at the source.
  - Built-in sources have a fixed type (onboard → `motherboard`, input-a/b → `input`, …).
  - A 1-Wire slot has a **configured** type (`sensorN_type`, new).
- **Channel:** a type-local number. A channel means the same thing on every device with
  that type. The numbering is append-only; a channel is never renumbered or reused.
- **Quantity** (temperature, humidity, …) stays, but only as a *property of a channel*
  (unit, display name, grouping in the UI). It no longer identifies a rule.

### Sensor types (initial table)

Type ids share one number space. 1-Wire types keep today's `app_w1_slot_type` values
(1 dallas, 2 machine-probe), because they are already on the wire in
`SensorReading.type` and `ConfigDump.w1_slot_type`. Built-in types start at 64.

| id | type | channels |
|---|---|---|
| 1 | `dallas` | 0 temperature |
| 2 | `machine-probe` | 0 temperature (SHT), 1 humidity (SHT), 2 temperature (TMP112), 3 illuminance, 4 magnetic-field, 5 tilt (state), 6..8 accel x/y/z |
| 64 | `motherboard` | 0 temperature (SHT4x), 1 humidity (SHT4x), 2 pressure (MPL3115A2), 3 illuminance (OPT3001), 4 temperature (MPL3115A2) |
| 65 | `hall` | 0 state, 1 count |
| 66 | `input` | 0 state, 1 count, 2 voltage (analog, #396) |
| 67 | `pir` | 0 state (momentary), 1 count |
| 68 | `accel` | 0 state (momentary), 1 count |
| 69 | `battery` | 0 voltage (low-battery watchdog, not a rule target) |

Channel numbers are proposals. They are frozen when the registry lands (step 1).

## Registry: one YAML, generated everywhere

The channel tables must not be hand-maintained in C, `ttn.js` and the Manager-App. They
come from **`app/src/sensor_types.yml`**:

```yaml
types:
  - id: 2
    name: machine-probe
    w1_family: 0x19            # 1-Wire only: family code used for detection
    channels:
      - {ch: 0, name: temperature,     quantity: temperature, unit: degC, scale: 100, kind: threshold, liveness: true}
      - {ch: 1, name: humidity,        quantity: humidity,    unit: "%RH", scale: 100, kind: threshold}
      - {ch: 2, name: temperature-aux, quantity: temperature, unit: degC, scale: 100, kind: threshold}
      - {ch: 5, name: tilt,            quantity: tilt,        kind: state}
      # ...
```

A new west command (`west sensorgen`, alongside `configen`) generates:

- `app_sensor_types.c/.h`: `const struct app_sensor_type m_sensor_types[]` with the
  channel descriptors, plus lookup helpers (`app_sensor_type_get(id)`,
  `app_sensor_channel_get(type, ch)`, `app_sensor_channel_by_name()`).
- A table block inside `ttn.js`. The decoder must stay one strict-ES5 file with no
  `require()` (LNS sandboxes), so the generator rewrites the region between
  `// BEGIN sensor_types` / `// END sensor_types` markers.
- `sensor_types.json` for the Manager-App: names, units and scales for UI rendering and
  rule editing.
- A pytest check (same pattern as the configen sync test) that fails CI when any
  generated output is out of date.

`kind` is `threshold | state | rate`. `momentary: true` marks PIR/accel-style sources that
only assert. That behaviour is hard-coded today in `source_is_momentary()` and
`rule_state_shape_valid()`. `liveness: true` marks the channel the no-data watchdog
watches; it replaces the hand-written `m_nodata_tab`.

## Firmware changes

### Readings → channels

`struct app_w1_slot_reading` and the ad-hoc on-board fields in `struct app_sensor_data`
become one per-source channel vector:

```c
struct app_sensor_channels {
	uint8_t type;                       /* sensor type id, 0 = none */
	uint32_t valid;                     /* bit ch = value present */
	float v[APP_SENSOR_CH_MAX];         /* physical units; state = 0/1, count as float */
};
```

- Drivers fill channels by number (`machine_probe_read()` also reads the TMP112 into ch2).
  A sub-sensor that fails to respond leaves its bit clear, like today's NaN.
- `read_threshold_value()`, `read_poll_state()` and `read_counter()` collapse into one
  `app_sensor_get(source, ch, &value)`.
- Counters stay `uint32_t` at the source. The vector is only the alarm/read view; the
  rate evaluator keeps its exact integer delta.
- Callers of the old fields need to move to the channel view: `app_compose.c`,
  `app_ats.c` (`sN-<quantity>` names become `sN-<channel name>`), `app_history.c` and
  the `sensor` / `w1` shell.

### Expected type per 1-Wire slot

- New config keys `sensor1_type..sensor4_type` (enum of the 1-Wire type ids).
  `teach` / `assign` set the key from the detected family. Provisioning can set it
  before the probe is plugged in.
- Rebind uses the expected type. The binding rules below are the proposed behaviour:
  - A bound ROM whose family does not match `sensorN_type` puts the slot in
    **mismatch**: no channel is valid, rules on the slot are inert, and a mismatch
    error is raised.
  - Auto-enroll only fills a free slot whose `sensorN_type` is the detected type or
    still empty.
- Errors are reported two ways:
  - a slot state in `Info` (next to `w1_slot_type`),
  - an `AlarmEvent` with a new `Type` value `TYPE_SENSOR_MISMATCH = 5` from the same
    watchdog path as `TYPE_NO_DATA`. Both activate and deactivate edges are sent.

### Rules

- The 17 B `alarm_N` blob stays the same size. Byte `[2]` becomes **channel** (was
  quantity). No migration; the release is non-migratable.
- `app_alarm_rule_valid(source, ch)` checks the channel against the source's type table.
  - A rule on a 1-Wire slot needs `sensorN_type` set, otherwise `-EINVAL`. The order is:
    provision the type, then the rules.
  - Kind comes from the descriptor, replacing `app_alarm_quantity_kind()`.
  - The momentary edge-only constraint also comes from the descriptor.
- `alarm_scale()` becomes a lookup of the descriptor `scale`.
- Shell: `alarm set <rule> <source> <channel-name|number> ...` with names from the
  registry. `sensor types` lists the tables.

## Wire changes (breaking)

| Message | Change |
|---|---|
| `AlarmEvent` | `quantity = 6` → `reserved 6`; new `uint32 channel = 10`, `optional uint32 sensor_type = 11` |
| `Info.AlarmStatus` | `quantity = 2` → `reserved 2`; new `uint32 channel = 4`, `optional uint32 sensor_type = 5` |
| `AlarmEvent.Type` | new `TYPE_SENSOR_MISMATCH = 5` |
| `Info` | per-slot state (ok / absent / replaced / mismatch) |

- `sensor_type` is sent **only for 1-Wire slot sources**. The decoder maps a built-in
  source to its fixed type, so on-board and discrete events stay as small as today. This
  matters for the 11 B tier (US915 DR0 / AU915 DR2) found in the uplink split audit.
- The decoder is stateless (#425 constraint): it resolves `(source, sensor_type?, channel)`
  to name, quantity, unit and scale from its generated table. No per-device state is
  needed.

**Telemetry `SensorReading` (field 27)** already carries `slot` and `type`. It has
fixed per-quantity fields (3..10), so a second temperature needs a new field. There are
two options:

- **(a)** Keep the typed fields and add fields per new channel. This is simple, but each
  new sensor edits the proto again.
- **(b)** `repeated sint32 value = 11 [packed]`, indexed by channel and scaled by the
  descriptor, with a sentinel for "absent". This is fully generic and the decoder uses the
  same table.

Recommendation: **(b)**. It is the same idea as the alarms, and the release is breaking
anyway. This is decision D2 below.

**History** has a fixed channel enum (`APP_HISTORY_S1_TEMP`, `APP_HISTORY_S1_HUM`, …).
Making it per-channel is out of scope for this plan and tracked as decision D3.
History keeps recording today's channels through the channel view.

## Delivery (PR sequence)

1. **Registry + generator.** `sensor_types.yml`, `west sensorgen`, generated C/JS/JSON,
   CI sync test. No behaviour change.
2. **Readings → channels.** `struct app_sensor_channels`, drivers fill channels,
   TMP112 + MPL3115A2 temperature become readable. Telemetry is still encoded from the
   channel view into the current fields.
3. **Expected slot type.** `sensorN_type`, mismatch state, Info slot state, the
   `TYPE_SENSOR_MISMATCH` watchdog.
4. **Rules + alarm wire.** Blob byte `[2]` = channel, validity/kind/scale from the
   registry, `AlarmEvent` / `AlarmStatus` changes, `ttn.js`, shell, ATS.
   - The Manager-App MR (rule editor from `sensor_types.json`) goes in parallel.
   - Server side: the ProXimos/Hub decoder needs the same table.
5. **Telemetry `SensorReading` generic values** (if D2 = b).
6. **History per channel** (if D3 is taken).

Each step lands with its own tests. `doc/` updates are the last commit of each PR.

## Verification

- `tests/alarm_rules`:
  - validity per type table,
  - slot rule rejected without `sensorN_type`,
  - channel out of range,
  - momentary edge-only rule from the descriptor.
- `tests/alarm_eval`:
  - two temperature channels on one machine-probe slot fire independently,
  - mismatch makes slot rules inert,
  - scale from the descriptor.
- `tests/compose`, `tests/cmd`: `AlarmEvent` / `AlarmStatus` encoding with and without
  `sensor_type`, and byte budget at the 11 B tier.
- `ttn.test.js`: decode of every generated `(type, channel)`, unknown type/channel falls
  back to `"t<type>c<ch>"`.
- pytest: generator output in sync, ids and channels append-only against the previous
  committed table.
- HIL: machine-probe with TMP112 populated, both temperature channels in a rule, and a
  swapped probe type on a taught slot raising mismatch.

## Decisions to take

| # | Question | Proposal |
|---|---|---|
| D1 | Built-in type ids: separate range (≥ 64) or one flat list | ≥ 64, keeps 1-Wire ids unchanged on the wire |
| D2 | Telemetry `SensorReading`: typed fields (a) or generic `repeated value` (b) | (b) |
| D3 | History per channel in this release | Defer; keep today's fixed channels |
| D4 | Mismatch reporting: new `TYPE_SENSOR_MISMATCH` alarm, Info state only, or both | Both |
| D5 | Does the Manager-App get the registry from the build (`sensor_types.json`) or from the device (a new read command) | Build artefact; the device only sends ids |
| D6 | Max channels per type (`APP_SENSOR_CH_MAX`, `valid` bitmask width) | 16 |
