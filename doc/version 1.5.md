# HARDWARIO STICKER — Firmware v1.5.0 — What's New

This document lists **only the changes introduced in firmware v1.5.0** relative to the v1.4.0 series. Existing v1.4.0 behaviour is unchanged unless noted.

---

## Overview of changes

| Area | Change |
|---|---|
| Buzzer | **New** — alarm-driven melodies (#397, Phase 2 of #338): the buzzer HW variant now sounds automatically while any alarm is active, gated on a new global `alarm-buzzer-mode` config key |
| Debug builds | **New** — 8 independently Kconfig-toggleable subsystems (#395): `debug.conf` ships a lean default (W1, accelerometer, buzzer, PIR off) with real flash/RAM headroom instead of a maximally-squeezed image; `CONFIG_RADIO_LORAWAN=n` disables all radio for bench work. Release builds unaffected. |
| Radio: P2P | **New** — the raw-LoRa point-to-point transport is complete on the node (#118): `radio-mode p2p` pairs with a Proximos `Control.radio.P2P` central over a FIBER modem, with an acknowledged data plane, downlink commands, network-initiated pairing control, per-node TX power, and strict EU868 duty compliance. LoRaWAN is unaffected — both stacks link into the same image and the choice is made at boot. |
| LED | **New** — HW-PWM-backed LED primitives (#301): `app_led_fade()` / `app_led_heartbeat()` and a runtime idle-indicator config, exposed via debug-build shell (`ats led fade\|heartbeat\|idle`). The boot carousel now fades red/green (yellow unchanged); the LoRaWAN-off idle blink is unchanged (unvalidated power cost, see §3). |
| LoRaWAN | **New** — autonomous settings-info uplink after boot (#412): right after the join `Info`, the device pushes a one-page `ConfigDump` on fPort 85 with its key operating settings + detected 1-Wire slot types, so the network learns the effective config without polling. |
| LoRaWAN | **Fixed** — LoRaWAN glue in the Zephyr fork (`sticker-zephyr` `v4.3.0-sticker2-branch`, #421): a (re)join no longer returns the stale result of an earlier link-check / device-time confirm (L-7, #241); MAC-confirm waits are bounded (`-ETIMEDOUT` instead of a wedged `m_work_q`, #181); all LoRaMac access is serialised by one MAC lock (#241). |
| LoRaWAN | **Fix** — region guard (#409 A1): a stored `lrw-region` that is not compiled into the image no longer kills LoRaWAN init silently — the radio stays silent (never falls back to another band), reported as `lrw_disabled` plus an error log. |
| LoRaWAN | **New** — manual uplink datarate `lrw-datarate` (#409 A3): `auto` (default) or `dr0`–`dr7`, pinned after every join when ADR is off. |
| LoRaWAN | **Fix** — low-DR delivery (#409 A5a, part 1): compact LoRaWAN `Error` so a command is always answered at the 11 B tier; MAC-flood (budget 0) no longer drops responses/alarms; alarm batches split across frames; alarm state mirrored into telemetry `system_flags`. |
| LoRaWAN | **Fix** — `DevStatusReq` right after `LinkADRReq` is now answered (#419), via a `loramac-node` patch applied with `west patch apply`. |
| LoRaWAN | **New** — AS923 region (#409 A6): `lrw-region as923`, channel plan AS923-1, release builds. |
| LoRaWAN | **Improved** — faster link-loss recovery (#424): link check on every report while `WARNING`, a TX-power/data-rate step-down ladder before the rejoin (a moved device regains its gateway on a lower DR without losing the session), and US915/AU915 no longer lose the configured sub-band after repeated failed joins. |
| LoRaWAN / P2P | **New** — universal response paging (#425): every answer that does not fit one frame is split into self-contained pages numbered `page_index`/`page_count` in the `Response` envelope (and in `AlarmReport`), sent by the device on its own; decoders label them `pages: "i/N"`. |
| LoRaWAN / NFC | **New** — `GetSettings` command (#428): the boot settings-info `ConfigDump` (§4) on request, with the command's `seq`, so a host can refresh the key operating settings without a full multi-page `GetConfig`. |
| LoRaWAN | **Fix** — command answers from the ProXimos Nodes test (#432): the deferred `clock_sync` Info now carries the command's `seq`; `force_send` / `sample` leave at once (no fleet jitter, no silent merge into a pending report); `w1_scan` without 1-Wire answers `NOT_SUPPORTED`. |
| LoRaWAN | **Changed** — `GetConfig` over LoRaWAN leaves out the 1-Wire slot ROMs `sensor1_rom`..`sensor4_rom` (#433): 4 pages instead of 6 at EU868 DR0; NFC/shell GetConfig and an explicit `GetParam` still return them. |
| LoRaWAN / NFC | **New** — `SetParam.alarms_replace` (#434): one message rewrites the whole alarm table — all rule slots are emptied before the message's `alarms` group is applied (or all cleared without one), rolled back with the batch on a fault. |
| NFC | **Changed (breaking)** — all interactive NFC commands (`GetInfo` / `GetConfig` / `SetParam` / vendor) move from NDEF records to the **ST25DV Fast-Transfer-Mode mailbox** (#313): one tap, phone held still, iOS at parity with Android. The tag now holds **no NDEF record at all** — even the identity record is gone; the phone reads identity via the mailbox `get_basic_info` command. Battery-less configuration is dropped; claiming moves to a powered device (see also PR #415). See §18. |
| NFC claiming | **Changed (breaking for provisioning)** — new unauthenticated `plain_text` command transport with a compile-time allow-list, first command `get_claim_info` (#415); the claim window becomes an explicit two-state latch (`active`/`done`) with the auto-arm and the implicit close removed; commands `clm_ack`/`clm_rearm` renamed to `claim_done`/`claim_active` (same wire ids 25/27). See §19. |
| NFC | **New** — last-downlink RSSI / SNR and their age in the NFC `GetInfo` (#409 A2), so an installer with a phone can judge the link at the mounting spot. |
| History | **Fix** — record timestamps follow the RTC (F27/F28, H-4): report cadence on wall-clock slots, no capture skipped during a replay, each flash page stamped from the RTC (a reboot / power loss / halt is a gap, not a shift), page header v2 keeps a clock-sync fix-up across reboots, the replay ends with the window's last frame. HistoryFrame protocol unchanged. See §21. |
| LoRaWAN | **Fix** — the M-2 stale-uplink watchdog no longer forces a rejoin while the duty cycle is refusing sends (F29): a rejoin reset the band credits and let the device exceed the 1 % limit. See §22. |

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
| `CONFIG_RADIO_LORAWAN=n` | ~41.0 KB | ~15.1 KB | LoRaMac stack + radio HAL + `app_radio_lrw.c` — disables **all** radio transmission (telemetry/alarm sampling and history capture keep running locally, just never sent) | **ON** |
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
blink, unchanged. This closes out #301's own documented fallback option
("restrict PWM to interactive moments... keep a plain GPIO blink for the idle
heartbeat"). Doing the PPK2 measurement to settle whether a periodic heartbeat
is viable after all is unscheduled future work — not tracked by an open issue
for now.

**First event-driven consumer: the boot carousel.** `main.c`'s
`play_carousel_boot()` — the one-shot red/yellow/green sequence played once
at boot, before the app's normal idle behaviour starts — now fades red and
green in/out (`app_led_fade`) instead of a hard on/off blink; yellow keeps its
plain GPIO blink (no timer channel). This is exactly the kind of caller the
power concern above doesn't apply to: it runs once per boot, not on a 3 s
idle cadence, and the CPU is already fully awake for NFC/radio bring-up during
that window regardless. Per-colour timing (500/250/500/250/1500 ms) is
unchanged from the previous hard-blink carousel, so the overall boot animation
length is identical — only the red/green transitions are now smooth. HW-
confirmed on the bench (J-Link EDU Mini 801053709, SN 2162165627): red and
green fade smoothly, yellow blinks as before.

---

## 4. Autonomous settings-info uplink after boot (#412)

Every boot, once the device joins, it announces itself with an autonomous
`GetInfo` uplink on fPort 85 (`on_join_success()` → `queue_info_uplink()`). The
network server, however, had no picture of the device's **configuration** unless
it actively polled with `GetParam` / `GetConfig` downlinks — so after any local
reconfiguration (shell / NFC), the LNS copy stayed stale until someone asked.

This adds a **second autonomous fPort-85 uplink right after the boot `Info`**: a
`Response.ConfigDump` (one frame when it fits, paged otherwise — §13) carrying a
fixed selection of the key operating settings. Because `settings save` cold-reboots
and every boot re-joins, this **re-announces the effective config automatically**
after every persisted change — no diff-tracking, no extra state.

**Contents:**

| Group | Fields |
|---|---|
| `application` | `interval_sample`, `interval_report`, `history_enable` |
| `sensors` | `cap_hall_left` … `cap_accelerometer` (all nine capability flags, emitted explicitly incl. `false`) |
| `w1_slot_type` | detected 1-Wire sensor type per logical slot 1..4 |

`w1_slot_type` (`ConfigDump` field 7, packed `repeated uint32`) reports what is
physically attached to each 1-Wire slot. Its values mirror the firmware's single
source of truth, `enum app_w1_slot_type` (`app_w1_slots.h`):

| Value | Meaning |
|:-:|---|
| 0 | empty |
| 1 | dallas (DS18B20) |
| 2 | machine-probe (DS28E17) |

Adding a new sensor family is a one-place change to that enum + the type registry
in `app_w1_slots.c`; the new value flows onto the wire automatically (the proto
stays a raw `uint32`, so no schema change). A decoder that predates a value renders
it as `type<N>` rather than failing.

**Decoded example** (`ttn.js` output as the LNS sees it, one frame; like every other
config reply, bool fields decode as `0`/`1`):

```json
{ "config_dump": {
    "application": { "interval_sample": 60, "interval_report": 900, "history_enable": 0 },
    "sensors": { "cap_hall_left": 1, "cap_hall_right": 0, "cap_input_a": 1,
                 "cap_input_b": 0, "cap_light_sensor": 1, "cap_barometer": 0,
                 "cap_pir_detector": 0, "cap_w1_sensors": 1, "cap_accelerometer": 0 },
    "w1_slot_type": ["machine-probe", "dallas", "empty", "empty"] } }
```

**Notes:**

- Size incl. the `APP_PROTO_VERSION` byte: 34 B without 1-Wire (`CONFIG_W1=n`, no
  field 7), 40 B with the four `w1_slot_type` entries, up to ~46 B with large
  interval values. It fits the EU868 DR0 budget (51 B) and the 64 B response buffer.
- **Low DR outside EU868 (#418, resolved by #409 / #425):** below the EU868 DR0
  budget the settings-info is paged (§13); a setting that does not fit even alone is
  left out, and when nothing fits the device sends it once a DR change makes room.
- The lean debug default (`debug.conf`, #395) builds with `CONFIG_W1=n`, so a
  debug image omits `w1_slot_type`. Build with `-DCONFIG_W1=y` to exercise it.
  Release builds have 1-Wire on.
- **Zero proto/decoder disruption** otherwise: `ConfigDump`,
  `app_config_fill_application()` / `fill_sensors()` (selected-ids fill), and the
  `ttn.js` `_decodeConfigDump()` already handle the config fields.
- The **persisted 1-Wire slot ROM serials** are *not* in this frame (to keep it one
  DR0 uplink); a host that wants them reads `GetParam(sensors 11..14)`.
- `w1_slot_type` is runtime state, filled **only** by this boot uplink and by its
  on-request twin `GetSettings` (§14) — a plain `GetConfig` / `GetParam` reply stays a
  pure config snapshot and never carries it.

**HW verification (2026-09-22, EU868, ChirpStack v4):** after every join the
device sent `Info` (FCnt 1), then this `ConfigDump` page 0/1 (FCnt 2), then
telemetry (FCnt 3), all at DR0. The dumped values matched `config show`, and a
`-DCONFIG_W1=y` debug image carried `w1_slot_type` = 4× `empty` (40 B). The
`dallas` / `machine-probe` values are covered only by the unit tests: the test
unit had no 1-Wire bridge. See `doc/manual-test-plan.md` scenario **L4b**.


---

## 5. LoRaWAN glue fixes in the Zephyr fork (#421)

The firmware now builds against `hardwario/sticker-zephyr` **`v4.3.0-sticker2-branch`** (Zephyr v4.3.0 +
the existing I2C PM fix + three LoRaWAN glue commits). The migration to Zephyr v4.4.2 is tracked in #420 and will
carry these commits over.

| Fix | Before | After |
|---|---|---|
| **Stale join result** (L-7, #241) | Every link-check / device-time MLME confirm left a token in the join semaphore, so the next (re)join returned right after TX with the *previous* result. A stale failure made the app drop a session the MAC had actually joined, then back off. | Only the join confirm signals the join waiter; the semaphore is drained before each join. |
| **Bounded confirm wait** (#181) | `lorawan_send()` / `lorawan_join()` waited forever for the MAC confirm; a lost confirm wedged `m_work_q` until the #182 watchdog reset the SoC. | `CONFIG_LORAWAN_CONFIRM_TIMEOUT_MS` (20 s, `BUILD_ASSERT` < the 30 s liveness window). A lost confirm returns `-ETIMEDOUT` and the normal bounded retry path takes over. |
| **MAC lock** (#241) | LoRaMac (not thread-safe) was entered from `m_work_q`, shell/NFC and the system work queue (timer + radio events) without a shared lock. | One recursive `lorawan_mac_lock()` around every LoRaMac entry, never held across a confirm wait. `app_radio_lrw.c` wraps its direct LoRaMac calls. |

Also fixed: `ats lrw status` / NFC info during the boot window before `lorawan_start()` no longer
reads LoRaMac's still-uninitialised crypto context (it showed a garbage FCntUp).

**Behaviour notes:**

- A send that times out may already have been on air (only the confirm was lost), so the bounded retry can
  send a duplicate uplink with a new FCnt. That is preferable to the previous permanent wedge.
- Cost: about 400–480 B flash, 64 B RAM.

**HW verification (2026-09-23, EU868, ChirpStack v4 on the ProXimos Hub):** T1–T8 PASS. The key results:
- **A/B:** `lorawan_join()` after link-check / device-time confirms returned after **26 ms** on v1.5.0 (stale
  result) versus **8305 ms** with #421 (the real JoinAccept).
- **Fault injection:** a dropped confirm returned `-ETIMEDOUT` after 20 s with no wedge and no watchdog reset.
- **Stress:** about 68 k locked MIB reads during chained downlinks, with no hang.
- **Rejoin:** after a network loss, the first rejoin once the network was back succeeded.

See `doc/manual-test-plan.md` **L17** and `doc/plan/421 - LoRaWAN glue fixes in sticker-zephyr.md`.


---

## 6. Raw-LoRa P2P transport (#118)

A second radio transport, selectable at boot with `config radio-mode p2p`, for
deployments with no LoRaWAN infrastructure: the STICKER talks directly to a
HARDWARIO FIBER acting as a modem, and a Proximos `Control.radio.P2P` central
behind it owns the network. The payload layer is unchanged — `app_compose`
builds the same protobuf snapshots and `app_report` owns the same
`interval_report` cadence — so telemetry, alarms and history behave as they do
over LoRaWAN. Full design in `doc/p2p.md`; the acceptance matrix is
`doc/p2p-e2e-test-plan.md`.

**Setup** is three commands and a save. `lrw_appkey` is the root of the whole
transport (there is no separate P2P key — the central already has it from
ordinary OTAA provisioning), and an all-zero one makes the radio refuse to
start rather than join under a publicly known key. `lrw_deveui` is the node's
on-air identity (#417): an all-zero DevEUI refuses a new join likewise:

```
config lrw-appkey <32 hex>
config radio-mode p2p
settings save                    # persists + reboots
ats radio status                 # kind: P2P, app_key: set, state: JOINING|PAIRED
config show                      # lrw-deveui: the DevEUI the central registers
```

The three radio parameters (`p2p-frequency`, `p2p-spreading-factor`,
`p2p-tx-power`) must match the Hub's and are shell-only by design — see
`doc/p2p.md` §2.

**What the node does:**

| Area | Behaviour |
|---|---|
| Pairing | On-air join handshake (JoinRequest/JoinAccept, 16 B AES-CMAC tags under `app_key`); the JoinRequest identifies the node by its DevEUI (8 B, MSB-first, #417 — the serial number is no longer on the P2P air). Fast retries for the 120 s boot window, then a slow backoff (≈ one attempt pass per hour) instead of falling silent; each pass sweeps the spreading factors nearest-first, so a node finds a Hub that moved the network SF. Session persisted to NVS so a power cycle never costs a re-join. `join` forces a fresh session; `ats radio unjoin` simulates a never-paired boot. |
| Data plane | AES-CCM under a derived `session_key`, 4 B tag, per-frame counter persisted with a reservation window so a reboot can never reuse a nonce. Confirmed uplinks with up to 3 retransmissions of the byte-identical frame. |
| Link quality | Each Ack carries the RSSI/SNR the central measured on that uplink, surfaced by `ats radio status`. |
| Clock | The Ack can carry a Unix-time tail, so a node with no RTC gets wall time from the central — no `clock_sync` command needed. |
| Downlink commands | `0x56` carries the same protobuf `Command` as LoRaWAN fPort 85, dispatched through the shared handler and answered with a `0x55`. Deferred actions (`settings_save`, `reboot`) execute only **after** that answer has been acknowledged, so a commanded reboot cannot swallow its own response. |
| RX window | The announcing Ack states the pending command's exact on-air length, so the receiver stays on for that frame instead of a 255 B worst case — 468 ms instead of 2434 ms for a short command at SF10. |
| Pairing control | The central can end a pairing (`Detach`) or ask for a rekey (`RejoinRequest`); both are authenticated and empty-bodied. A detached node goes quiet and stays quiet — no automatic re-join — until a reboot or an explicit `join`. |
| Radio assignment | JoinAccept can assign this node's TX power (2..22 dBm), applied and persisted with the pairing; `ats radio status` shows `assigned` versus `config`. Channel and SF stay network-wide: the modem has one receiver. |
| Duty cycle | Raw LoRa bypasses LoRaMac's enforcement, so the node keeps its own exact sliding-hour ledger: **every** rolling hour stays within EU868's 1 %, not merely the long-run average. |
| History replay | A central-requested history replay works over P2P too (B8): a device-driven stream of history frames, each acknowledged, with telemetry paused for the duration. |
| Self-healing | Eight consecutive fully-failed uplink cycles start a re-join with exponential backoff (60 s → 1 h), so a node survives a central DB restore or a long outage without a site visit. |

**Not in this release:** listen-before-talk (CAD) — the Zephyr LoRa driver API
has no CAD entry point yet, so it is a follow-up (`doc/plan/`); bulk history
region support beyond EU868; and NFC
configuration of the P2P radio parameters or an NFC `p2p_join` trigger, both of
which need a coordinated Manager-App release.

**Build note:** both radio stacks link into the same image, gated by
`CONFIG_RADIO_P2P` (default `y`) and `CONFIG_RADIO_LORAWAN`. The flash-tight
`debug.conf` overlay drops P2P; `debug.conf;debug_p2p_bench.conf` is the only
debug image containing it, and it pays for that by dropping LoRaWAN.

---

## 7. LoRaWAN region guard (#409 A1)

`lorawan_set_region()` returns `-ENOTSUP` for a region whose
`CONFIG_LORAMAC_REGION_*` is not compiled in. Until now that made `app_radio_lrw_init()`
fail, leaving a device with **no radio and no diagnosable state** — a real case,
because `debug.conf` trims US915/AU915, so a device configured for `us915` that is
flashed with a debug image (or any trimmed build) went dead.

Now `app_radio_lrw_init()` resolves the stored region against the regions in the image
first. If it is missing (or out of range):

- the radio stays **silent** through the existing radio-mode OFF path
  (`APP_RADIO_LRW_STATE_DISABLED`, no LoRaMac bring-up, join/send are no-ops);
- an error is logged: `lrw-region <n> is not compiled into this image: radio-silent`;
- over NFC the device reports `lrw_state` DISABLED and the existing `device_status`
  bit 12 `lrw_disabled` (no dedicated bit — `config show` shows the stored region).

There is **deliberately no fallback to another region**: a device configured for
US915 or AU915 must never transmit on 868 MHz (or vice versa). Fix by setting a
compiled-in `lrw-region` (NFC / shell) or flashing a full image.

Cost: a few dozen bytes of flash, +0 B RAM.


---

## 8. Manual uplink datarate `lrw-datarate` (#409 A3)

New config key, modelled on twr-sdk's `AT$DR`:

```
config lrw-adr false
config lrw-datarate dr3
settings save
```

| Value | Meaning |
|---|---|
| `auto` (default) | stack / ADR choose the DR — behaviour unchanged from v1.4.0 |
| `dr0` … `dr7` | pin the region's DRn for uplinks |

- Applied in `on_join_success()` on **every (re)join**, after ADR is configured and
  before the payload budget is captured, so the telemetry split follows the pinned DR.
  JoinRequests are not affected: they always go out at the region's default join DR
  (every (re)join re-initialises the MAC); the pin applies from the first uplink after
  the join.
- **Only with ADR off.** With `lrw-adr true` the value is ignored and a warning is
  logged (Zephyr's `lorawan_set_datarate()` refuses while ADR is on).
- DR validity is **region-dependent**: EU868 DR0–7, US915 DR0–4, AU915 DR2–6 with the
  default dwell time (DR0/DR1 have a 0-byte payload there). A DR the MAC rejects is
  logged as an error and the stack's own DR stays in use — the device keeps working.
- Calibration mode pins its own DR and ignores `lrw-datarate`.
- Writable over shell and NFC only (like the rest of the `lorawan` group, never over a
  LoRaWAN downlink); preserved across `device_reset`.

**Wire format:** `AppConfigMessage.Lorawan.datarate` (field 16), enum `Datarate`:
`AUTO = 0`, `DRn = n + 1` — the offset lets `auto` be the proto3 default. `ttn.js`
encodes the names (`"DR3"`) and decodes the raw value.

Cost: release +408 B flash, +0 B RAM.


---

## 9. Low-DR delivery, part 1 (#409 A5a)

At the smallest LoRaWAN budget tier — **11 B** on US915 DR0 and AU915 / AS923 DR2 — most
fPort 85 / fPort 3 messages cannot fit even one field. Policy: this tier is a *floor*
(telemetry, Ack, compact Error, Info-lite); full delivery targets ≥ 51 B.

- **Compact LoRaWAN `Error`.** Over LoRaWAN an `Error` carries `code` + `fault_field`
  only; the `detail` string is NFC-only (`ttn.js` defaults a missing `code` to 0 =
  UNKNOWN, which proto3 omits). The LoRaWAN "response too large" fallback is a 7 B
  `Error{ code = 9 BUDGET_TOO_SMALL }` — "retry once ADR raises the DR" — so a command
  that cannot be answered in full still gets an answer. NFC keeps `UNKNOWN` + detail.
- **Info at a small budget** is paged (§13) — the join / clock-sync `Info` and a
  LoRaWAN `GetInfo`. (An interim `InfoLite` message from the #409 draft was replaced by
  this before release; `Response` field 11 is reserved.)
- **Deferred boot announce.** If not even one field of the join `Info` or the #412
  settings-info fits, the device sends it by itself once a DR change makes room — no
  host poll needed.
- **History replay (`req_history`) at low DR.** Frames are sized with the real frame
  count instead of the worst-case varint, ~8 B more samples per frame (EU868 DR0: ~26 B
  instead of ~18 B). When records exist but not one fits the current DR, the answer is
  `Error BUDGET_TOO_SMALL` instead of `HISTORY_UNAVAILABLE`; a replay cut short by a DR
  drop ends with the same `Error` (request `seq`) instead of going silent.
- **GetConfig / GetParam over LoRaWAN send every page by themselves.** One downlink
  request is enough: the device answers with the requested page (0 unless `page` is
  given) and then uplinks the remaining pages on its own — same `seq`, numbered as in
  §13, paced by the duty cycle (at EU868 DR0 a full config takes minutes). A new paged
  request replaces a stream still running; a rejoin cancels it. The page size stays
  30 B. NFC is unchanged (the phone still asks page by page, ~450 B pages).
- **DR drop between queueing and sending.** A queued frame that no longer fits after
  ADR lowered the DR is recovered instead of dropped: the boot `Info` / settings-info
  are re-sent once the DR rises again, a command answer becomes `Error
  BUDGET_TOO_SMALL` with the command's `seq`, an alarm frame is dropped (its state is
  still in telemetry `system_flags`).
- **Budget 0 (MAC-command flood)** no longer drops a queued response or alarm: an empty
  uplink flushes the MAC answers and the payload is retried.
- **Alarm batches split** across as many `AlarmReport` frames as needed (same
  `base_time` / `total` in each) instead of trimming to the first frame. At the 11 B tier
  no `AlarmReport` fits; the frame is skipped and logged.
- **Alarm state in telemetry.** `Telemetry.system_flags` bits 1..8 now carry the
  `device_status` alarm byte (bit 0 is still `boot`), so the alarm state reaches the LNS in
  every telemetry frame, including at the 11 B tier. `ttn.js` adds `alarm_status` and
  `alarm_status_flags` (e.g. `["alarm_any", "alarm_threshold"]`). Additive — older decoders
  ignore the extra bits.

## 10. `DevStatusReq` after `LinkADRReq` answered (#419)

LoRaMac-node's MAC-command parser skipped a `DevStatusReq` that is the last FOpts byte
right after a `LinkADRReq` block — exactly how ChirpStack bundles them — so `DevStatusAns`
(battery, margin) was never sent. Not fixed upstream.

The fix is carried as a **Zephyr `west patch`** on the `loramac-node` module
(`zephyr/patches.yml`, `zephyr/patches/loramac-node/`):

```
west update
west patch apply      # re-run after every west update
```

CI applies it automatically. A LoRaWAN build **fails** when the patch is missing — CMake
checks for the `STICKER-419` marker at configure time and again on every build, because
`west update` resets the module and an incremental build (including the one `west flash`
runs) does not reconfigure. `-DSTICKER_ALLOW_UNPATCHED_MODULES=ON` overrides it for a
throwaway build. HW-verified 2026-09-23: after ChirpStack's `LinkADRReq + DevStatusReq` the
next uplink carries `DevStatusAns`, and ChirpStack shows the device's battery / margin. From a git worktree pass absolute paths:
`west patch apply -b <worktree>/zephyr/patches -l <worktree>/zephyr/patches.yml`.


---

## 11. AS923 region (#409 A6)

`lrw-region` accepts `as923` (wire value 3 in `AppConfigMessage.Lorawan.region`):

```
config lrw-region as923
settings save
```

- **Channel plan group AS923-1** (923.2 / 923.4 MHz default channels), the loramac-node
  default. The group is compile-time only (`REGION_AS923_DEFAULT_CHANNEL_PLAN`); other
  groups (AS923-2/-3/-4) would be separate build variants.
- **No sub-band** — `lrw-sub-band` applies to US915/AU915 only.
- **Dwell time on by default:** DR0/DR1 carry 0 B and DR2 carries 11 B, so AS923 at its
  lowest DR is the 11 B budget tier handled by §9 and §13 (paged answers, compact
  `Error`, alarm state in telemetry, deferred boot announce). `lrw-datarate dr0` / `dr1` are rejected by
  the MAC and logged; the stack's DR stays in use.
- **Release builds only.** `debug.conf` trims AS923 together with AU915/US915; a stored
  `as923` on a debug image leaves the radio silent (§7), never on another band.
- `ttn.js` encodes `region: "AS923"` in `set_param`.

Cost: release +2 536 B flash, +0 B RAM (loramac-node channel structures are already
sized for US915's 72 channels). Not tested on HW — the bench gateway is EU868 only.

**HW verification of #409 (2026-09-23, EU868, ChirpStack v4 on the ProXimos Hub, STICKER DevEUI `5876070000000413`):**
the region guard (§7), `lrw-datarate` (§8), compact `Error`, alarm split and alarm bits, GetConfig/GetParam page
streaming (§9), `DevStatusAns` (§10) and the release image with AS923 compiled in all PASS. Not HW-tested (no
US915/AU915/AS923 gateway): the 11 B budget tier of §9 and AS923 on air. See the HIL records in
`doc/plan/409 - LoRaWAN improvements - regions, datarate, diagnostics.md`.


---

## 12. Faster link-loss recovery (#424)

When the network disappears (gateway off, or the device moved out of reach of its ADR-optimised data rate), v1.5.0 recovers faster and, where possible, without a rejoin.

| | Before | After |
|---|---|---|
| Link check in `WARNING` | every `lrw-link-check-interval`-th report | **every report** (`lrw-link-check-interval 0` still disables link checks) |
| DR fallback | only through the OTAA rejoin (MAC reset to the join DR). LoRaMac's own ADR backoff needs 128 unanswered uplinks for its first step (~32 h at 900 s) | **Recovery ladder**: entering `WARNING` and every later failed check restore the default (max) TX power and drop the DR by one step. A check that succeeds on the lower DR returns to `HEALTHY` with the same session. |
| Rejoin | after `lrw-link-check-fail-rejoin` failures in `WARNING` | after that many failures **and** once the ladder is at the floor (region minimum DR, default TX power) |
| Link loss → rejoin (EU868 from DR5, defaults 900 s / LC 5 / 5) | ≈ 9–10 h | ≈ 4–5 h |
| US915/AU915 sub-band | set only as the active channel mask at boot. After ~8 failed joins, JoinRequests spread over all 8 sub-bands (~1 in 8 hit an 8-channel gateway). | also set as the LoRaMac **default** mask and re-applied after each rejoin's MAC re-init |

**Behaviour notes:**

- New log lines: `Link recovery: TX power <a> -> <b>, DR<x> -> DR<y> (payload <n> B)` and `LC FAIL in WARNING (total: n/m, ladder step)`. `ats lrw status` also prints `tx power: <index> (0 = max)`.
- After a ladder recovery the device stays on the lower DR. With ADR on, the network raises it again from the uplinks it receives. A lower DR means a smaller payload budget (EU868 DR0–2: 51 B), so telemetry may take more frames until then.
- The link-check timeout now starts after the uplink's RX windows closed. It no longer races a LinkCheckAns at DR0/SF12 with a 5 s RX1 delay.
- Works together with `lrw-datarate` (§8): a pinned DR is stepped down by the ladder like any other, and the next join re-pins it.
- `ats lrw status` now reports the live DR from the MAC. Before, it showed a stale value after an ADR-off DR change (`lrw-datarate`, a ladder rung).
- Cost: +272 B flash release, +744 B debug, +0 B RAM.

**HW verification (2026-09-23, EU868, ChirpStack v4 on the ProXimos Hub):**
- Ladder runs with ADR off and on: one rung per report DR5 → DR0 on air, with the TX-power rung as +8–9 dB RSSI.
- Recovery on a lower DR with the same DevAddr, both on an injected `lc ok` and after a real NS outage (device disabled on ChirpStack, no rejoin).
- At the floor: a rejoin.
- Combined with `lrw-datarate`: the ladder steps down and the next join re-pins.
- The release image passed too.
- Not HW-tested: the US915/AU915 sub-band fix (no 915 MHz gateway), code review only.

See `doc/manual-test-plan.md` **L18**/**L19** and `doc/plan/424 - Faster link-loss recovery.md`.

---

## 13. Universal response paging (#425)

One paging rule for every answer the device sends over a radio. When a response does
not fit one frame, it is split into **pages**; each page is a complete, independently
decodable message with the same `seq`, carrying part of the answer and "page *i* of *N*".
The device sends all pages by itself. Plan: `doc/plan/425 - Universal response paging.md`.

**Wire format**

| Message | Fields |
|---|---|
| `Response` (fPort 85, every body type) | `page_index = 12`, `page_count = 13` |
| `AlarmReport` (fPort 3) | `page_index = 5`, `page_count = 6` |

- Absent (`page_count` 0/1) = the whole answer is in this one frame, byte-identical to
  an unpaged answer. `page_count >= 2` = page `page_index` (0-based) of `page_count`.
- `ConfigDump.page_index/page_count` and `HistoryFrame.frame_index/frame_count` are
  **deprecated and no longer set**; they stay declared so older firmware still decodes.
- `Response` field 11 is reserved (the #409 draft's `InfoLite`, replaced by Info pages).

**What pages, and how**

| Answer | Page unit |
|---|---|
| Info (join, clock-sync, GetInfo) | each field (NFC also `claim_token` / `lrw_state` / `dev_eui`), then each active alarm (radio: one snapshot for all pages; NFC: a fresh one per page) |
| GetConfig / GetParam | config fields (fixed 30 B pages on LoRaWAN at any DR; not at the 11 B tier → `BUDGET_TOO_SMALL`) |
| settings-info (#412) | each setting / the `w1_slot_type` block |
| W1Scan | ROMs (radio: scan result kept, no rescan per page; NFC: rescan per page, bus order is deterministic) |
| History replay (`req_history`) | records (as before, numbering now in the envelope) |
| AlarmReport | events (every page keeps its own `base_time` / `total`) |

- **Device-driven on the radio**: page 0 answers the request (or is the autonomous
  uplink), the rest follow one per send cycle, paced by the duty cycle; one stream at a
  time (a new paged answer replaces a running one; a rejoin cancels it; the boot
  settings-info waits for the Info pages). A host missing a page re-sends the request
  with `page = <index>` — the stream then sends from that page on.
- **Host-driven on NFC** (also the vendor channel and the debug shell): the phone asks for
  every page itself — `GetConfig.page` / `GetParam.page`, and `GetInfo.page` / `W1Scan.page`
  for an Info or a W1Scan that does not fit the frame (page 0 is the plain request; an Info
  that overflows is paged, no longer trimmed). Nothing is kept between requests: each page is
  laid out from a fresh snapshot, so the pages of one read may differ by the few tenths of a
  second between the requests (accepted). A page past the end → `Error OUT_OF_RANGE`
  (`fault_field` 1). With the 256 B FTM mailbox frame (#414) an Info pages only with about 19+
  simultaneously active alarms and a W1Scan (≤ 4 ROMs, 47 B) never — the rule keeps both
  correct for any smaller frame. `GetInfo.page` / `W1Scan.page` are ignored over LoRaWAN (the
  device streams). NFC history keeps its cursor (`next_ord` / `has_more`), no envelope numbering.
- **Physical floor**: a unit that does not fit even alone is left out (at the 11 B tier
  e.g. the serial number, unix time, an alarm entry or rule); when nothing fits the
  answer is `Error BUDGET_TOO_SMALL`.
- **Telemetry (fPort 2)** is not paged: it keeps its per-sensor-group split, every
  frame already a complete snapshot slice.

**Decoders (TTN / ChirpStack) are stateless.** `ttn.js` decodes each page on its own and
adds `pages: "i/N"`; an Info page lists only the fields it carries (no default zeros).
Nothing is buffered or merged in the decoder — a consumer that wants the whole answer
merges pages by (DevEUI, fPort, `seq`), for `AlarmReport` by `base_time`. A consumer
that ignores `pages` just sees several partial answers.

**Host impact**: Manager-App NFC GetConfig / GetParam / GetInfo / W1Scan must read the page
count from `Response.page_count` (absent = 1) and ask for pages 1..N-1 with `page` — merge an
Info's scalar fields and concatenate its `active_alarms`, concatenate W1Scan ROMs; the proximos-v2 decoder should merge pages
(Hub side: proximos-v2#96; decoder parity: proximos-v2#90). **P2P** uses the same rule and format (driver in
PR #426 on `feat-p2p`): an answer that does not fit one 0x55 RESPONSE (64 B) is streamed
as pages with the same `seq`; the P2P central must accept several 0x55 with one `seq`.

**Duty cycle (host guidance).** At EU868 DR0 a page is ~2.1 s of SF12 airtime, so a
6-page GetConfig uses about a third of the 36 s/h budget of its sub-band; once the budget
is spent, LoRaMac holds **every** uplink (pages, telemetry, alarms) until its hourly window
resets (HW-seen: ~50 min). The firmware does not throttle streams — the host must: no
repeated full GetConfig/GetParam at SF11/SF12 (ask only for the keys needed, wait for a
higher DR, or use NFC), no immediate re-request of a missing page (it may just be waiting
for duty-cycle credit), and page-assembly timeouts of ≥ 1 h at a low DR.

Cost: release about +1.8 KB flash, +128 B RAM.

**Known limitations:**
- **Manager-App** must read `Response.page_count` over NFC (absent = 1). The deprecated `ConfigDump.page_*` is no
  longer set, so an older app sees only the first page of a paged GetConfig. There is no compatibility shim, by
  decision.
- GetConfig / GetParam over LoRaWAN use a fixed 30 B page layout, so the page count is the same at every DR ≥ 51 B.
  At 11 B they answer `BUDGET_TOO_SMALL`.
- A queued AlarmReport page waits behind a running response stream.
- A rejoin does not cancel pages already queued.

**HW verification (2026-09-23, EU868, ChirpStack v4 on the ProXimos Hub):**
- GetConfig (6 pages at DR0, 14 with 8 rules), `page=N` resume, stream cancel, GetParam, paged GetInfo with active
  alarms, AlarmReport pages, history frames, release image and coexistence with #424 all PASS.
- The first run found a `m_work_q` stack overflow on a paged GetInfo. It was a regression of this change (A/B
  against the previous `v1.5.0` was clean) and is fixed before merge.
- Not HW-tested: the 11 B tier and AU915 (no 915 MHz gateway), and P2P (#426).
- See §10 of `doc/plan/425 - Universal response paging.md`.

---

## 14. `GetSettings` — settings-info on request (#428)

The boot settings-info (§4) tells the network the effective configuration once per boot.
A host that wants to refresh it later had only `GetConfig`: 34 keys in 4+ pages over LoRaWAN (one page
per alarm rule on top), ~13 s of SF12 airtime at EU868 DR0. `GetSettings` returns exactly
the §4 content on request.

| | |
|---|---|
| Command | `get_settings` = `Command` field **31**, empty body (29/30 are taken by #414; 15 was `req_alarm_rules`, not reused) |
| Downlink | fPort 85, e.g. `0807fa0100` (seq 7) |
| Answer | `Response.config_dump` with the command's `seq`: `application` interval_sample / interval_report / history_enable, the nine `sensors.cap_*` flags, runtime `w1_slot_type` (1-Wire builds) |
| Size | the boot dump + 2 B for the `seq` (34 B measured without 1-Wire, +6 B with the four `w1_slot_type` entries): one frame at EU868 DR0 and up |
| Paging | over LoRaWAN the same pages as the boot dump (§13 envelope) when the budget is smaller; every page carries the `seq` |
| Transports | all (LoRaWAN, P2P, NFC, vendor, shell) except the plaintext mailbox channel (#414); read-only, no secrets |

The values are the **staged** config, like every `GetConfig` / `GetParam` answer (a change
without `settings save` shows up at once). The boot dump keeps `seq` 0, so a host can tell
the autonomous dump from an answer. Decoders need no change: the answer is a normal
`config_dump`; `ttn.js` only learns the command name for `encodeDownlink`.

**HW verification (2026-09-23, EU868, ProXimos Hub ChirpStack v4, PR #429):** downlink
`0807fa0100` → one fPort-85 frame at DR5, 34 B, `Response{seq 7, config_dump}` whose
`config_dump` bytes are identical to the boot settings-info of the same boot. See
`doc/manual-test-plan.md` scenario **L4c**.
Hub combined11 (proximos-v2 !91): the Portal "Refresh from device" button sends
`GetSettings` and completes on the one-frame answer. A v1.5.0 image without this command
answers `Error NOT_SUPPORTED` (code 7) with the `seq` kept, so the Hub's config is untouched.
Not HW-tested: NFC, DR0 (34 B fits one frame there too) and the paged form (native tests only).


---

## 15. Command answers the Hub can pair (#432)

Found by the ProXimos Nodes test (Hub CLI + Portal against a STICKER, 2026-09-23):

| Command | Before | Now |
|---|---|---|
| `clock_sync` (empty, LoRaWAN) | Answered by the Info that follows the DeviceTimeAns, but that Info had **seq 0**, so the Hub could not pair it with the request | The Info carries the **command's `seq`** (every page of it, when paged). The boot Info keeps seq 0. A newer `clock_sync` before the time lands takes over the seq; no answer at all means the network did not answer `DeviceTimeReq` |
| `force_send`, `sample` (LoRaWAN) | The uplink went through the fleet pre-send jitter (up to 10 s). A request that arrived while a jittered report was pending **collapsed into it** (one uplink instead of two) | The uplink leaves **at once** (~1–3 s incl. the TX); a pending jittered report is folded into this send, so the host gets one fresh uplink right after its command. Periodic reports, alarms and the link-ready kick keep the jitter |
| `w1_scan` on an image without 1-Wire | `NOT_READY` (3), indistinguishable from a bus that is not ready (over LoRaWAN the detail is stripped) | `NOT_SUPPORTED` (7), like other commands not built into the image |
| Any command that fails to decode (truncated, corrupted) | `BAD_REQUEST` with **seq 0**: the host could not pair the error with its request | `BAD_REQUEST` with the **request's `seq`** whenever field 1 is readable before the damage (0 otherwise) — #435 |

No wire-format change: the answers are the same messages. A host that already pairs by `seq`
now also pairs `clock_sync`.

**HW verification (2026-09-24, EU868, ProXimos Hub ChirpStack v4):** ClockSync seq 25 → the
DeviceTimeAns in the RX of the next uplink, then `Response{seq 25, info}` with the synced
`unix_time` (`010819…`); `force_send` → uplink after 1.1–2.8 s, also right after a telemetry
uplink; `send` + `force_send` back to back → one uplink after 1.2 s (before: only after the
jitter); `w1_scan` on a debug image without 1-Wire → `Error{code 7}`.
Re-run with the downlinks sent from the Hub CLI (`proximosctl control.radio node-send`), each
answer paired on the Hub by its `seq`: `clock-sync` seq 45 → `Response{seq 45, info}` with the
synced time; `force-send` seq 46 → extra fPort-2 uplink 1.16 s after the uplink that carried
the downlink; `w1-scan` seq 47 → `Response{seq 47, error{code 7}}`.
A malformed downlink (seq 126, one byte too many in a `set_param`) → `Response{seq 126,
error{code 1 BAD_REQUEST}}`, paired on the Hub (before: seq 0).

**Alarms after a reboot (checked, no change needed):** the alarm latches are plain RAM, so
after any reboot (including `settings_save`) every condition that still holds activates
again and is re-reported on fPort 3 once the device is joined: threshold rules on the first
evaluation (after their dwell), level `state` rules after their dwell, low battery on the
first valid measurement, no-data after 5 s of NaN. Edge/momentary/count rules are events and
fire again only on a new event. A host detects the reboot from the rejoin, the boot `Info`
(`reset_cause`, small `uptime`) or the first telemetry's `boot` flag, drops its open alarms
and waits for them to activate again — the same way it already handles low battery.


---

## 16. `GetConfig` over LoRaWAN without the slot ROMs (#433)

The four 1-Wire slot ROMs (`sensors` 11..14, 8 B each) took two of the six pages of a
LoRaWAN `GetConfig` at EU868 DR0, and the network has no use for them — the ProXimos
Portal does not show them. They are now left out of a **LoRaWAN** `GetConfig`:

| Read path | Slot ROMs |
|---|---|
| `GetConfig` over LoRaWAN | **left out** — 34 keys in 4 pages at DR0 (was 38 in 6) |
| `GetConfig` over NFC / shell / vendor | included, as before |
| `GetParam(sensors 11..14)` over any transport | included — an explicit request still reads them |
| boot settings-info, `GetSettings` | never carried them |

Mechanism: a new configen attribute `dump_lrw: false` keeps a field in `DUMP_FIELDS`
but flags it `lrw_skip`; `app_cmd_handle_get_config()` skips such a field when the
transport is LoRaWAN. A host that merges a complete `GetConfig` into its config copy
therefore no longer sees `sensorN_rom` from LoRaWAN; a host that replaces its copy
(ProXimos !91) drops them. A **P2P** `GetConfig` (§6) leaves them out too: P2P is
budget-limited like LoRaWAN and its device-driven pages are laid out as LoRaWAN pages,
so page 0 has to use the same layout.

**HW verification (2026-09-24, EU868, ProXimos Hub ChirpStack v4):** `get-config` from the
Hub CLI → 4 pages at DR5 (was 6), no `sensor1_rom`..`sensor4_rom`, Hub config 34 keys;
`GetParam(sensors [11])` seq 122 → `config_dump{sensors{sensor1_rom}}` in one frame.


---

## 17. `SetParam.alarms_replace` — rewrite the whole alarm table (#434)

A host that is the source of truth for the alarm rules (the ProXimos Portal) had no way to
say "these are *all* the rules": a `SetParam` only sets the slots it carries, so a rule
deleted in the host (or added over NFC in the meantime) stayed on the device.

| | |
|---|---|
| Field | `Command.SetParam.alarms_replace` = **6** (`optional bool`, next to `save`) |
| Effect | when `true`, every rule slot `alarm_0`..`alarm_15` is emptied in staging **before** this message's `alarms` group is applied, so exactly the rules it carries remain; without an `alarms` group it clears all slots. `alarm_limit` and `alarm_buzzer_mode` are kept |
| Atomicity | part of the batch snapshot: on any fault (e.g. an invalid rule → `OUT_OF_RANGE`, `fault_field` 400) the whole batch rolls back, cleared slots included, and the rule cache is rebuilt |
| Persistence | staged like any other key; `save` persists (+ reboot) |
| Transports | LoRaWAN, NFC, shell (like `alarm_N`); refused over the vendor channel (`NOT_WRITABLE`, `fault_field` 400) |
| Multi-frame table | `alarms_replace` on the **first** message only, `save` on the last |

`ttn.js` encodes and decodes it (`set_param.alarms_replace: true`); e.g. seq 8 with no
`alarms` group is `080812023001`.

**HW verification (2026-09-24, EU868, ProXimos Hub ChirpStack v4, raw downlinks from the Hub
CLI):** with rules [0], [1], [3] seeded, `set_param{alarms{alarm_5}, alarms_replace}` seq 123
→ `Ack` and only [5] left; `set_param{alarms{alarm_2 = invalid}, alarms_replace}` seq 124 →
`Error{OUT_OF_RANGE, fault_field 400}` and nothing changed; `set_param{alarms_replace}` seq 125
→ `Ack` and 0 rules, `alarm_limit` kept. A malformed downlink (one byte too many) was answered
`BAD_REQUEST` without touching the rules.

---

## 18. NFC command channel: ST25DV Fast-Transfer-Mode mailbox (#313)

**Why.** In v1.4.0 the phone drove interactive commands by writing an NDEF
`hio.stck:cmd` record into the ST25DV's user EEPROM and reading an `hio.stck:rsp`
record back (v1.4.0 §10). That EEPROM is **single-port**: the firmware can only
read the command and write the reply while the RF field is **off**, so the phone
has to drop its field between the write and the read. Android reader mode can do
that silently; **iOS Core NFC cannot**, so the iOS flow needed the operator to
tap, lift for ~2 s, and tap again — two taps per command, two per config page,
and frequently a stall. This is a platform + hardware limit, not app code.

**What changed.** Interactive commands now travel through the ST25DV **Fast-
Transfer-Mode (FTM) mailbox** — a 256-byte **dual-port** RAM that the RF reader
and the I2C host exchange messages through **with the field on**, coordinated by
a hardware handshake (`RF_PUT_MSG` / `HOST_PUT_MSG`). No field-off window is ever
needed, so the whole exchange completes in **one tap with the phone held still**,
on iOS exactly as on Android. HW-measured on the bench: ~0.1–0.3 s per exchange,
a full multi-page `GetConfig` and a `SetParam`-with-save in a single hold.

### Protocol (phone side)

The mailbox is reached with standard ISO 15693 custom commands (manufacturer
code `0x02`), the same on Android (`NfcV.transceive`) and iOS
(`Iso15693.customCommand`, non-addressed):

1. `0xAD` read `EH_CTRL_Dyn`: `VCC_ON` must be set (the device is powered — the
   mailbox needs the MCU running; a battery-less unit has no mailbox and no NDEF,
   so it reads as a blank tag).
2. `0xAE` write `MB_CTRL_Dyn = MB_EN`, then `0xAD` read it back. If `MB_EN`
   does not stick within ~1 s the unit is a legacy v1.4.x firmware (no mailbox)
   — fall back to the NDEF flow (Android only).
3. `0xAA`/`0xAB`/`0xAC` a **`[0x03] get_basic_info`** frame → serial + nonce
   high-water + config/FW version. This is the identity bootstrap that replaces
   the old plaintext inf record: the phone picks the cached `secret_key` by serial
   and sends the next command's counter = nonce + 1. It is identity only — the
   device status (alarms, battery, radio, claim window) is owner-only and read
   from `Info.device_status` with the encrypted `GetInfo`.
4. `0xAA` Write Message a **`[channel][payload]`** command frame, poll `0xAD` for
   `HOST_PUT_MSG`, then `0xAB`/`0xAC` Read the reply (in ≤200 B chunks for iOS).
5. Repeat for further commands; `0xAE` write `MB_EN = 0` (or just leave) when done.

**Firmware session limits.** While the field is on the firmware keeps the chip
powered (LPD low) and watches for `MB_EN`. One mailbox session lasts at most
120 s and ends after 3 s without a request. A field held for **120 s without a
served reply** (a phone left lying on the sticker, a fixed reader nearby) releases
the chip until the field changes — every served exchange restarts that window, so
a long exchange is never cut. A command that stages a deferred action (reboot,
settings save, `set_secret_key`, resets, …) ends the session and the action runs
**right away**, even if the phone still holds the field — nothing else is served
until it has run, so a follow-up command can neither see the unapplied state nor
replace the action. After a non-rebooting action (e.g. `lrw_join`) the firmware
resumes the hold, so the phone re-enables `MB_EN` (same ~1 s retry as step 2) and
continues in the same tap; after a reboot it re-reads `get_basic_info`.

**NFC starts last in the boot.** The NFC init and the poll thread run at the end
of the init chain, after the boot LED carousel and every component a command can
reach (clock, history, alarm rules, LoRaWAN, battery, sensors, counters) — ~8 s
after boot — and just before the LoRaWAN join. Until then the chip stays
unpowered (`VCC_ON = 0`), so no phone command can act on uninitialised state (the
#340 M8 class: a `reset_counters` saved before the counters were restored wiped
every totalizer). A phone kept on the tag across an NFC-triggered reboot does not
have to be lifted: it waits for `VCC_ON`; the init then keeps the chip powered
while the field is present, leaves a mailbox the phone enables right then alone
(MB_EN is cleared at boot only with no field, or before a first-boot EEPROM
write), and the poll thread serves the phone's first request at once. A unit whose
mailbox is unavailable (see Production tester) does not hold the chip at all.

The frame is `[channel 1 B][payload]`:

| Channel | Payload | Key |
|:-:|---|---|
| `0x01` | encrypted `Command` (request) / `Response` (reply), byte-identical to the old `hio.stck:cmd`/`hio.stck:rsp` content | `secret_key` |
| `0x02` | same, vendor channel | `vendor_token` |
| `0x03` | plaintext `Command` → `0x01 \|\| Response`, the unauthenticated allow-listed transport: `get_basic_info` (identity bootstrap) and `get_claim_info` (PR #415) | none |

The AES-CCM envelope, the direction-separated nonce, the anti-replay window and
the response cache are **unchanged** from v1.4.0 §10 — only the transport moved,
so the phone's codec is the same. A mailbox frame is 256 B, leaving **231 B of
plaintext** (256 − 1 channel − 8 header − 16 tag); `GetConfig`/`GetParam` and
history now page to fit that (a full snapshot is a few pages read in one hold),
and a `GetInfo` with more than ~17 simultaneously-active alarms drops the alarm
list to fit, as it already does on a tight LoRaWAN frame.

### Identity: no NDEF record — `get_basic_info` instead

v1.5.0 removes the `hio.stck:inf` record too: the tag holds **no NDEF at all**.
A phone reads the serial, the anti-replay nonce high-water and the config/FW
version from the plaintext `get_basic_info` command over the mailbox (channel
`0x03`), right after enabling it — so a generic NFC reader or a
dead-battery unit now shows a **blank tag** rather than the serial (accepted,
since configuration and claiming already need a powered device). Dropping the
record removes the last EEPROM writer, and with it the field-off gate whose
single-port RF/I2C contention was the whole reason the v1.4.0 NDEF channel could
stall or wedge i2c1 — the poll thread now only ever serves the mailbox.

### What is removed / breaking

- **All NDEF records.** The command channel (`hio.stck:cmd` / `hio.stck:rsp` /
  `hio.stck:ack`, the vendor `hio.stck:vnd`) AND the resting identity record
  (`hio.stck:inf`) and the `hio.stck:clm` claim record are gone — the tag holds
  no NDEF. Firmware v1.5.0 answers only over the mailbox; the Manager-App must
  use it (lockstep release). An old app's `hio.stck:cmd` left on the tag is
  ignored, never executed.
- **Battery-less configuration / boot-staged provisioning** (v1.4.0 §10
  "Provisioning while powered off", #147/#250). The mailbox needs the MCU
  powered, so a command can no longer be staged into an unpowered unit and
  applied at the next boot. Claiming likewise moves to a powered device; the
  claim-window redesign and the plaintext `get_claim_info` are in **PR #415**.
- **Android tap-to-launch** via the MIME identity record (#298).
- **`nfc dump` and `nfc check|autocheck`** (v1.4.0 bench shell). The firmware no
  longer touches the user EEPROM on any path. Enabling the mailbox does not
  change the EEPROM either, so a unit reflashed from v1.4.x keeps its old NDEF
  records (possibly a plaintext `clm` claim token) until they are wiped by hand —
  `nfc clear` on a debug build (see Bench shell) or an RF erase (e.g. ST25 NFC Tap).

### Production tester

Authorising FTM sets the static `MB_MODE` bit once, at the first boot, together
with the static GPO config (the I2C-password session and the EEPROM writes run only
while one of those bits is still unset; later boots only read them). A unit whose `MB_MODE`
cannot be set has **no interactive NFC channel** — a hardware/production defect,
not something the firmware can work around. It is reported as
`APP_DEVICE_STATUS_MAILBOX_DOWN` (device_status **bit 13**, `0x2000`) in the
GetInfo response and as an `NFC mailbox: UNAVAILABLE` line in `ats device info`,
so the production tester rejects it.

### LED during a tap

| What happens | LED |
|---|---|
| Phone detected (RF field), no mailbox session yet | green, at most 5 s |
| Mailbox session running | green blink |
| Session ended, **last** exchange OK | green + yellow, 2 s |
| Session ended, last exchange failed | red, 2 s |
| Otherwise / afterwards | off |

"Failed" means the last request was rejected (wrong key or nonce, unknown channel — no reply
is sent), its reply could not be written or was never read by the phone, or the session aborted
on I2C errors; an authenticated `Response.error` counts as a valid reply. The last exchange
decides, so an app that resyncs after a rejection and then succeeds ends green + yellow. A
command that reboots the device (save, reboot, resets, `set_secret_key`, `claim_active`,
calibration, `lrw_reset`) first lets the result finish, then reboots; the boot carousel follows.
The v1.4.0 NDEF states (per-command processing blink, green + yellow "response waiting",
immediate red blink per rejected frame, pre-reboot green NFC carousel) are gone.

### Bench shell

`nfc mb status` (dump the FTM registers), `nfc mb on|off` (drive `MB_EN` from the
I2C side), and `nfc mb serve` (enable and serve the mailbox for a reader that
cannot issue Write Dynamic Configuration itself); `nfc reg|regw` read/write a
system or dynamic register. Debug build only: `nfc read <off> <len>`,
`nfc write <off> <hex>` (≤ 64 B) and `nfc clear` (zero all 512 B) access the user
EEPROM by hand, e.g. to wipe stale v1.4.x NDEF records; they refuse while an RF
field is present (remove the phone) and clear `MB_EN` first (the chip refuses
EEPROM writes while FTM is on). `ats cmd nfc` still injects a
command straight into `app_cmd_handle` for phone-free command-logic testing.

### Test coverage

`tests/nfc_hw` gained a full ST25DV mailbox model (registers, 256 B RAM, the
RF/host handshake, the datasheet rule that every EEPROM write NACKs while
`MB_EN=1`, and a password-failure mode) plus session ztests: boot authorisation
+ GPO config, the `MAILBOX_DOWN` flag on a password failure, a stuck `MB_EN`
cleared on the next boot, a field present at boot served without a field change,
a mailbox session the phone opened during boot kept by the init (MB_EN kept, chip
left powered) and served, owner- and vendor-command sessions that advance the nonce
and leave the claim window active, a rejected channel prefix, a plaintext
`get_basic_info`, and the session limits: a field held without traffic released
after 120 s (restarted by an exchange), no hold when the mailbox is unavailable, a
deferred action ending the poll while the phone still holds the field (a follow-up
command is not served and cannot replace it), and `app_cmd_get_info()` — which
`m_work_q` runs for the on-join / clock-sync / downlink `GetInfo` — never waiting
on a tap (the claim state is read lock-free). `tests/cmd` checks every `GetConfig`
page fits one 256 B mailbox frame.

---

## 19. Plaintext command transport and explicit claiming (#415)

Prepares the claim flow for the NFC mailbox move (#313/#414) and tightens the
claim window into something with no automatic behaviour.

### 19.1 The `plain_text` transport

A new command transport, `plain_text`, carries a **raw `Command` protobuf** in and
`0x01 || Response` out — no AES-CCM, no nonce, no response cache. It is the
unauthenticated, identity-disclosure channel a phone uses before it holds any key.
It is reachable over the NFC mailbox channel `0x03` (added by #414) and, for the
bench, the shell `ats cmd plain <hex>`; it is **never** reachable over LoRaWAN.

The transport is **strictly opt-in**. A command answers on it only by listing
`plain_text` in `app_config.yml`; every other command is rejected by the generated
dispatch with `NOT_READY "transport not allowed"`. This is enforced in configen:
the historical "omitted `transports:` = all transports" default now means "all
transports **except** `plain_text`", so a command that does not name it — `get_info`
(which would disclose `claim_token`), `set_param` (which would write config), … —
can never be answered without a key. **Rule for any command that opts in:
read-only, and disclosing identity-class data only.**

### 19.2 `get_claim_info` (proto 29)

The first `plain_text` command (also allowed over `nfc` and `shell`). Empty request;
returns `Response.claim_info { serial_number, claim_token }` — the same data the
plaintext `hio.stck:clm` NDEF record carries today — **while the claim window is
active**. Once the window is `done` it returns `NOT_READY "claimed"`; before a
token is provisioned, `NOT_READY "no claim token"`. Unlike the NDEF record it needs
a **powered** device, so a shelf attacker can no longer read the token off an
unpowered box.

### 19.3 Explicit two-state claim window

The claim window (`clm/state` in NVS) is now a two-state latch:

| State | Meaning |
|---|---|
| `active` | factory default — the device may still be claimed: `get_claim_info` discloses the token |
| `done` | claiming finished — `get_claim_info` → `NOT_READY "claimed"` |

Removed relative to v1.4.0: the auto-arm (a provisioned token no longer lazily
"arms" the record) and the **implicit close** — in v1.4.0 any successfully
decrypted command closed the window (#308); now it closes **only** on an explicit
`claim_done`. The app must therefore send `claim_done` after storing the claimed
keys; a crash in between leaves the token readable on a powered unit (accepted:
the backend refuses a second claim of the same serial, so only the token leaks,
not control). Mutators are explicit only: `claim_done` / `ats claim done` →
`done`; `claim_active` / `ats claim active` / `vendor_reset` → `active`.
`device_reset` / `factory_reset` leave the state alone.

Upgrading from v1.4.x migrates the old tri-state in place: `unset`/`pending` →
`active`, `consumed` → `done`.

### 19.4 Command rename (wire-compatible)

`clm_ack` → `claim_done` (id 25) and `clm_rearm` → `claim_active` (id 27); messages
`ClmAck`/`ClmRearm` → `ClaimDone`/`ClaimActive`. The **field numbers do not move**,
so already-deployed downlinks and vendored protos stay byte-compatible — only the
generated names change (firmware, JS decoder, and the Manager-App's vendored proto).

### 19.5 Bench

`ats cmd plain <hex>` injects a raw Command over the transport; `ats claim
active|done|status` drives and prints the window state. Example:
`ats claim status` on a freshly provisioned unit prints `claim window: active`;
`ats cmd plain <GetClaimInfo>` returns the `ClaimInfo`; `ats cmd plain <GetInfo>`
returns `NOT_READY "transport not allowed"`.

---

## 20. Last-downlink link quality in the NFC GetInfo (#409 A2)

An installer with only a phone (Manager-App over NFC) has no view of the network
server, so it could not tell whether the radio link is good where the device is
mounted. The NFC `Info` now carries the link quality of the **last downlink the device
received**, as measured by the device:

| Field | Type | Meaning |
|---|---|---|
| 16 `last_dl_rssi` | sint32 | RSSI of the last downlink, dBm |
| 17 `last_dl_snr` | sint32 | SNR of the last downlink, dB |
| 18 `last_dl_age_s` | uint32 | seconds since that downlink was received |

- **NFC only** — like `lrw_state` and `dev_eui`. The LoRaWAN `Info` does not carry them:
  the network server already has the uplink RSSI/SNR per gateway and, with `DevStatusAns`
  (#419), the device-side downlink SNR margin and battery.
- **Always with its age.** A Class A device only receives a downlink when the network
  sends one, so the reading can be hours old. The values reflect any downlink, including
  MAC-only ones (ADR, DevStatusReq, LinkCheckAns).
- **Omitted until the first downlink since boot**, so a missing value never reads as 0 dBm.
- The same values are on the debug shell: `ats radio status` (`rssi`, `snr`).
- `ttn.js` decodes them as `last_dl_rssi`, `last_dl_snr`, `last_dl_age_s`.
- **Paging (with §18):** in the host-driven NFC `GetInfo` paging the three fields form **one**
  NFC-only Info unit (next to `lrw_state` / `claim_token` / `dev_eui`), so RSSI/SNR never
  travel on a page without their age; the unit is empty (not sent) until the first downlink.

Cost: release +160 B flash, +0 B RAM.

---

## 21. History timestamps follow the RTC (F27, F28, H-4)

A history record carries no time of its own: its time is implicit, `base +
ordinal × interval_report`. v1.5.0 before this change assumed every record came
exactly one interval after the previous one and none was ever missing. On the
bench (unit 0413) that failed in four ways:

| Cause | Effect (before) |
|---|---|
| Report cadence re-armed `interval_report` after each run on the kernel clock; the debug build's SysTick runs on the free-running MSI (~1.22 % slow, `CONFIG_PM=n`) | Debug timestamps lagged ~44 s/h, ~10 min after 6 h (F27) |
| `app_history_capture()` returned early while a LoRaWAN replay was streaming (#126) | Every skipped tick shifted all newer records by −1 interval |
| MCU halted / stalled for several intervals (H10: 6 min halt) | Records after the halt claimed times inside the halt (+358 s) |
| Flash ring after a reboot / power loss: the first new page was stamped by *ordinal continuation* of the old ring | Post-boot records claimed the time the outage started — shifted by the whole outage, after a power loss even flagged synced (F28, reproduced in `tests/history_flash`: −18000 s after a 5 h outage) |

### What changed

- **Debug clock (A).** `app/debug.overlay` sets `msi-pll-mode` on `clk_msi`: the
  MSI is trimmed by hardware against the 32.768 kHz LSE, so the debug kernel clock
  (and every kernel timeout, LoRaWAN RX windows included) tracks the crystal.
  `app/CMakeLists.txt` applies the overlay automatically whenever `debug.conf` is
  in `EXTRA_CONF_FILE` (so also for `debug-history-flash`). Release is unchanged:
  its tick already runs on LPTIM1/LSE, and MSI PLL mode was not validated there
  across Stop2.
- **Cadence on RTC slots (B).** The periodic report (sample + history capture +
  telemetry) runs on a slot grid `anchor + k × interval_report` of the wall clock
  (`app_slot.c`). Each run maps to the nearest slot and arms the timer for the
  distance to the next one, re-read from the RTC, so kernel-clock drift and late
  runs never accumulate. Before the RTC is set the grid runs on uptime (as before);
  at the first sync the anchor is carried over by the `unix − uptime` offset, so
  the phase is kept. A timer firing early or late by up to
  `MIN(interval / 2, 15 s)` (`APP_SLOT_TOLERANCE_S`, covers work-queue latency)
  keeps its slot. A run further off — a debug halt or a stall longer than the
  timer's remaining time, an RTC step — re-lays the grid at its own time, so its
  record keeps the true sampling time (and history opens a new segment) instead
  of borrowing a slot up to half an interval away (HIL T4: a 150 s halt put the
  run 30 s off the grid). An `interval_report` change lays a new grid. Boot arming is
  unchanged (first report one interval out) and the telemetry pre-send jitter
  (#267) stays in `app_radio_lrw`.
- **No capture skipped during a replay (C).** The replay cursor is an absolute
  record ordinal (ring start + evicted total), so eviction under a running replay
  moves nothing: no record is repeated or skipped, a cursor whose record was
  evicted resumes at the oldest stored one, and the replay covers the records that
  existed at its start (newer ones are left for the next replay). Flash backend:
  writes within the current page go on, but the page rollover (a ~20 ms erase that
  would stall the replay's RX windows) is held off until the replay ends — a record
  that needs the next page meanwhile is dropped (a hole, no RAM for a queue).
- **Segments with their own time base (D).** Record time is periodic only within a
  *segment*, and a `HistoryFrame` never crosses a segment boundary, so the host's
  `t0_unix + j × interval_s` stays exact without a protocol change:
  - **flash ring (release):** segment = page. A page's `base_time` is the RTC time
    of its first record's slot (uptime with `base_synced=0` while the RTC is unset),
    never the continuation of the page before it. Every boot still starts a new
    page, which now gets its real time — F28 is gone. A report slot that doesn't
    continue the head page's grid (missed slots after a halt/stall or a dropped
    record, an RTC step of more than half an interval) closes the page early and
    opens a new one stamped with that slot; the rest of the old page stays unused.
  - **page header v2** (`PAGE_MAGIC` "HRN2", 40 B = the 32 B v1 header + one double
    word). The extra double word stays erased when the page is opened. A page opened
    before the RTC was set (power loss, no RTC until the network `DeviceTimeAns`)
    is re-based at the clock sync, and the `unix − uptime` offset (+ CRC) is
    programmed into that double word once — from the report work queue, never from
    the downlink callback (#96). Mount applies it, so the page keeps its unix times
    after later reboots. This replaces the #191 "newest record = now" estimate: a
    page of an earlier boot that never saw the clock stays **unsynced**
    (`time_synced=false`) instead of getting a guessed time.
  - **v1 pages** (32 B header, earlier firmware) stay mountable and readable in the
    same chain, each with its own base; new pages are always v2, so the ring
    migrates as it wraps.
  - **RAM ring (debug):** a 4-entry segment table (32 B). A slot discontinuity
    opens a new entry; a fifth one drops the oldest segment with its records.
- **Replay end (E, H-4).** The export cursor skips to the next record *inside* the
  window, so the frame that carries the window's last record ends the replay at
  once — no extra empty attempt, no `WRN History replay stop at frame N/N`, ~3 s
  earlier. The warning and the `BUDGET_TOO_SMALL` error stay for the real case
  (records left but none fits the data rate). The NFC paged read uses the same
  cursor: the page that reaches the window end already returns `has_more=false`.
  The P2P replay (§6, B8) uses the same absolute cursor, per-frame `time_synced`
  and end rule.

### Host-visible behaviour

- **Wire format unchanged** (`HistoryFrame` fields, `ttn.js`, golden vectors).
- **More frames:** one extra frame per segment boundary inside the requested window
  (at most one per page: ~585 records per page vs. ~70 records per frame at DR5).
  `frame_count` counts them.
- **`time_synced` is per frame** now (it was one flag for the whole buffer): each
  frame reports its segment's state.
- **Unsynced records and the window.** A record of an unsynced segment has no unix
  time, so a `[from_unix, to_unix]` window can't place it. It is returned for an
  open window (`from_unix` 0, `to_unix` `UINT32_MAX` — what a host sends without
  bounds) and while the device itself has no wall clock (unchanged: the whole
  buffer until the clock is synced), but not for a bounded window on a synced
  device, so a Portal gap fill doesn't drag stale uptime pages along every time.
- Shell: `history info` adds `segments:`; `history read` prints an unsynced record's
  time as `up <s> (no-rtc)` (uptime of the boot that recorded it).

### Cost

| | Before | After |
|---|---|---|
| Release FLASH / RAM | 163740 B / 52620 B | 165020 B (+1280) / 52684 B (+64, per-page base in RAM) |
| Debug FLASH / RAM | 221448 B / 61628 B | 223160 B (+1712) / 61628 B (+0) |
| `debug-history-flash` FLASH / RAM | 223712 B / 60668 B | 225440 B (+1728, 98.28 % of 224 KB) / 60732 B (+64) |
| Flash ring, temp + humidity (3 B) | 588 records/page, 9408 total | 585 records/page, 9360 total (**−0.51 %**: 1764 → 1757 data bytes/page) |
| Debug RAM ring | 341 records | 341 records |

A split (halt, stall, RTC step) additionally leaves the rest of that page unused;
reboots already did. Flash writes per record are unchanged; the fix-up double word
is one extra program per page recorded without RTC.

### Upgrade / downgrade

Upgrading keeps the stored history (v1 pages are read as before). A **downgrade**
to an earlier firmware does not recognise v2 pages: it mounts whatever v1 pages are
left from before the upgrade (stale records) — run `history clear` after a
downgrade.

### Tests

`tests/history_flash`: F28 (RTC kept across the reboot and power loss + sync:
zero shift, first frame ends at the page boundary), unsynced page of an earlier
boot, fix-up double word written from the work queue or the next capture and
re-read after reboots, v1 pages mounted next to v2 pages, v2 page capacity,
missed-slot page split, slot jitter, RTC step back, replay holding off the
rollover (plain and split). `tests/history`: capture during a replay with eviction
(absolute cursor), reset during a replay, RAM segment split / table overflow /
retirement / clock-sync re-base, replay end at the window end. `tests/slot`: slot
grid rounding, early/late runs, a 1.22 % slow kernel clock over 6 h, uptime → unix
switch, RTC steps, interval change, and a run far off its slot re-anchoring.
`tests/history_flash` also checks that a page of foreign data is skipped at
mount (not erased) and does not hide a valid chain that wraps around the end of
the partition.

### Hardware acceptance (bench unit, 2026-09-25/26)

Run on the bench STICKER against the ProXimos Hub (ChirpStack + Portal), on the
debug RAM, debug-history-flash and release images:

| Test | Result |
|---|---|
| F28 on the old code (debug-history-flash, 3 min halt + reset) | reproduced: post-boot records stamped −235 s |
| Same with this change | post-boot records carry their real time |
| v1 → v2 upgrade (same partition) | v1 pages mounted and exported next to new v2 pages |
| Halt 6 min / 4 × 150 s (flash and RAM) | one new segment per halt, a hole instead of a shift; replay returns one frame per segment with the correct `t0`; the RAM segment table overflows cleanly |
| Run 30 s off its slot after a halt | fixed during HIL: the record now carries its sampling time (was borrowing the nearest slot) |
| Debug kernel drift, MSI PLL | −2 s over 6 h (1.22 % ≈ 267 s before); after 6 h the newest record is stamped within ~1 s of its sampling time, 340 consecutive 60 s steps in one segment |
| DR0, 60 s, 91 min with duty-cycle restriction and forced rejoins | no re-anchor, no hole, no page closed early; replay at DR0 = 9 records per frame |
| Release: 30 min ChirpStack outage + Portal auto backfill | one replay, 36 records at 60 s steps, times exact |
| Release: `interval_report` change | history restarts at the new interval (unchanged, intended) |

Not covered on hardware: the fix-up double word after a real power loss with the
RTC unset (a J-Link reset keeps the RTC) — covered by `tests/history_flash`.
Known and unchanged: a reset or power loss loses the ≤ 2 records still staged in
RAM (one double word), and history is not preserved across a partition layout
change (debug-history-flash ↔ release) — acceptable, history exists to bridge
LoRaWAN outages, not as an archive across firmware updates.

---

## 22. M-2 watchdog respects the duty cycle (F29)

The M-2 stale-uplink watchdog (`heartbeat_work_handler` in `app_radio_lrw.c`) forces a
MAC-reset rejoin when the device is joined but no telemetry uplink has left for
4 × `interval_report`. That catches a mute station whose sends are perpetually
skipped (budget 0 loop, retries exhausted) while the work queue and the IWDG
stay healthy.

It also fired when the sends were refused by the EU868 duty cycle
(`lorawan_send()` → `-ECONNREFUSED`, `Duty-cycle restricted`). The MAC is alive
then — only throttled until the 1 h observation window of the band credits rolls
over — and the rejoin re-initialises LoRaMac, whose band credits live in RAM. The
device got fresh credits and could exceed the 1 % limit (ETSI EN 300 220): on the
bench at DR0 with a 60 s interval it rejoined twice in 91 min. A join does not
entitle the device to new airtime; the duty cycle applies to the radio, not to
the session.

- Every uplink now goes through `lrw_send()`, which records the result in a
  duty-cycle refusal streak (first and last refusal); a successful send or a join
  clears it.
- The decision is `stale_check()` in `app_radio_lrw.c`: when the station is stale
  but duty-cycle refusals keep coming (the last one within one report interval
  + 3 min) and the streak is shorter than the credit window + margin (75 min),
  M-2 holds and logs `... the duty cycle is refusing sends ...: no rejoin (M-2)`
  once. Otherwise it rejoins exactly as before.
- Link loss is still detected by the link-check ladder (3 fails → WARNING →
  5 fails → RECONNECT), which is independent of M-2; a MAC stuck in
  "restricted" beyond the window still ends in an M-2 rejoin.
- Telemetry refused by the duty cycle is dropped after its retries as before;
  the history ring keeps capturing, so the values are backfilled once the
  credits return.

At the default 900 s interval this never triggers (DR0 ≈ 4 uplinks/h ≈ 8 s of the
36 s budget); it matters for short intervals at low data rates and long history
replays at DR0.

Tests: the decision first shipped as its own module with a `tests/lrw_stale`
suite (7 cases). It was folded back into `app_radio_lrw.c` so the transport code stays
in the transport modules; the unit tests return with the common `app_radio`
layer on feat-p2p. The hardware run below covers the behaviour.

Hardware (bench unit, 2026-09-26, DR0 + ADR off, 60 s): the duty cycle refused
every send from 07:21:46Z (the 1 h credit window started at the join, 07:14Z);
M-2 logged the hold once at 07:25:43Z and did **not** rejoin; the uplinks
resumed on the same session (same DevAddr) at 08:14:59Z, when the window
rolled over. Before the fix the same run rejoined 4 intervals into the
restriction and got fresh credits.

---

*Applies to firmware v1.5.0. Reflects changes relative to v1.4.0 — see `doc/version 1.4.md` for the full v1.4.0 feature set this builds on.*
