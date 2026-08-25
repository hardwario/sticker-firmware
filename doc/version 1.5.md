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

*Applies to firmware v1.5.0. Reflects changes relative to v1.4.0 — see `doc/version 1.4.md` for the full v1.4.0 feature set this builds on.*
