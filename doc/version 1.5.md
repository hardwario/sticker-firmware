# HARDWARIO STICKER — Firmware v1.5.0 — What's New

This document lists **only the changes introduced in firmware v1.5.0** relative to the v1.4.0 series. Existing v1.4.0 behaviour is unchanged unless noted.

---

## Overview of changes

| Area | Change |
|---|---|
| Buzzer | **New** — alarm-driven melodies (#397, Phase 2 of #338): the buzzer HW variant now sounds automatically while any alarm is active, gated on a new global `alarm-buzzer-mode` config key |
| Debug builds | **New** — 8 independently Kconfig-toggleable subsystems (#395): `debug.conf` ships a lean default (W1, accelerometer, buzzer, PIR off) with real flash/RAM headroom instead of a maximally-squeezed image; `CONFIG_RADIO_LORAWAN=n` disables all radio for bench work. Release builds unaffected. |
| Analog inputs | **New** — analog voltage measurement on the external inputs GP_A/GP_B (#396), gated by `cap-analog-a`/`cap-analog-b`, mutually exclusive with the existing digital `cap-input-a`/`cap-input-b` on the same pins. |

---

## 1. Buzzer alarm-driven melodies (#397)

Phase 1 (#338, v1.4.0 §7) delivered the buzzer HW variant's control surface — `cap-buzzer`, the melody engine, the `ats buzzer` shell, and the remote `buzzer_play` command — but left it a purely on-demand indicator: nothing in the firmware triggered a melody by itself. This adds exactly that: the buzzer now sounds automatically as a local, audible companion to the existing red alarm LED (v1.4.0 §16).

One new configuration parameter (`alarms` proto group):

| Shell key | Type | Default | Range | Description |
|---|---|---|---|---|
| `alarm-buzzer-mode` | enum | `off` | `off`/`once`/`slow`/`normal`/`fast`/`continuous`/`reserved6`/`reserved7` | Buzzer behavior while any alarm is active, ordered by intensity. **Requires `cap-buzzer`.** |

**Mode behavior.** Every non-`off` mode plays the alarm melody (`app_buzzer.c`'s `MELODY_TABLE[APP_BUZZER_KIND_ALARM]` — five quick beeps, ~0.9 s) **immediately whenever a new alarm activates** — including while another alarm is already sounding (a second, different alarm firing mid-repeat replays the melody right away and restarts the repeat cycle). The modes differ only in how often the melody replays while at least one alarm stays active:

| Mode | Wire value | On a newly activated alarm | Replay while any alarm stays active |
|---|:-:|---|---|
| `off` | 0 | silent | — |
| `once` | 1 | melody, once | no replay |
| `slow` | 2 | melody | every **120 s** |
| `normal` | 3 | melody | every **30 s** |
| `fast` | 4 | melody | every **10 s** |
| `continuous` | 5 | melody | back-to-back (`repeat_s = 1`; with the melody's ~0.9 s length and the engine's 0.5 s cooldown the real rhythm is one melody every ~2.4 s) |
| `reserved6`/`reserved7` | 6/7 | reserved for future variants — currently behave like `normal` | every 30 s |

The moment the **last** active alarm clears, the buzzer silences immediately (no fade-out, no waiting for the current repeat cycle to finish) — in every mode. An alarm clearing while others remain active changes nothing audible. Reserving the full 0–7 wire range as named values now is deliberate: a later release can give `reserved6`/`reserved7` real behavior without a breaking config/proto change.

**What counts as an alarm / a new alarm:** `app_alarm_poll()` (the same evaluation pass that drives the red alarm LED, v1.4.0 §16) now assembles a per-alarm active bitmask — all 16 rule slots (threshold / state / rate), each no-data watchdog entry, and the low-battery watchdog (v1.4.0 §3) — and edge-detects individual bits centrally, so "a new alarm" means a specific rule slot or watchdog going active, not just the aggregate flipping. The same edge is exposed firmware-wide via two new read-only `app_alarm.h` accessors: `app_alarm_active_mask()` (the latest poll's per-alarm bitmask, slot bits 0–15 + watchdog bits above) and `app_alarm_activation_seq()` (a monotonic counter bumped once per poll that latched a new alarm — any number of independent readers detect "a new alarm fired since I last looked" by comparing against their saved copy). The buzzer is simply the first consumer. This is deliberately **not** wired through `app_alarm.c`'s per-source `app_alarm_event()` callback (used by the commissioning input-activity LED, v1.4.0 §16): that callback only ever fires for a raw hall/input/PIR/accel GPIO edge and would silently miss threshold, rate, no-data, and low-battery alarms. Since the poll runs on the main loop's 3 s cadence, "immediately" means within ≤3 s of the alarm latching; several alarms activating within one 3 s window produce a single replay.

**Global, not per-rule.** A per-rule "audible" flag was considered and rejected: every byte of the 17-byte packed `alarm_N` rule slot (v1.4.0 §7) is already in use, so adding one would be a breaking change to the wire format (Manager-App + `ttn.js` both parse that struct). One melody for every alarm type/rule is enough for this phase; a per-rule or per-type mapping is a possible future refinement if a real deployment asks for it.

**Interaction with the remote `buzzer_play` command:** no special-case logic exists to arbitrate between an alarm-driven melody and a manually-triggered one (`ats buzzer play` / the remote `buzzer_play` command, v1.4.0 §7) — they share the same melody-engine queue, and its existing "newest request replaces one that hasn't started yet, and immediately wakes a thread that's mid-playback or waiting out a repeat interval" policy already gives the alarm the effective priority: an alarm activating always preempts an unrelated melody. The one asymmetric case is deactivation: since silencing also purges the queue, a remote melody that happened to be queued right as the last alarm clears is muted along with it — a low-probability, low-consequence trade-off, not worth extra bookkeeping to avoid.

**Power note:** on current (unreworked) HW, R10/R11 cap the buzzer to a quiet ~1–2 mA (v1.4.0 §7), so even the `continuous` mode is not a meaningful battery cost. This changes once a unit gets the R10/R11 rework for full ~80 dB volume (still outstanding, tracked in #338) — worth revisiting the default and the mode cadences at that point, `continuous` especially.

**Test coverage:** a new `tests/buzzer` native_sim suite exercises `app_buzzer.c` directly against a `gpio_emul`-backed fake GPIO (melody sequencing, the abort-ordering regression, queue-replace policy, stale-request discard, and the `buzzer_play` id-validation bounds) — the first direct coverage of the melody engine, previously only exercised indirectly through a stub in `tests/cmd`. `tests/alarm_eval` gained seven new cases covering this feature's own plumbing: `cap-buzzer` gating, `alarm-buzzer-mode` gating, the activation/deactivation edges, the per-mode replay-interval table (including the reserved modes' `normal` fallback), that a poll with no state change never re-triggers a melody mid-playback, and that a **new** alarm activating while another is already active replays the melody (while a partial deactivation stays silent).

---

## 2. Debug build: independently-toggleable subsystems (#395)

`debug.conf` (RTT log + shell, `CONFIG_PM=n`) used to compile in every optional subsystem unconditionally, alongside the app's own always-on code — by the end of v1.4.0 that left only ~20–170 B of "free" flash/RAM per the linker's own report (98.48% flash / 99.92% RAM). That margin turned out to be unsafe in practice, not just tight: a system-heap write past the end of RAM crashed real hardware at exactly that margin (issue #394).

Eight subsystems are now independently toggleable via Kconfig — Release (`prj.conf`) is untouched: every one of them defaults to `y` there, and behavior is unchanged (verified byte-identical release build):

| Toggle | Flash saved | RAM saved | What it drops | Default in `debug.conf` |
|---|---:|---:|---|:-:|
| `CONFIG_RADIO_LORAWAN=n` | ~41.0 KB | ~15.1 KB | LoRaMac stack + radio HAL + `app_lrw.c` — disables **all** radio transmission (telemetry/alarm sampling and history capture keep running locally, just never sent) | **ON** |
| `CONFIG_W1=n` | ~20.3 KB | ~0.5 KB | 1-Wire bus: DS18B20, DS28E17 machine-probe bridge, ROM-bound slot registry | OFF |
| `CONFIG_LIS2DH=n` | ~7.6 KB | ~0.3 KB | Accelerometer (orientation, motion, free-fall) | OFF |
| `CONFIG_APP_BUZZER=n` | ~2.3 KB | ~0.8 KB | Buzzer/melody HW variant (#338/#397) + its shell/remote-command surface | OFF |
| `CONFIG_APP_PYQ1648=n` | ~1.0 KB | ~2.3 KB | PIR motion sensor | OFF |
| `CONFIG_SHT4X=n` | ~2.1 KB | ~0.1 KB | Onboard temperature/humidity sensor | on |
| `CONFIG_APP_CALIBRATION=n` | ~2.0 KB | ~0.3 KB | Magnet-triggered factory calibration mode | on |
| `CONFIG_OPT3001=n` | ~0.75 KB | ~0 B | Ambient light sensor | on |

The four OFF-by-default toggles (W1, LIS2DH, buzzer, PIR) were picked for the best savings-to-"remember to re-enable" ratio; the other four stay on since their individual savings are small. New default baseline: **85.5% flash / 93.7% RAM** (was 98.5%/99.9%). Re-enable any one for a bench session with `-DCONFIG_X=y` on the `west build` command line, no file edit needed.

**`CONFIG_RADIO_LORAWAN`** (app-level, wraps the underlying `CONFIG_LORAWAN` via `select ... if LORA`) is the single biggest lever but stays on by default — turning off all radio is a materially bigger behavioral change than any sensor toggle. `-DCONFIG_RADIO_LORAWAN=n` on top of the default gives a radio-free bench profile: **68.4% flash / 70.2% RAM**. A paired `CONFIG_RADIO_P2P` placeholder exists for the future raw-LoRa P2P transport (#118) but has no effect yet — nothing selects or depends on it until that work merges.

**Remote commands respect these toggles too**, not just local builds: `enter_calibration` (LoRaWAN/NFC) and `buzzer_play` (LoRaWAN/NFC/vendor) both report `NOT_SUPPORTED` instead of silently no-op'ing (or, in `enter_calibration`'s case, persisting a flag nothing would ever consume) when their subsystem is compiled out.

**Not independently toggleable** (deliberately out of scope, see issue #395): `CONFIG_DS28E17=n` alone (use `CONFIG_W1=n`, which drops both together — `app_w1_slots.c`'s sensor-type registry references the machine-probe API unconditionally); NFC, the alarm engine itself, hall/input counters, and history (each referenced from many more call sites than a single flag away); `CONFIG_ADC=n` / battery voltage (near-universally wanted in every telemetry frame).

---

## 3. Analog voltage measurement on external inputs GP_A/GP_B (#396)

The external inputs GP_A (PB4/ADC1_IN3) and GP_B (PA11/ADC1_IN7) — previously digital-only via `cap-input-a`/`cap-input-b` — can now instead be read as an analog voltage. A 33k/1k resistor divider on the front-end (series 33k, shunt 1k to GND) scales the 0–24 V input at the connector down into the pin's 0..VDD (3.3 V, TPS7A2033 LDO) ADC range; the firmware reports the **input-referred** voltage — `V_in = V_pin × (33+1)/1 = V_pin × 34`, mirroring `app_battery.c`'s 560k/100k correction — 12-bit. Signals beyond 0–24 V (e.g. 4–20 mA loop shunts) need external conditioning. The hall pins (PA3/PA7) are not ADC-capable on the WLE5 and stay digital-only.

Two new configuration parameters (`sensors` proto group):

| Shell key | Type | Default | Description |
|---|---|---|---|
| `cap-analog-a` | bool | `false` | Enable analog voltage measurement on GP_A. |
| `cap-analog-b` | bool | `false` | Enable analog voltage measurement on GP_B. |

**Pin sharing (#90).** GP_A/GP_B are also wired (via solder jumpers S1/S2 + series resistors) to the PIR PYD-1698 and to the buzzer HW variant (#338); the ADC pins park in analog mode (the lowest-power pin state) whenever nothing digital claims them. `app_sensor_init()` resolves a device with more than one of these capabilities enabled at once, in this priority order, same "the more established capability keeps the pin" logic that already applied between PIR and the buzzer:

1. `cap-pir-detector` / `cap-buzzer` — win over everything else on both pins.
2. `cap-input-a` / `cap-input-b` (digital) — win over the corresponding analog capability on the same pin.
3. `cap-analog-a` / `cap-analog-b` — only take effect when nothing above claims that pin.

A capability that loses a conflict is cleared at runtime (not persisted) and logged (`LOG_WRN`), so `GetConfig`/telemetry never show a capability as "enabled" while it is silently non-functional.

**Telemetry** (`Telemetry` proto, new group, always sent whole when its capability is on — sentinel on a failed/absent reading, same policy as the other analog scalars):

| Field | Wire | Unit |
|---|:-:|---|
| `input_a_voltage` | 28 | mV |
| `input_b_voltage` | 29 | mV |

Both are `uint32` mV on the wire (clamped 0..65534; `TM_U32_NA` = `0xFFFFFFFF` for a failed/absent reading → `null` in the decoder). They ride the periodic fPort-2 telemetry uplink **and** the synchronous `Sample`-command response over NFC (`app_compose_snapshot()` → `fill_telemetry()`, the same encoder), and are printed by `ats sensors sample` (`input-a-voltage` / `input-b-voltage`).

Example payload — 1.000 V on GP_A, 2.000 V on GP_B encode (protobuf, tag carries the field number) as the byte run:

```
… e0 01 e8 07   e8 01 d0 0f …
  ├──┬──┤├──┬─┤  ├──┬──┤├──┬─┤
  fld 28 1000mV  fld 29 2000mV   (value = mV as a varint)
```

which `ttn.js decodeUplink({fPort: 2})` decodes to `{ input_a_voltage: 1.0, input_b_voltage: 2.0 }` (mV ÷ 1000 → V). HIL-captured frames matched the PPK2 source within ADC quantization (see below).

**History:** two new selectable channels, `input-a-voltage` / `input-b-voltage` (uint16 LE mV, sentinel `0xFFFF`), appended after `accel-motion` in `history-sensors` — existing channel bit positions are unchanged.

**Alarms:** `APP_ALARM_Q_VOLTAGE` (already used for the low-battery watchdog) is now also a valid quantity for `APP_ALARM_SRC_INPUT_A`/`APP_ALARM_SRC_INPUT_B`, alongside the existing digital `state`/`count` quantities — a threshold rule (lo/hi/dwell) can be provisioned on either input regardless of which capability (digital or analog) is currently active on it.

**Power:** one one-shot ADC read per `interval_sample`, same resume→read→suspend pattern as the existing battery measurement — the issue's own estimate is idle impact ~zero, not yet independently re-measured for this change (HIL still pending, see below).

**HIL verification (done).** A PPK2 driven as a controlled source on the input connector (before the divider) confirmed the full chain: the divider correction is exact (`V_report = V_pin × 34` at every measured point), and the reported voltage tracks the applied source with a ratio ≈ 1.00–1.02 across the 1.0–5.0 V PPK2 range — within resistor tolerance plus ADC quantization (~34 mV/LSB referred to the input, i.e. coarse at the low end, inherent to the ×34 attenuation). Propagation was verified end-to-end: telemetry (`ats lrw compose` → ttn.js-decoded `input_a/b_voltage`), history (`history read` of the `input-a/b-voltage` channels), and a threshold alarm on `input-b voltage` (active below the band, recovery inside it). No-data-watchdog coverage for a failed/absent analog reading was deliberately left out of scope, matching the existing gap for other non-monitored analog sensors (e.g. the light sensor).

**Still pending:** confirming the idle-power estimate on real hardware (needs the J-Link detached and a current meter on the battery rail — not covered by the source-injection test above).

**Test coverage:** `tests/alarm_rules` (voltage rule on `INPUT_A`/`INPUT_B` accepted, existing digital validity unchanged), `tests/compose` (capability gating + mV scaling + sentinel), `tests/history` (channel present/absent), decoder (`ttn.test.js`, telemetry + history decode + sentinels), and the `configen` round-trip (`app_config.yml` ↔ generated `app_config.{c,h,proto}`).

---

*Applies to firmware v1.5.0. Reflects changes relative to v1.4.0 — see `doc/version 1.4.md` for the full v1.4.0 feature set this builds on.*
