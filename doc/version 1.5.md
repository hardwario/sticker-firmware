# HARDWARIO STICKER — Firmware v1.5.0 — What's New

This document lists **only the changes introduced in firmware v1.5.0** relative to the v1.4.0 series. Existing v1.4.0 behaviour is unchanged unless noted.

---

## Overview of changes

| Area | Change |
|---|---|
| Buzzer | **New** — alarm-driven melodies (#397, Phase 2 of #338): the buzzer HW variant now sounds automatically while any alarm is active, gated on a new global `alarm-buzzer-mode` config key |
| Debug builds | **New** — 8 independently Kconfig-toggleable subsystems (#395): `debug.conf` ships a lean default (W1, accelerometer, buzzer, PIR off) with real flash/RAM headroom instead of a maximally-squeezed image; `CONFIG_RADIO_LORAWAN=n` disables all radio for bench work. Release builds unaffected. |
| LED | **New** — HW-PWM-backed LED primitives (#301): `app_led_fade()` / `app_led_heartbeat()` and a runtime idle-indicator config, exposed via debug-build shell (`ats led fade\|heartbeat\|idle`). The boot carousel now fades red/green (yellow unchanged); the LoRaWAN-off idle blink is unchanged (unvalidated power cost, see §3). |
| NFC | **Changed (breaking)** — interactive NFC commands (`GetInfo` / `GetConfig` / `SetParam` / vendor) move from NDEF records to the **ST25DV Fast-Transfer-Mode mailbox** (#313): one tap, phone held still, iOS at parity with Android. The NDEF command/response/ack records are removed; the identity record stays (now a short external type, no tap-to-launch). Battery-less configuration is dropped; claiming moves to a powered device (see also PR #415). See §4. |
| NFC claiming | **Changed (breaking for provisioning)** — new unauthenticated `plain_text` command transport with a compile-time allow-list, first command `get_claim_info` (#415); the claim window becomes an explicit two-state latch (`active`/`done`) with the auto-arm and the implicit close removed; commands `clm_ack`/`clm_rearm` renamed to `claim_done`/`claim_active` (same wire ids 25/27). See §5. |

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

## 4. NFC command channel: ST25DV Fast-Transfer-Mode mailbox (#313)

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

1. Read the resting **`hio.stck:inf`** record for the serial and the anti-replay
   nonce high-water (unchanged contract, see below).
2. `0xAD` read `EH_CTRL_Dyn`: `VCC_ON` must be set (the device is powered — the
   mailbox needs the MCU running; a battery-less unit has no mailbox).
3. `0xAE` write `MB_CTRL_Dyn = MB_EN`, then `0xAD` read it back. If `MB_EN`
   does not stick within ~1 s the unit is a legacy v1.4.x firmware (no mailbox)
   — fall back to the NDEF flow (Android only).
4. `0xAA` Write Message a **`[channel][payload]`** frame, poll `0xAD` for
   `HOST_PUT_MSG`, then `0xAB`/`0xAC` Read the reply (in ≤200 B chunks for iOS).
5. Repeat for further commands; `0xAE` write `MB_EN = 0` (or just leave) when done.

The frame is `[channel 1 B][payload]`:

| Channel | Payload | Key |
|:-:|---|---|
| `0x01` | encrypted `Command` (request) / `Response` (reply), byte-identical to the old `hio.stck:cmd`/`hio.stck:rsp` content | `secret_key` |
| `0x02` | same, vendor channel | `vendor_token` |
| `0x03` | reserved for the plaintext `get_claim_info` command (PR #415) — rejected until it lands | — |

The AES-CCM envelope, the direction-separated nonce, the anti-replay window and
the response cache are **unchanged** from v1.4.0 §10 — only the transport moved,
so the phone's codec is the same. A mailbox frame is 256 B, leaving **231 B of
plaintext** (256 − 1 channel − 8 header − 16 tag); `GetConfig`/`GetParam` and
history now page to fit that (a full snapshot is a few pages read in one hold),
and a `GetInfo` with more than ~17 simultaneously-active alarms drops the alarm
list to fit, as it already does on a tight LoRaWAN frame.

### Identity record (`hio.stck:inf`)

Still present at rest, still plaintext, same `<serial>:<config_ver>:<nonce_hi>`
ASCII payload readable by any NFC reader. It is now a short **external-type**
record (`hio.stck:inf`) instead of the v1.4.0 MIME media-type record: Android
**tap-to-launch** (#298) is dropped — the user opens the app themselves — which
also removes the intent-filter that was a source of RF-field regressions on the
phone side. After a mailbox session the record is refreshed once the field drops
(the nonce high-water advanced).

### What is removed / breaking

- **The NDEF command channel** (`hio.stck:cmd` / `hio.stck:rsp` / `hio.stck:ack`
  and the vendor `hio.stck:vnd` record). Firmware v1.5.0 no longer answers a
  command written into the tag EEPROM; the Manager-App must use the mailbox
  (lockstep release). An old app's `hio.stck:cmd` left on the tag is ignored,
  never executed.
- **Battery-less configuration / boot-staged provisioning** (v1.4.0 §10
  "Provisioning while powered off", #147/#250). The mailbox needs the MCU
  powered, so a command can no longer be staged into an unpowered unit and
  applied at the next boot. Claiming likewise moves to a powered device; the
  claim-window redesign and the plaintext `get_claim_info` are in **PR #415**.
- **Android tap-to-launch** via the MIME identity record (#298).

### Production tester

Authorising FTM sets the static `MB_MODE` bit once at boot (inside the existing
I2C-password session that already configures the GPO). A unit whose `MB_MODE`
cannot be set has **no interactive NFC channel** — a hardware/production defect,
not something the firmware can work around. It is reported as
`APP_DEVICE_STATUS_MAILBOX_DOWN` (device_status **bit 13**, `0x2000`) in the
GetInfo response and as an `NFC mailbox: UNAVAILABLE` line in `ats device info`,
so the production tester rejects it.

### Bench shell

`nfc mb status` (dump the FTM registers), `nfc mb on|off` (drive `MB_EN` from the
I2C side), and `nfc mb serve` (enable and serve the mailbox for a reader that
cannot issue Write Dynamic Configuration itself). `ats cmd nfc` still injects a
command straight into `app_cmd_handle` for phone-free command-logic testing.

### Test coverage

`tests/nfc_hw` gained a full ST25DV mailbox model (registers, 256 B RAM, the
RF/host handshake, the datasheet rule that every EEPROM write NACKs while
`MB_EN=1`, and a password-failure mode) plus session ztests: boot authorisation
+ GPO config, the `MAILBOX_DOWN` flag on a password failure, a stuck `MB_EN`
cleared on the next boot, an owner-command session that consumes the claim window
and advances the nonce, a vendor session that does not, and a rejected channel
prefix. `tests/cmd` checks every `GetConfig` page fits one 256 B mailbox frame.

---

## 5. Plaintext command transport and explicit claiming (#415)

Prepares the claim flow for the NFC mailbox move (#313/#414) and tightens the
claim window into something with no automatic behaviour.

### 5.1 The `plain_text` transport

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

### 5.2 `get_claim_info` (proto 29)

The first `plain_text` command (also allowed over `nfc` and `shell`). Empty request;
returns `Response.claim_info { serial_number, claim_token }` — the same data the
plaintext `hio.stck:clm` NDEF record carries today — **while the claim window is
active**. Once the window is `done` it returns `NOT_READY "claimed"`; before a
token is provisioned, `NOT_READY "no claim token"`. Unlike the NDEF record it needs
a **powered** device, so a shelf attacker can no longer read the token off an
unpowered box.

### 5.3 Explicit two-state claim window

The claim window (`clm/state` in NVS) is now a two-state latch:

| State | Meaning |
|---|---|
| `active` | factory default — the device may still be claimed: the `hio.stck:clm` record is laid and `get_claim_info` discloses the token |
| `done` | claiming finished — no `clm` record, `get_claim_info` → `NOT_READY` |

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

### 5.4 Command rename (wire-compatible)

`clm_ack` → `claim_done` (id 25) and `clm_rearm` → `claim_active` (id 27); messages
`ClmAck`/`ClmRearm` → `ClaimDone`/`ClaimActive`. The **field numbers do not move**,
so already-deployed downlinks and vendored protos stay byte-compatible — only the
generated names change (firmware, JS decoder, and the Manager-App's vendored proto).

### 5.5 Bench

`ats cmd plain <hex>` injects a raw Command over the transport; `ats claim
active|done|status` drives and prints the window state. Example:
`ats claim status` on a freshly provisioned unit prints `claim window: active`;
`ats cmd plain <GetClaimInfo>` returns the `ClaimInfo`; `ats cmd plain <GetInfo>`
returns `NOT_READY "transport not allowed"`.

---

*Applies to firmware v1.5.0. Reflects changes relative to v1.4.0 — see `doc/version 1.4.md` for the full v1.4.0 feature set this builds on.*
