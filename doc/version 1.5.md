# HARDWARIO STICKER — Firmware v1.5.0 — What's New

This document lists **only the changes introduced in firmware v1.5.0** relative to the v1.4.0 series. Existing v1.4.0 behaviour is unchanged unless noted.

---

## Overview of changes

| Area | Change |
|---|---|
| Buzzer | **New** — alarm-driven melodies (#397, Phase 2 of #338): the buzzer HW variant now sounds automatically while any alarm is active, gated on a new global `alarm-buzzer-mode` config key |

---

## 1. Buzzer alarm-driven melodies (#397)

Phase 1 (#338, v1.4.0 §7) delivered the buzzer HW variant's control surface — `cap-buzzer`, the melody engine, the `ats buzzer` shell, and the remote `buzzer_play` command — but left it a purely on-demand indicator: nothing in the firmware triggered a melody by itself. This adds exactly that: the buzzer now sounds automatically as a local, audible companion to the existing red alarm LED (v1.4.0 §16).

One new configuration parameter (`alarms` proto group):

| Shell key | Type | Default | Range | Description |
|---|---|---|---|---|
| `alarm-buzzer-mode` | enum | `off` | `off`/`on`/`continuous`/`brief`/`slow`/`frequent`/`on-new-alarm`/`reserved7` | Buzzer behavior while any alarm is active. **Requires `cap-buzzer`.** |

Only `off` and `on` do anything today; `continuous`/`brief`/`slow`/`frequent`/`on-new-alarm`/`reserved7` are wire-reserved placeholders for future timing variants (see below) and **currently behave identically to `on`** — a device set to e.g. `continuous` will beep exactly like `on` until a future firmware release gives that mode its own behavior. This is a deliberate wire-format choice: reserving the full 0–7 range as named values now means a later release can implement one of them without a breaking config/proto change.

**`on` behavior:** while any alarm is active, play the same fast-beep melody the alarm LED uses internally (`app_buzzer.c`'s `MELODY_TABLE[APP_BUZZER_KIND_ALARM]`, five quick beeps) and repeat it every **30 s** for as long as at least one alarm stays active. The moment the last active alarm clears, the buzzer silences immediately (no fade-out, no waiting for the current repeat cycle to finish).

**What counts as "any alarm active":** the same aggregate `app_alarm_poll()` already computes for the red alarm LED (v1.4.0 §16) — every rule kind (threshold / state / rate) across all 16 rule slots, **plus** the no-data watchdog and the low-battery watchdog (v1.4.0 §3). This is deliberately **not** wired through `app_alarm.c`'s per-source `app_alarm_event()` callback (used by the commissioning input-activity LED, v1.4.0 §16): that callback only ever fires for a raw hall/input/PIR/accel GPIO edge and would silently miss threshold, rate, no-data, and low-battery alarms — most of what "any alarm active" needs to mean here.

**Global, not per-rule.** A per-rule "audible" flag was considered and rejected: every byte of the 17-byte packed `alarm_N` rule slot (v1.4.0 §7) is already in use, so adding one would be a breaking change to the wire format (Manager-App + `ttn.js` both parse that struct). One melody for every alarm type/rule is enough for this phase; a per-rule or per-type mapping is a possible future refinement if a real deployment asks for it.

**Interaction with the remote `buzzer_play` command:** no special-case logic exists to arbitrate between an alarm-driven melody and a manually-triggered one (`ats buzzer play` / the remote `buzzer_play` command, v1.4.0 §7) — they share the same melody-engine queue, and its existing "newest request replaces one that hasn't started yet, and immediately wakes a thread that's mid-playback or waiting out a repeat interval" policy already gives the alarm the effective priority: an alarm activating always preempts an unrelated melody. The one asymmetric case is deactivation: since silencing also purges the queue, a remote melody that happened to be queued right as the last alarm clears is muted along with it — a low-probability, low-consequence trade-off, not worth extra bookkeeping to avoid.

**Power note:** on current (unreworked) HW, R10/R11 cap the buzzer to a quiet ~1–2 mA (v1.4.0 §7), so a repeating 30 s alarm melody is not a meaningful battery cost. This changes once a unit gets the R10/R11 rework for full ~80 dB volume (still outstanding, tracked in #338) — worth revisiting the default and the repeat cadence at that point.

**Test coverage:** a new `tests/buzzer` native_sim suite exercises `app_buzzer.c` directly against a `gpio_emul`-backed fake GPIO (melody sequencing, the abort-ordering regression, queue-replace policy, stale-request discard, and the `buzzer_play` id-validation bounds) — the first direct coverage of the melody engine, previously only exercised indirectly through a stub in `tests/cmd`. `tests/alarm_eval` gained five new cases covering this feature's own plumbing (`cap-buzzer` gating, `alarm-buzzer-mode` gating, the activation/deactivation edges, and that a poll with no state change never re-triggers a melody mid-playback).

---

*Applies to firmware v1.5.0. Reflects changes relative to v1.4.0 — see `doc/version 1.4.md` for the full v1.4.0 feature set this builds on.*
