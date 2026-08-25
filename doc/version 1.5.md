# HARDWARIO STICKER — Firmware v1.5.0 — What's New

This document lists **only the changes introduced in firmware v1.5.0** relative to the v1.4.0 series. Existing v1.4.0 behaviour is unchanged unless noted.

---

## Overview of changes

| Area | Change |
|---|---|
| Buzzer | **New** — alarm-driven melodies (#397, Phase 2 of #338): the buzzer HW variant now sounds automatically while any alarm is active, gated on a new global `alarm-buzzer-mode` config key |
| Debug builds | **New** — 8 independently Kconfig-toggleable subsystems (#395): `debug.conf` ships a lean default (W1, accelerometer, buzzer, PIR off) with real flash/RAM headroom instead of a maximally-squeezed image; `CONFIG_RADIO_LORAWAN=n` disables all radio for bench work. Release builds unaffected. |
| LED | **New** — HW-PWM-backed LED primitives (#301): `app_led_fade()` / `app_led_heartbeat()` and a runtime idle-indicator config, exposed via debug-build shell (`ats led fade\|heartbeat\|idle`). Not wired into any automatic runtime path yet — the LoRaWAN-off idle blink is unchanged. |

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

## 3. LED PWM primitives (#301)

Red (PA5/TIM2_CH1) and green (PA6/TIM16_CH1) status LEDs gained a hardware-PWM
path (`app.overlay`'s new `pwmleds` node) alongside their existing plain-GPIO
control, so they can be dimmed and smoothly faded instead of a hard on/off
blink. Yellow (PA4) has no timer channel and stays GPIO-only. "On" now drives
the PWM at a ~20% duty (`LED_DIM_PERCENT`) instead of full brightness — about
1/5 the LED current while staying clearly visible.

New `app_led` primitives (`app_led.h`):

| Function | Behaviour |
|---|---|
| `app_led_fade(channel, from%, to%, duration_ms)` | Smooth PWM ramp (~5 ms step), red/green only, blocking |
| `app_led_heartbeat(channel)` | One pulse: 0→100% over 80 ms, 100→0% over 120 ms (200 ms total) |
| `app_led_idle_config()` / `_get()` / `_pulse()` | Runtime-only (not persisted) style knob for a periodic indicator: `off` / `gpio` (short blink) / `pwm` (heartbeat), any colour |

Debug-build shell (`ats led fade|heartbeat|idle`) exercises all three directly
on hardware, for comparing visual behaviour and power live.

**Deliberately not wired into the app.** The original goal (#301) was to drive
the LoRaWAN-off idle indicator with the PWM heartbeat by default, once per
3 s. A review of the software fade implementation found a concrete power risk
before that could ship: `app_led_fade()`'s 5 ms step granularity is *shorter*
than this SoC's own ~9 ms Stop-mode wake cost (`power-consumption.md` §5,
`stm32_clock_control_init` re-running on every Stop0/1/2 exit) — so a periodic
200 ms/3 s heartbeat would likely keep the MCU out of deep sleep for most of
every pulse, an estimated 100+ µA average adder on top of this board's
~74–119 µA measured idle floor (`power-consumption.md` §1). That is exactly
the risk #301 flagged as the merge blocker, and it remains unmeasured on real
hardware.

Rather than ship the periodic path unvalidated, this PR ships **only the
primitives**: the LoRaWAN-off branch keeps its original single yellow GPIO
blink, unchanged. The functions stay available for a future event-driven
caller (an NFC tap, an alarm transition — moments the device is already awake
for other reasons) to invoke on demand, paying the pulse cost only when that
event actually happens rather than on a fixed cadence while otherwise idle.
This closes out #301's own documented fallback option ("restrict PWM to
interactive moments... keep a plain GPIO blink for the idle heartbeat").
Picking a first event-driven caller, and/or doing the PPK2 measurement to
settle whether a periodic heartbeat is viable after all, is unscheduled
future work — not tracked by an open issue for now.

---

*Applies to firmware v1.5.0. Reflects changes relative to v1.4.0 — see `doc/version 1.4.md` for the full v1.4.0 feature set this builds on.*
