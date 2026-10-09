# HARDWARIO STICKER — Firmware v1.5.0 — What's New

This document lists **only the changes introduced in firmware v1.5.0** relative to the v1.4.0 series. Existing v1.4.0 behaviour is unchanged unless noted.

---

## What's new in v1.5.0 — summary for users

This chapter is the short version for anyone who installs, configures or integrates
STICKER: what is new, what behaves differently, and what has to be done when a fleet
moves from v1.4.x to v1.5.0. The numbered sections below (§1–§37) carry the full
technical detail.

### Highlights

| | What you get | Details |
|---|---|---|
| **P2P radio** | A second radio mode for sites with no LoRaWAN network: `radio-mode p2p` talks directly to a HARDWARIO FIBER modem and a ProXimos Hub. Telemetry, alarms, commands, history backfill, network time and link supervision work as over LoRaWAN; the same firmware image does both, the choice is made at boot. | §6, §23–§34 |
| **One-tap NFC, iOS included** | All phone commands go through the ST25DV Fast-Transfer-Mode mailbox: one tap, phone held still, a full configuration read or write in one hold (~0.1–0.3 s per exchange). iOS now works the same as Android. | §18 |
| **Safer claiming** | The claim token is no longer readable from an unpowered box. The claim window is an explicit `active` / `done` switch that the app closes with `claim_done`; a corrupted NVS value closes it. After a `vendor_reset` the owner re-opens it with `claim_active`, which also creates a new token. | §19, §35 |
| **The network knows the configuration** | After every boot/join the device sends its Info **and** its key settings (intervals, every sensor capability flag, detected 1-Wire sensors). The same pair is re-sent every `interval-announce` hours (default 24 h), and `GetSettings` asks for it at any time. | §4, §14, §37 |
| **Audible alarms** | On the buzzer HW variant the buzzer plays a melody when an alarm activates, and repeats it while the alarm lasts, set with `alarm-buzzer-mode`. | §1 |
| **More robust LoRaWAN** | A device that lost its gateway first lowers the data rate and raises the TX power, and rejoins only when that fails: about 4–5 h from link loss to rejoin (was 9–10 h). Fixes: a stale join result, a wedged radio work queue, the missing `DevStatusAns` (battery/margin in ChirpStack), rejoins that broke the EU868 duty cycle, alarms lost in a burst or to a reboot after a command. | §5, §10, §12, §22, §31, §32 |
| **New region and fixed data rate** | `lrw-region as923` (AS923-1). With ADR off, `lrw-datarate dr0`…`dr7` fixes the uplink data rate. | §8, §11 |
| **Radio diagnostics** | `get_radio_state` returns the link state, SF/DR, TX power, RSSI/SNR of both link directions, gateway count, duty-cycle use and counters since boot, over NFC or a radio, for both radio modes. | §24 |
| **Long answers always arrive** | Any answer that does not fit one radio frame is split into self-contained pages that the device sends by itself. At the lowest data rates a command is still answered (`BUDGET_TOO_SMALL`), never left silent. | §9, §13 |
| **Correct history timestamps** | History records keep their real time across reboots, power loss, debugger halts and replays. Before, they could be shifted by the length of an outage. | §21 |
| **Switchable onboard sensor** | `cap-sht` turns the onboard temperature/humidity sensor off like every other sensor. | §36 |
| **Replace all alarm rules at once** | `SetParam.alarms_replace` makes one message the complete alarm table, so rules deleted on the host are deleted on the device. | §17 |

### New configuration parameters

| Parameter | Default | Values | Purpose | § |
|---|---|---|---|---|
| `alarm-buzzer-mode` | `off` | `off` / `once` / `slow` (120 s) / `normal` (30 s) / `fast` (10 s) / `continuous` | Buzzer while an alarm is active; needs `cap-buzzer` | §1 |
| `lrw-datarate` | `auto` | `auto`, `dr0`…`dr7` | Fixed uplink DR; only with `lrw-adr false` | §8 |
| `lrw-region` | — | adds `as923` | AS923-1 channel plan (release builds) | §11 |
| `radio-mode` | `lorawan` | adds `p2p` | Selects the P2P radio | §6 |
| `p2p-frequency`, `p2p-spreading-factor` (default 7), `p2p-tx-power` | — | shell only | P2P radio parameters; must match the Hub | §6, §28 |
| `radio-alarm-ack` | `false` | `true` / `false` | Send alarms as confirmed uplinks (both radios) | §30 |
| `interval-announce` | `24` | 0 (off), 1–168 h | Period of the Info + settings-info re-announce | §37 |
| `cap-sht` | `true` | `true` / `false` | Onboard SHT4x on/off | §36 |

### New commands

| Command | What it does | § |
|---|---|---|
| `get_settings` (31) | The settings-info (key settings + 1-Wire slot types) on request | §14 |
| `get_radio_state` (32) | Radio link state and diagnostics | §24 |
| `get_claim_info` (29) | Serial + claim token over plaintext NFC, only while the claim window is active | §19 |
| `get_basic_info` | Identity bootstrap for the phone (serial, nonce, versions); replaces the NDEF identity record | §18 |
| `SetParam.alarms_replace` | Rewrite the whole alarm table in one message | §17 |

### Changed behaviour

- **Answer pairing.** Every answer can be paired with its request by `seq`: `clock_sync`
  is answered by an Info with the command's `seq`, and a request that fails to decode gets
  `BAD_REQUEST` with its `seq`. `force_send` / `sample` send at once, without the fleet
  jitter (§15).
- **Uplink timing.** A periodic report is sent at a fixed per-device offset of up to 60 s
  after its time slot (was up to 10 s), so devices rebooted together do not collide (§27).
- **Alarm state in every telemetry frame** (`system_flags`, decoded as `alarm_status`),
  and alarm batches are split across frames instead of being cut (§9).
- **LoRaWAN `GetConfig`** leaves out the 1-Wire slot ROMs (4 pages instead of 6 at EU868
  DR0) and sends all its pages after one request. NFC and `GetParam` still return the
  ROMs (§9, §16).
- **Info** no longer carries `lrw_state` and the last-downlink RSSI/SNR. Use
  `get_radio_state` (§24).
- **`factory_reset`** now also clears the pulse counters and the history. `vendor_reset`
  closes the claim window (§35).
- **Region not in the image.** If the stored `lrw-region` is not compiled into the image,
  the radio stays silent and reports `lrw_disabled`. It never falls back to another band (§7).
- **LED.** Red and green are plain GPIO again, as in v1.4.0. The boot carousel no longer
  delays the boot, and NFC answers ~1.2 s after reset (was ~6 s). During a tap only the
  NFC LED shows (§3, §18).
- **Duty cycle.** LoRaWAN holds a frame until the sliding hour has room for it, instead of
  retrying against the MAC's refusal. P2P takes its budget from the EU868 sub-band of
  `p2p-frequency` (§31).

### Upgrading from v1.4.x — action required

1. **Manager-App: update together with the firmware.** v1.5.0 answers on the NFC mailbox
   only. An older app cannot configure a v1.5.0 device. The app must also read
   `Response.page_count`, accept `claim_info` as the answer to `claim_active`, send
   `claim_done` after a claim, and read the link state with `get_radio_state`
   (§13, §18, §19, §24, §35).
2. **NFC needs a powered device.** Battery-less configuration, configuration staged into a
   switched-off unit, and Android tap-to-launch are gone. A generic NFC reader, or a unit
   with a dead battery, shows a blank tag (§18).
3. **Wipe the old NDEF records once.** v1.5.0 never writes the NFC EEPROM, so a unit
   upgraded from v1.4.x keeps its old records, including the plaintext `hio.stck:clm` claim
   token. Clear them with `nfc clear` on a debug build or by writing an empty NDEF message
   from the phone (§18, §35).
4. **Renamed shell parameters** (no aliases; update production and bench scripts):

   | v1.4.x | v1.5.0 | Stored value after the upgrade |
   |---|---|---|
   | `lrw-deveui`, `lrw-appkey` | `radio-deveui`, `radio-appkey` | **kept** (§33) |
   | `lrw-link-check-interval`, `lrw-link-check-fail-rejoin` | `radio-link-check-interval`, `radio-link-check-fail-rejoin` | **not kept**: back to the defaults 5 / 5; set them again if you changed them (§26) |

5. **Decoder and integrations.** Use the v1.5.0 `ttn.js`. Integrations that read decoded
   keys must use `lorawan.radio_link_check_interval` / `radio_link_check_fail_rejoin`. A
   consumer that wants a whole paged answer merges the pages by (DevEUI, fPort, `seq`).
   The deprecated `ConfigDump.page_*` fields are no longer set (§13, §26).
6. **Host duty-cycle etiquette at low DR.** A full `GetConfig` at SF12 takes a large part of
   the hourly airtime. Ask only for the keys you need, do not re-request a missing page at
   once, and allow ≥ 1 h for page assembly at a low DR (§13).
7. **Downgrade.** History pages written by v1.5.0 are not readable by older firmware: run
   `history clear` after a downgrade (§21).
8. **P2P deployments only:** the P2P frame header changed (flag day). Nodes and the Hub must
   be updated together (§28).

### Known limitations

- Not tested on hardware (no 915/923 MHz gateway on the bench): US915/AU915/AS923 on air
  and the 11 B budget tier. Code review and unit tests only (§9, §11, §12).
- P2P: no listen-before-talk yet, no NFC configuration of the P2P radio parameters (§6).
- With `radio-alarm-ack false` (the default) an alarm frame lost on the air is not repeated;
  the alarm state still reaches the network in the next telemetry frame (§9, §30, §32).

### For developers

- Builds need the `sticker-zephyr` `v4.3.0-sticker2-branch` fork (`west update`) and the
  `loramac-node` patch (`west patch apply`, re-run after every `west update`). A LoRaWAN
  build without the patch fails on purpose (§5, §10).
- `debug.conf` ships a lean default (1-Wire, accelerometer, buzzer and PIR off). Re-enable
  one with `-DCONFIG_<X>=y` (§2).

---

## Overview of changes

| Area | Change |
|---|---|
| Buzzer | **New** — alarm-driven melodies (#397, Phase 2 of #338): the buzzer HW variant now sounds automatically while any alarm is active, gated on a new global `alarm-buzzer-mode` config key |
| Debug builds | **New** — 8 independently Kconfig-toggleable subsystems (#395): `debug.conf` ships a lean default (W1, accelerometer, buzzer, PIR off) with real flash/RAM headroom instead of a maximally-squeezed image; `CONFIG_RADIO_LORAWAN=n` disables all radio for bench work. Release builds unaffected. |
| Radio: P2P | **New** — the raw-LoRa point-to-point transport is complete on the node (#118): `radio-mode p2p` pairs with a Proximos `Control.radio.P2P` central over a FIBER modem, with an acknowledged data plane, downlink commands, network-initiated pairing control, per-node TX power, and strict EU868 duty compliance. LoRaWAN is unaffected — both stacks link into the same image and the choice is made at boot. |
| LED | **Changed** — red and green are plain GPIO again, as in v1.4.0. The PWM path from #301 is removed, mainly to save ~2 KB of flash; it also froze in Stop mode on release builds, so the heartbeat and the boot fades were broken. The boot carousel is the v1.4.0 hard blink and no longer holds up the boot; the NFC interaction LED holds the indicator (#467, §3). |
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
| NFC claiming / resets | **Changed** — the claim latch fails closed on a corrupt NVS value; `vendor_reset` closes the claim window and wipes the token; `claim_active` generates a token when none is set and answers `ClaimInfo` (breaking for the Manager-App, was `Ack`); `factory_reset` also clears the counters and the history (#471). See §35. |
| NFC | ~~**New** — last-downlink RSSI / SNR and their age in the NFC `GetInfo` (#409 A2)~~ — superseded before release by `get_radio_state` (#446, §24): the fields moved out of Info. |
| History | **Fix** — record timestamps follow the RTC (F27/F28, H-4): report cadence on wall-clock slots, no capture skipped during a replay, each flash page stamped from the RTC (a reboot / power loss / halt is a gap, not a shift), page header v2 keeps a clock-sync fix-up across reboots, the replay ends with the window's last frame. HistoryFrame protocol unchanged. See §21. |
| LoRaWAN | **Fix** — the M-2 stale-uplink watchdog no longer forces a rejoin while the duty cycle is refusing sends (F29): a rejoin reset the band credits and let the device exceed the 1 % limit. See §22. |
| Radio: P2P | **New** — LoRaWAN ↔ P2P parity, part 1 (#448): the boot / join `Info` + settings-info announce is one `app_radio` path for both radios (P2P announced nothing before); `force_send` / `sample` / `buzzer_play` / `clock_sync` work over P2P and answer as over LoRaWAN; `lrw_join` re-joins P2P without a reboot. See §23. |
| LoRaWAN / P2P / NFC | **New** — `get_radio_state` (#446): one `RadioState` for both radios — link state, radio parameters, both link directions, session, failure streak, duty cycle, counters since boot — on request only, paged like every answer. Info no longer carries `lrw_state` / `last_dl_*`. See §24. |
| Radio: P2P | **Fix / New** — LoRaWAN ↔ P2P parity, part 2 (#449): frames leave in counter order (F-P1-1: one confirmed uplink in flight + 1 s gap), queues survive an unpaired phase, refused telemetry is retried then reset, the fleet jitter and the M-2 watchdog policy are shared, reset tiers clear the P2P pairing, `BUDGET_TOO_SMALL` over P2P, `p2p-*` readable via GetConfig/GetParam. See §25. |
| Radio: P2P / LoRaWAN | **New** — P2P retry backoff and a per-node uplink phase: retry n waits a random 1..2^n s (was a fixed ~2.3 s rhythm), and a periodic report is sent at a stable DevEUI-derived offset inside min(interval − jitter − 1 s, 60 s), on both radios, so nodes rebooted together no longer collide every interval (F-P2P-4 / F-P2P-5). See §27. |
| Radio: P2P | **Changed (wire, flag day)** — decision #22: a `FCtrl` byte in the header (11 → 12 B); telemetry is **unconfirmed and sent once**, except the link check (first report after link-up and every `radio-link-check-interval`-th, every report while WARNING); alarms / answers / history stay confirmed; the RX1 opens after every uplink for a `0x56` of up to 64 B; LoRaWAN-like link supervision (WARNING after 3 failed checks, TX-power step, re-join after `radio-link-check-fail-rejoin`); `p2p-spreading-factor` default 7, join without an SF sweep (last resort after 24 h). See §28. |
| LoRaWAN / P2P | **Renamed** — `lrw-link-check-interval` / `lrw-link-check-fail-rejoin` → `radio-link-check-interval` / `radio-link-check-fail-rejoin`: link supervision is shared by both radios. Wire-compatible (same `lorawan` group fields 13/14); a value stored under the old name is not carried over (defaults 5 / 5). See §26. |
| LoRaWAN / P2P | **Changed (internal)** — one radio work queue in `app_radio` for both backends (doc/plan/439 T2a): release RAM −4.3 KB, no behaviour change (§29). |
| LoRaWAN / P2P | **New / Changed** — `radio-alarm-ack` (#460 T2c): alarms are confirmed on both radios when true. The default, false, sends them unconfirmed on both, which changes P2P, where §28 confirmed every alarm. The Ack retry ladder of a confirmed frame (3 retries, random 1..2^n s) is one `app_radio` path for both radios. See §30. |
| LoRaWAN / P2P | **New / Changed** — one duty-cycle ledger for both radios (#460 T2d): LoRaWAN holds a frame the sliding hour has no room for and sends it the moment it fits, instead of retrying into the MAC's refusal; P2P takes its budget from the EU868 sub-band of `p2p-frequency` (863–865 MHz: 0.1 %, was 1 %). `airtime_hour_ms` in `RadioState` on both radios. The M-2 watchdog waits out a ledger hold instead of rejoining. See §31. |
| LoRaWAN / P2P | **Fix** — alarm frames no longer lost on a burst or to a command's reboot (#462): a batch that does not fit the free slots of the 4-frame alarm queue waits and collects the next edges, and a deferred command action waits for the queued alarm frames and sends a collecting batch first. See §32. |
| LoRaWAN / P2P | **Renamed** — `lrw-deveui` / `lrw-appkey` → `radio-deveui` / `radio-appkey`: both radios use the DevEUI and the AppKey. Shell names only; the NVS keys, proto field names and numbers are unchanged, so the stored identity survives the upgrade and a downgrade. See §33. |
| LoRaWAN / P2P | **New / Changed** — network time through `app_radio` on both radios: P2P asks for the time with uplink `FCtrl` bit 1 `TIME_REQ` (after a link-up without one, the weekly re-sync, `clock_sync`, `clock sync`); the weekly re-sync (#96) now runs on P2P too; `app_clock` has no LoRaWAN code left. Wire-compatible. See §34. |
| Sensors | **New** — `cap_sht` (`sensors` 22, default `true`, #465): the onboard SHT4x temperature/humidity can be switched off like every other sensor (no read, no telemetry fields, no `no_data` alarm, no history channel). The settings-info / `GetSettings` now also carry `cap_buzzer` and `cap_sht`. See §36. |
| LoRaWAN / P2P | **New** — periodic announce (#445): every `interval-announce` hours (default 24, 0 = off) the node re-sends the boot/join `Info` + settings-info, so the network's retained identity and config heal without a reboot. See §37. |

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

## 3. LED: back to GPIO, PWM removed (#301, #466)

#301 (PR #406) moved red (PA5/TIM2_CH1) and green (PA6/TIM16_CH1) onto hardware
PWM, dimmed "on" to 20 % duty and faded them in the boot carousel. That path is
removed again. All three LEDs are plain GPIO, as in v1.4.0.

**Why we dropped PWM:**

- **Flash, the main reason.** PWM served only a cosmetic fade in the boot carousel,
  and it cost about 2 KB of flash. Removing it saves:

  | Image | Flash | RAM |
  |---|---|---|
  | Release, `v1.5.0` (53966b92) | −1944 B (165428 → 163484) | −64 B |
  | Debug, `v1.5.0` (53966b92) | −3712 B (223888 → 220176) | −64 B |
  | Release, `feat-p2p` (647acf21) | −1960 B (182916 → 180956) | 0 |

- **It did not work on release builds.** Release runs with `CONFIG_PM=y`, and every
  `k_sleep()` enters Stop (Stop0 min-residency 100 µs). In Stop, TIM2 and TIM16 have no
  clock, so the PWM output freezes at whatever phase it was in.
  - The release heartbeat (5 ms green at 20 % duty) was mostly invisible, and the boot
    fades stuttered.
  - Every red or green indication outside an NFC session was unreliable. An NFC session
    holds a PM lock, which hid the problem there.
  - Debug builds (`CONFIG_PM=n`) hid it too; the #406 HW check ran on a debug image.
    Found on 5722 and 0413 (release, 2026-09-28).
- **Power.** A GPIO keeps its level in Stop, so the CPU sleeps while an LED is lit.
  Working PWM would need the CPU out of Stop for as long as an LED is on (Sleep, mA-level,
  instead of Stop, µA-level). The 20 % dimming would not win that back.
- **One LED path.** The alternative was GPIO for every indication, with the pins switched
  to the timer only for the boot fade. It would have kept the ~2 KB, needed runtime
  pinmux switching (`PINCTRL_NON_STATIC`) and a PM lock during the carousel. It would
  also have kept two LED paths that behave differently between builds, which is how this
  bug hid. Rejected.

**What changed:**

- `app_led.c`, `app_led.h` and `app.overlay` are the pre-#406 versions again.
  - Gone: `app_led_fade()`, `app_led_heartbeat()`, `app_led_idle_config()` / `_get()` /
    `_pulse()`, the `pwmleds` node, and the TIM2 and TIM16 PWM nodes.
  - `CONFIG_PWM` is off.
- The debug shell loses `ats led fade|heartbeat|idle`; `ats led cycle|switch` stay.
- The boot carousel is the v1.4.0 hard blink: red 500 ms, yellow 500 ms, green 1500 ms,
  with 250 ms gaps.
- The heartbeat is the v1.4.0 one again: a 5 ms green blink every 3 s at full brightness
  (#390: about +6 µA average, accepted).

### Boot carousel no longer blocks; NFC holds the indicator (#467)

`main()` used to sleep 5 s after queueing the 3 s carousel. The sleep came with the move of
the LED to its own thread (`847327f7`); before that the carousel blocked for its own 3 s. It is
gone now:

- The init chain runs while the carousel plays. On the debug P2P bench, NFC serves a phone
  **1.19 s** after reset instead of 6.19 s, which matters most for a phone kept on the tag
  across an NFC-triggered reboot.
- A heartbeat or status blink requested during the carousel goes stale behind it and is
  dropped silently; the next one follows within 3 s. `app_led` no longer logs stale drops or
  a full queue: every request is a periodic or best-effort indication.
- `app_led_hold()` hands the pins to the NFC interaction LED:
  - In every lit NFC state (detected, session, result), `app_nfc.c` takes the hold.
  - It releases the hold once the LED is off and the keep-awake window has closed.
  - While held, the LED thread writes no pin, cuts a running carousel or blink short, and
    drops requests: new ones return `-EBUSY`, and queued ones are discarded.
  - The result: the carousel, heartbeat, status and alarm blinks never mix into a tap.
- Cost: +336 B flash and +24 B RAM on release.
- **HW-verified 2026-09-28** (`ffd1002`, debug P2P bench, SN 2162190413, reboot with RTT
  attached):
  - `NFC: GPO IRQ on PB12 ready` at 1.236 s;
  - no WRN / ERR in the first ~25 s, and nothing from `app_led`;
  - P2P Info + settings-info announced at 1.88 s, first ACK at 3.09 s;
  - visually (release + debug), the full carousel plays at boot and the first heartbeat follows
    it.

  Still to run with a phone:
  - a phone kept on the tag across an NFC-triggered reboot (the carousel is cut, the NFC LED
    clean);
  - an alarm during a `getinfo` loop (no alarm blink while the NFC LED holds).

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
| `sensors` | every capability flag, emitted explicitly incl. `false`: `cap_hall_left` … `cap_accelerometer` (1–9), `cap_buzzer` (19) and `cap_sht` (22) — the last two since #465 (§36) |
| `w1_slot_type` | detected 1-Wire sensor type per logical slot 1..4 |

`w1_slot_type` (`ConfigDump` field 7, packed `repeated uint32`) reports what is
physically attached to each 1-Wire slot. Its values mirror the firmware's single
source of truth, `enum app_w1_slot_type` (`app_w1_slots.h`):

| Value | Meaning |
|:-:|---|
| 0 | empty |
| 2 | dallas (DS18B20) |
| 3 | machine-probe (DS28E17) |

Since #430 step 3 (PR #431) these are the sensor-type registry ids
(`app_sensor_types.yaml`, 1 = motherboard), the same ids as `SensorReading.type`;
before that the values were 1 = dallas, 2 = machine-probe. Adding a new sensor family
is a registry entry + driver in `app_w1_slots.c`; the new value flows onto the wire automatically (the proto
stays a raw `uint32`, so no schema change). A decoder that predates a value renders
it as `type<N>` rather than failing.

**Decoded example** (`ttn.js` output as the LNS sees it, one frame; like every other
config reply, bool fields decode as `0`/`1`):

```json
{ "config_dump": {
    "application": { "interval_sample": 60, "interval_report": 900, "history_enable": 0 },
    "sensors": { "cap_hall_left": 1, "cap_hall_right": 0, "cap_input_a": 1,
                 "cap_input_b": 0, "cap_light_sensor": 1, "cap_barometer": 0,
                 "cap_pir_detector": 0, "cap_w1_sensors": 1, "cap_accelerometer": 0,
                 "cap_buzzer": 0, "cap_sht": 1 },
    "w1_slot_type": ["machine-probe", "dallas", "empty", "empty"] } }
```

**Notes:**

- Size incl. the `APP_PROTO_VERSION` byte: 34 B without 1-Wire (`CONFIG_W1=n`, no
  field 7), 40 B with the four `w1_slot_type` entries, up to ~46 B with large
  interval values. `cap_buzzer` + `cap_sht` (#465) add 6 B (3 B each, tag ≥ 16);
  the worst case is then 40 B without / 46 B with 1-Wire (§36). It fits the EU868 DR0 budget (51 B)
  and the 64 B response buffer; below that it pages (§13).
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

Also fixed: `ats lrw status` (now `ats radio status`) / NFC info during the boot window before `lorawan_start()` no longer
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

**Setup** is three commands and a save. `radio_appkey` is the root of the whole
transport (there is no separate P2P key — the central already has it from
ordinary OTAA provisioning), and an all-zero one makes the radio refuse to
start rather than join under a publicly known key. `radio_deveui` is the node's
on-air identity (#417): an all-zero DevEUI refuses a new join likewise:

```
config radio-appkey <32 hex>
config radio-mode p2p
settings save                    # persists + reboots
ats radio status                 # kind: P2P, app_key: set, state: JOINING|PAIRED
config show                      # radio-deveui: the DevEUI the central registers
```

The three radio parameters (`p2p-frequency`, `p2p-spreading-factor`,
`p2p-tx-power`) must match the Hub's and are shell-only by design — see
`doc/p2p.md` §2.

**What the node does:**

| Area | Behaviour |
|---|---|
| Pairing | On-air join handshake (JoinRequest/JoinAccept, 16 B AES-CMAC tags under `app_key`); the JoinRequest identifies the node by its DevEUI (8 B, MSB-first, #417 — the serial number is no longer on the P2P air). Fast retries for the 120 s boot window, then a slow backoff (≈ one attempt pass per hour) instead of falling silent; each pass sweeps the spreading factors nearest-first, so a node finds a Hub that moved the network SF. Session persisted to NVS so a power cycle never costs a re-join. `join` forces a fresh session; `ats radio unjoin` simulates a never-paired boot. |
| Data plane | AES-CCM under a derived `session_key`, 4 B tag, per-frame counter persisted with a reservation window so a reboot can never reuse a nonce. Confirmed uplinks with up to 3 retransmissions of the byte-identical frame. |
| Link quality | Each Ack carries the RSSI/SNR the central measured on that uplink, surfaced by `ats radio status`. The node's own measurement of each received Ack/command is logged as `dl_rssi`/`dl_snr` and reported by `get_radio_state` (`dl_rssi` / `dl_snr` / `dl_age_s`, §24), as on LoRaWAN. |
| Link state | The node reports its link in the same terms as LoRaWAN (`app_radio` state): paired = healthy, 3+ failed confirmed cycles = warning, boot join = joining, self-heal / RejoinRequest join = reconnect, detached = idle, unprovisioned = disabled. The status LED, `get_radio_state.state` (§24) and `device_status` (`RADIO_LINK_DOWN`) follow it; no uplinks are composed while a re-join replaces the session. |
| Clock | The Ack can carry a Unix-time tail, so a node with no RTC gets wall time from the central — no `clock_sync` command needed. The tail is checked against the same plausibility window as the LoRaWAN DeviceTimeAns (2024–2100, L-5). |
| Downlink commands | `0x56` carries the same protobuf `Command` as LoRaWAN fPort 85, dispatched through the shared handler and answered with a `0x55`. Deferred actions (`settings_save`, `reboot`) execute only **after** that answer has been acknowledged, so a commanded reboot cannot swallow its own response. |
| RX window | The announcing Ack states the pending command's exact on-air length, so the receiver stays on for that frame instead of a 255 B worst case — 548 ms instead of 2514 ms for a short command at SF10. The window keeps a fixed 120 ms after the expected frame (F-P2P-2), so a central's constant Ack lateness cannot cut off an Ack at SF7. |
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
- `get_radio_state` reports `state` DISABLED (§24) and `device_status` carries the
  existing bit 9 `lrw_disabled` (no dedicated bit — `config show` shows the stored region).

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
| Link check in `WARNING` | every `radio-link-check-interval`-th report | **every report** (`radio-link-check-interval 0` still disables link checks) |
| DR fallback | only through the OTAA rejoin (MAC reset to the join DR). LoRaMac's own ADR backoff needs 128 unanswered uplinks for its first step (~32 h at 900 s) | **Recovery ladder**: entering `WARNING` and every later failed check restore the default (max) TX power and drop the DR by one step. A check that succeeds on the lower DR returns to `HEALTHY` with the same session. |
| Rejoin | after `radio-link-check-fail-rejoin` failures in `WARNING` | after that many failures **and** once the ladder is at the floor (region minimum DR, default TX power) |
| Link loss → rejoin (EU868 from DR5, defaults 900 s / LC 5 / 5) | ≈ 9–10 h | ≈ 4–5 h |
| US915/AU915 sub-band | set only as the active channel mask at boot. After ~8 failed joins, JoinRequests spread over all 8 sub-bands (~1 in 8 hit an 8-channel gateway). | also set as the LoRaMac **default** mask and re-applied after each rejoin's MAC re-init |

**Behaviour notes:**

- New log lines: `Link recovery: TX power <a> -> <b>, DR<x> -> DR<y> (payload <n> B)` and `LC FAIL in WARNING (total: n/m, ladder step)`. `ats lrw status` (now `ats radio status`) also prints `tx power: <index> (0 = max)`.
- After a ladder recovery the device stays on the lower DR. With ADR on, the network raises it again from the uplinks it receives. A lower DR means a smaller payload budget (EU868 DR0–2: 51 B), so telemetry may take more frames until then.
- The link-check timeout now starts after the uplink's RX windows closed. It no longer races a LinkCheckAns at DR0/SF12 with a 5 s RX1 delay.
- **Any authenticated downlink is a link-check success** (2026-09-27, parity with P2P, §28): a command, an ADR or DevStatus request or an Ack clears the fail streak and, in `WARNING`, returns the device to `HEALTHY` — not only a LinkCheckAns while a check is outstanding. A device the network is visibly reaching no longer walks down the ladder towards a rejoin. Log: `Link confirmed via downlink`.
- Works together with `lrw-datarate` (§8): a pinned DR is stepped down by the ladder like any other, and the next join re-pins it.
- `ats lrw status` (now `ats radio status`) reports the live DR from the MAC. Before, it showed a stale value after an ADR-off DR change (`lrw-datarate`, a ladder rung).
- Cost: +272 B flash release, +744 B debug, +0 B RAM.

**HW verification (2026-09-23, EU868, ChirpStack v4 on the ProXimos Hub):**
- Ladder runs with ADR off and on: one rung per report DR5 → DR0 on air, with the TX-power rung as +8–9 dB RSSI.
- Recovery on a lower DR with the same DevAddr, both on an injected `lc ok` and after a real NS outage (device disabled on ChirpStack, no rejoin).
- At the floor: a rejoin.
- Combined with `lrw-datarate`: the ladder steps down and the next join re-pins.
- The release image passed too.
- Not HW-tested: the US915/AU915 sub-band fix (no 915 MHz gateway), code review only.

**Power trace of a full link loss (AT-PWR-14, 2026-10-07, release CI `68bf48f`, EU868, ChirpStack v4 at 10.0.0.52, PPK2 3.0 V):**
`interval-report 120` (set over NFC; link-check defaults 5 / 5), ADR on, device disabled on ChirpStack at 14:41:13.
The LNS logs nothing for a disabled device, so the state was read over NFC every 5 min (mailbox `GetInfo`:
`lrw_state`, `last_dl_*`, `uptime_s`; see AT-PWR-14 in the playbook) and each rung was identified on the PPK2
trace by its TX burst (airtime and TX current):

| Time | State | TX on air | What it is |
|---|---|---|---|
| 14:28–15:00 | `HEALTHY` → outage | 61 ms, 28 mA | DR5/SF7 at the ADR-reduced TX power; link check every 5th report (unanswered from 14:41) |
| 15:02:01 | 3rd failed check | 62 ms, 32 mA | → `WARNING`, first rung taken at once |
| 15:03:55 | `WARNING` | 114 ms, 59 mA | default (max) TX power + DR4/SF8 |
| 15:06:03 | `WARNING` | 206 ms, 60 mA | DR3/SF9 |
| 15:07:55 | `WARNING` | 412 ms, 60 mA | DR2/SF10 |
| 15:09:55 | `WARNING` | 824 ms, 57 mA | DR1/SF11 |
| 15:12:02 | `WARNING` | 1483 ms, 57 mA | DR0/SF12 telemetry — the floor |
| 15:12:12 | → `RECONNECT` | — | check failed at the floor, 5th failure in `WARNING` |
| 15:13:01, 15:15:16, 15:18:34, 15:26:57, 15:46:14, 16:21:16 | `RECONNECT` | 1483 ms, 57 mA | SF12 JoinRequests, gaps 59 / 135 / 198 / 503 / 1157 / 2102 s (60 s × 2ⁿ + jitter, cap 3600 s) |
| 17:11:47 | — | — | device re-enabled on ChirpStack |
| 17:16:15 | → `HEALTHY` | 1483 ms + 2 × 2303 ms | next scheduled join (+3299 s) accepted; Info + ConfigDump at SF12, then ADR back to DR5 |
| 17:18–17:31 | `HEALTHY` | 78 ms, 28 mA | 120 s reports at SF7 again |

- The ladder runs to the end: every rung is taken before the rejoin, which comes only at the floor with the
  rejoin budget spent (`app_lrw.c` `on_lc_failure`). The M-2 stale-uplink watchdog did not fire — the device
  transmitted throughout.
- Outage → `WARNING` took 21 min (3 checks at every 5th report), `WARNING` → `RECONNECT` 10 min (one rung per
  report). At the default 900 s this scales to ≈ 2.5 h + 1.25 h.
- Charge of the TX part of a report: SF7 ≈ 1.7 mC → SF8 6.8 → SF9 12.3 → SF10 24.6 → SF11 47 → SF12 85 mC
  (×50 from DR5 at reduced power to DR0 at full power), plus ≈ 8–11 mC of RX windows. A JoinRequest costs
  ≈ 87 mC + ≈ 12 mC for its RX1/RX2 windows (the two small bursts ~5–6 s after each join). In `WARNING` the
  idle band between bursts is unchanged (70.5 µA healthy).
- In `RECONNECT` the floor rises from ≈ 76 µA to ≈ 149 µA: the status LED blinks yellow 10 ms + red 80 ms
  every 3 s (`main.c`, `BLINK_INTERVAL_SECONDS`), ≈ 0.23 mC per blink, ≈ +1.75 mAh/day for as long as the
  network is gone — about as much as the whole healthy idle budget.
- Average excluding the NFC probe windows: healthy 155.6 µA (560 mC/h at 120 s reports); outage hours with
  rejoins every 35–55 min 148–212 µA (534–763 mC/h). At the 120 s grid the outage costs little more than
  the healthy hour; the LED, not the radio, dominates once the rejoin back-off is at its cap.
- Recovery: the network comes back only at the next scheduled join — 4.5 min here, up to ≈ 1 h once the
  back-off has reached its cap. The join was accepted at once, Info and ConfigDump went out at SF12, ADR
  brought the device back to DR5 within two reports, and the last 10 min averaged 159 µA (idle 67.7 µA) — the
  healthy profile.

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
| Info (join, clock-sync, GetInfo) | each field (NFC also `claim_token` / `dev_eui`), then each active alarm (radio: one snapshot for all pages; NFC: a fresh one per page) |
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
| Answer | `Response.config_dump` with the command's `seq`: `application` interval_sample / interval_report / history_enable, every `sensors.cap_*` flag (incl. `cap_buzzer` / `cap_sht`, #465), runtime `w1_slot_type` (1-Wire builds) |
| Size | the boot dump + 2 B for the `seq` (34 B measured without 1-Wire before #465, +6 B for `cap_buzzer`/`cap_sht`, +6 B with the four `w1_slot_type` entries): one frame at EU868 DR0 and up |
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

Mechanism: a new configen attribute `dump_radio: false` (first named `dump_lrw`; renamed
with the yml `radio` transport token) keeps a field in `DUMP_FIELDS` but flags it
`lrw_skip`; `app_cmd_handle_get_config()` skips such a field when the transport is a
radio (LoRaWAN or P2P). A host that merges a complete `GetConfig` into its config copy
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
of the init chain, after every component a command can reach (clock, history,
alarm rules, LoRaWAN, battery, sensors, counters) — ~1.2 s after reset, while the
boot carousel may still be playing (a tap cuts it short, §3) — and just before
the LoRaWAN join. Until then the chip stays
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

From the first lit state until the LED is off and the keep-awake window has closed, the NFC
LED holds the indicator (`app_led_hold`, §3). A tap during the boot carousel cuts it short, and
no heartbeat, status or alarm blink mixes into the tap.

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
not control). Mutators are explicit only: `claim_done` / `ats claim done` /
`vendor_reset` → `done`; `claim_active` / `ats claim active` → `active`.
`device_reset` / `factory_reset` leave the state alone. (`vendor_reset` opened the
window before #471, see §35.)

Upgrading from v1.4.x migrates the old tri-state in place: `unset`/`pending` →
`active`, `consumed` → `done`. Any other stored value, a wrong length or a read
error closes the window (`done`, #471, see §35).

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

> **Superseded before v1.5.0 shipped (#446, §24).** The three fields below and
> `lrw_state` (12) are no longer part of Info (all four are `reserved`); the
> same data, and much more, is read with the `get_radio_state` command. The
> section is kept for the history of the design.

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


## 23. LoRaWAN ↔ P2P parity, part 1 (#448)

Goal (doc/plan/439): the STICKER behaves the same on LoRaWAN and P2P, and the
application layers reach the radio only through `app_radio`.

- **Boot / join announce, one path.** The Info (seq 0) and the #412
  settings-info `ConfigDump` (seq 0), paged for the budget (#425), with the
  pending / deferred logic (DR rise, page-stream end, over-budget re-arm), now
  live in `app_radio` (`app_radio_announce()` / `_run()` / `app_radio_send_info()`).
  LoRaWAN calls it on join success, P2P on every link-up — a boot with a
  persisted pairing and every JoinAccept. The announce is spread randomly over
  up to min(interval_report / 2, 30 s), so nodes rebooted together do not all
  transmit at once; the spread moves the whole sequence, whose order is fixed on
  both radios: **Info → settings-info → data**. An alarm batch (a latched alarm
  re-raised after the reboot included) and the first report wait for the
  announce — alarms also while the link is down — then the alarm goes first and
  the report without a jitter of its own (60 s after the spread at the latest);
  P2P sends queued answers and alarms before telemetry like LoRaWAN. P2P used to announce nothing, so the
  Hub never learned the device info / config of a P2P node without polling.
- **Commands on P2P.** `force_send`, `sample`, `buzzer_play` and `clock_sync`
  are allowed over P2P (only the transport gates stood in the way).
  `clock_sync` goes through `app_radio_clock_sync(seq)`: LoRaWAN keeps
  DeviceTimeReq + the deferred Info; P2P likewise forces no uplink and answers
  with the seq-carrying Info once the next regular uplink's Ack (time tail) has
  been processed. A bare `clock_sync` over NFC still acks the phone.
- **`lrw_join` = `app_radio_rejoin()`** on every path (NFC action, LoRaWAN and
  P2P post-command): on P2P a fresh join handshake without a reboot instead of
  "ignored".
- **Answers as over LoRaWAN.** `force_send` / `sample` answer with their
  telemetry uplink only, on both radios. The P2P central retires a delivered
  `0x56` on a `0x55` with the same seq, and a command with no command-port
  answer on the node's next uplink (proximos PN-4, Hub c43+). Before PN-4 such
  a command was re-delivered and re-measured forever (F-P1-2); the interim
  node-side Ack was dropped again for LoRaWAN parity.

Hardware (0413, P2P night test 2026-09-26/27, Hub c43+): `clock_sync` seq 17 →
Info seq 17 with a synced time; `lrw_join` seq 18 → Ack, JoinRequest, new
session, Info + settings-info announced, no reboot. The boot announce lost
the settings-info to a counter-order replay (F-P1-1) — fixed in #449.


## 24. `get_radio_state` — radio link state and diagnostics (#446)

The link information used to be scattered (Info `lrw_state`, Info `last_dl_*`,
the `ats lrw|radio status` dumps on a debug build) and LoRaWAN-flavoured. Now
one message, `Response.RadioState`, carries it for both radios, and a host asks
for it: **Command `get_radio_state` = field 32** (`GetRadioState { optional uint32
page }`), answered with **`Response.radio_state` = field 14**, on every transport
(read-only, no secrets). Design and field table: `doc/plan/447 - RadioState.md`.

| Group | Fields |
|---|---|
| State | 1 `state` (IDLE / JOINING / HEALTHY / WARNING / RECONNECT / DISABLED) |
| Radio parameters now | 2 `sf`, 3 `datarate` (LoRaWAN), 4 `tx_power_dbm` (conducted, PA-capped) |
| Last downlink (node-measured) | 5 `dl_rssi`, 6 `dl_snr`, 7 `dl_age_s`, 8 `dl_unix_time` |
| Last uplink as heard by the peer | 9 `ul_rssi`, 10 `ul_snr` (P2P Ack), 11 `ul_margin`, 12 `ul_gw_count` (LoRaWAN LinkCheckAns) |
| Session | 13 `dev_addr`, 14 `fcnt_up` |
| Link health | 15 `fail_streak`, 16 `join_attempts`, 17 `duty_blocked_s`, 18 `airtime_hour_ms` (P2P; both radios since §31) |
| Counters since boot | 19 `uptime_s`, 20 `tx_count`, 21 `rx_count`, 22 `retry_count`, 23 `fail_count`, 24 `tx_err_count`, 25 `join_count` |

- **Not part of Info, never announced.** Info `lrw_state` (12) and
  `last_dl_rssi/snr/age_s` (16–18) are `reserved`. The phone sends
  `get_radio_state` over NFC next to `get_info`; nothing of it goes into the boot /
  join announce, so it costs airtime only when a host asks.
- **Paging (#425).** Over a radio the answer is streamed from one snapshot, field
  by field (the downlink group and the uplink pairs travel together; a unit too
  big for the 11 B tier alone is left out); over NFC / vendor / shell the host
  asks with `page`. Normally one frame over NFC and from EU868 DR3 up.
- **Push model.** `app_radio` owns the data: both radio backends report every
  fact as it happens, and every reader takes a consistent snapshot with
  `app_radio_get_status()`. `ats device info` prints the signal, parameters,
  link health and counters.
- `ttn.js` decodes the answer (`radio_state` with `state_name`, `dev_addr_hex`)
  and encodes the command.

Breaking for Manager-App: read the link state with `get_radio_state` instead of
Info fields 12 / 16–18 (older app builds simply see those fields absent).


Hardware, LoRaWAN (0413, EU868, Hub c49 in dual mode, 2026-09-27, feat-p2p
`dd888f3`): `get_radio_state` seq 81 answered in one fPort 85 frame — state
healthy, SF7 / DR5, `dl_unix_time` matching the Hub clock after a
`clock_sync`, margin 17 dB / 1 gateway, counters since boot consistent.

## 25. LoRaWAN ↔ P2P parity, part 2 (#449)

The rest of the parity list (doc/plan/439 T2–T5 subset), all through
`app_radio`:

- **Frames in counter order (F-P1-1).** The P2P central keeps a strict counter
  high-water. A frame sent right after an Ack went unheard (Northbridge RX
  re-arm ~90 ms, NB-3), and its asynchronous Ack retry then came after the
  next fresh frame and was rejected as a replay — on every reboot the
  settings-info was lost, and after a rejoin a command answer, so the Hub
  re-delivered a config. Now **one confirmed uplink is in flight** (a fresh
  frame waits while an Ack retry is pending, not counted as a failure) and
  **1 s separates an Ack window from the next TX** (`P2P_TX_GAP_MS`).
- **Queue while unpaired.** Responses / alarms queued while joining or
  self-healing stay queued and leave on the next link-up (were dropped).
- **Refused telemetry** is re-sent as-is, 8× at most (15 s, or once the duty
  ledger clears), then `app_compose_reset()` — LoRaWAN #219 / #340 M6.
- **Fleet pre-send jitter (#267)** is one `app_radio` policy for both radios
  (P2P had none); force_send / sample still skip it (F14).
- **M-2 stale-uplink watchdog on P2P**, sharing LoRaWAN's policy (F29 duty
  hold) through `app_radio_stale_check()`.
- **Reset tiers:** factory_reset / vendor_reset / lrw_reset call
  `app_radio_reset_link()` — LoRaWAN NVM *and* P2P pairing (the P2P dev_nonce
  and frame counter are kept).
- **`BUDGET_TOO_SMALL` over P2P** for an answer that does not fit, as over
  LoRaWAN (was `UNKNOWN "response too large"`).
- **`p2p-frequency` / `p2p-spreading-factor` / `p2p-tx-power` readable** via
  GetConfig / GetParam on every transport (ConfigDump field 8, GetParam
  `p2p_field` 6); still `writable: [shell]` only (doc/p2p.md §2). A radio
  GetConfig of a device that is not in `radio-mode p2p` leaves the group out,
  so a LoRaWAN DR0 dump keeps its 34 keys / 4 pages; NFC and GetParam still
  return it.

Hardware (0413, 2026-09-27): boot announce Info / settings-info / telemetry and a
live `join` announce each acked on the first try, ~1.1 s apart; no replay.

## 26. Link-check parameters renamed to `radio-link-check-*`

Link supervision (periodic link check, `WARNING` after 3 misses, re-link after
N more) is one policy for both radios, so its two parameters drop the `lrw-`
prefix (ProXimos decision #22):

| Before (v1.4.x) | v1.5.0 | Default | Meaning |
|---|---|---|---|
| `lrw-link-check-interval` | `radio-link-check-interval` | 5 | Link check every N-th report (0 = off) |
| `lrw-link-check-fail-rejoin` | `radio-link-check-fail-rejoin` | 5 | Failures while `WARNING` before the link is re-established (LoRaWAN: OTAA rejoin) |

- **Wire-compatible.** Still `lorawan` group fields **13 / 14** in
  `AppConfigMessage`; only the proto field names change
  (`link_check_interval` → `radio_link_check_interval`,
  `link_check_fail_rejoin` → `radio_link_check_fail_rejoin`). Hosts that
  address fields by number need no change; the TTN decoder uses the new names.
  Same writability as before (`shell`, `nfc`; never over a radio downlink).
- **Shell:** `config radio-link-check-interval <n>`; the old command names are gone.
- **Scope:** the parameters drive the link supervision of both radios —
  LoRaWAN (§12) and, since decision #22, P2P (§28).
- **Decoded JSON keys renamed.** The TTN decoder emits
  `lorawan.radio_link_check_interval` / `radio_link_check_fail_rejoin` for every
  device (v1.4.x included, same field numbers); an integration or Portal mapping
  that reads the decoded keys must follow. The encoder still accepts the old
  `link_check_interval` / `link_check_fail_rejoin` in a SetParam for one release.
- **No NVS migration.** A value stored under the old settings key is not
  carried over: after the update both parameters run on their defaults (5 / 5)
  until set again, and the old key stays unused in NVS. A downgrade to v1.4.x
  likewise reads its defaults.

## 27. P2P retry backoff and a per-node uplink phase (F-P2P-4 / F-P2P-5)

Two Nodes rebooted together ran in lock-step on the one P2P channel and lost
frames to each other every interval (bench 2026-09-27: telemetry 40 ms apart,
all three retries of both colliding). Two causes, two fixes (decision #22
§3.2 and O9, pulled ahead of the rest of #22 by Hynek):

- **Retry backoff (P2P).** Retry n of a confirmed uplink waits a random
  1..2^n s — 1..2 s, 1..4 s, 1..8 s — on top of any duty-cycle block, like
  LoRaWAN's `ACK_TIMEOUT`. The fixed ~2.3 s rhythm before (1 s gap + RX1 +
  0..1 s) kept two colliding nodes colliding.
- **Uplink phase (both radios).** The report cadence stays on wall-clock slots
  (F27), but a periodic report is now sent at a stable offset derived from the
  DevEUI: FNV-1a(DevEUI) mod min(interval_report − fleet jitter − 1 s, 60 s),
  then the #267 fleet jitter. History records keep their slots; only the
  transmission moves. force_send / sample, ad-hoc reports (an alarm trigger)
  and the first report after the boot / join announce take no phase. At a
  60 s interval the bench Nodes 0413 / 5722 send at +2.0 s / +51.0 s. On
  LoRaWAN a periodic report now leaves up to 60 s after its slot (was ≤ 10 s).

## 28. P2P: unconfirmed telemetry, FCtrl header, link supervision (decision #22)

Hynek, 2026-09-27: "zrušíme pro p2p potvrzování telemetrie ihned". With every
uplink confirmed, the Hub's ACK traffic alone (57 ms per ACK at SF7, 1 % duty)
capped a 60 s network at ~10 Nodes; LoRaWAN confirms nothing but its link
checks. Design: ProXimos `plan/control/radio/p2p_link_check.md` §3.1–3.4.

- **Header:** `net_id | dev_addr | frame_type | FCtrl | counter` = 12 B, all
  of it AAD. `FCtrl` bit 0 CONFIRMED (uplink), bit 4 FPending and bit 5 ACK
  (downlink); join frames carry 0. Payload budget 239 B. Shared KAT fixtures
  `tests/ccm/p2p_join_kat.json` / `p2p_data_kat.json`
  (`tests/ccm/p2p_join_kat.py`). Protocol v1 is changed in place: Nodes and
  Hub update together (flag day).
- **Confirmed policy:** CONFIRMED are the link check — the first telemetry
  report after a link-up and every N-th after it (N =
  `radio-link-check-interval`, default 5; 0 = none), every report while
  WARNING — and every alarm, answer / announce and history frame. Other
  telemetry is unconfirmed and sent once; the history backfill covers a lost
  one.
- **RX1 after every uplink**, sized for a `0x56` of up to 64 B that the central
  may send unannounced (interim; a longer one is still announced by the Ack's
  pending bit). Any authenticated downlink is a link success.
- **Link supervision** as on LoRaWAN, same parameters: 3 failed link checks in
  a row → WARNING (session kept, every report confirmed, a central-assigned TX
  power steps 2 dB per failed check up to `p2p-tx-power`);
  `radio-link-check-fail-rejoin` failures in WARNING → self-healing re-join.
  Replaces "8 failed cycles → re-join". With the defaults at 900 s: a link
  check every 75 min, WARNING after ~3.75 h of silence, re-join ~1.25 h later.
- **SF:** `p2p-spreading-factor` defaults to 7 (was 10), the network default on
  both ends. A join / re-join stays on it; the SF7..12 sweep runs only as a last
  resort, one pass after 24 h without a JoinAccept.
- Updates `radio-link-check-*` (§26): P2P reads them now too.

## 29. One radio work queue (doc/plan/439 T2a)

First step of moving the policy both radios share into `app_radio`
(`doc/plan/439 - Radio transport layer.md`, decisions of 2026-09-27 in §3a).

- `app_radio` owns one work queue, `app_radio_work_q()` (thread `radio_wq`,
  4096 B stack, lowest application priority). It is started before `main()`, so
  calibration mode, which brings LoRaWAN up on its own, runs on it too.
- The LoRaWAN and the P2P backend run all their work on it. Each had its own
  4096 B queue before, although only one of them runs.
- No behaviour change. The release image, which has both radios, needs 4352 B
  less RAM (60 296 → 55 944 B, 92.0 → 85.4 %). The debug and P2P bench images
  have only one radio each, so they stay the same.

## 30. Confirmed uplinks on both radios, `radio-alarm-ack` (#460 T2c)

Part of `doc/plan/460 - One implementation per function in app_radio.md` §2.6.

- **New config `radio-alarm-ack`** (bool, default `false`; proto group `alarms`, field 21; writable over shell, NFC and radio).
  - `false` sends alarms unconfirmed, once, on both radios. LoRaWAN did so already. On P2P this amends decision #22 (§28), which confirmed every alarm.
  - `true` sends alarms confirmed on both radios. On LoRaWAN that is a confirmed uplink, retried as below.
- **One retry ladder** in `app_radio`:
  - A confirmed frame without its Ack goes again after a random 1..2^n s (n = the retry), on top of any duty-cycle wait, at most 3 times. Nothing else is sent meanwhile.
  - Given up, the frame counts as sent and as a failed link check (link supervision, §28).
  - P2P resends the same counter (a byte-identical frame). LoRaWAN takes a new FCnt, with LoRaMac NbTrans left at 1.
  - A deferred command action (reboot, settings save) waits for a pending retry on either radio.
- Answers and history frames stay confirmed on P2P and unconfirmed on LoRaWAN; telemetry is unchanged.
- The P2P bench image needs 820 B less RAM, because the P2P retry queue is gone.

## 31. One duty-cycle ledger for both radios (#460 T2d)

Part of `doc/plan/460 - One implementation per function in app_radio.md` §2.7.

- **One exact sliding-hour ledger** in `app_radio` for both radios, the one P2P had (doc/p2p.md §6). A frame goes out only if the air of the trailing hour plus its own fits the budget; otherwise it waits exactly until it fits and goes then. The log line stays `TX duty-cycle blocked for N ms`.
- **LoRaWAN** now checks the ledger before `lorawan_send()`.
  - Before, a frame went to LoRaMac, which refused it ("Duty-cycle restricted") until its fixed hourly credits came back, and the send was retried every 15 s meanwhile.
  - LoRaWAN EU868 gets 1 % over all channels, stricter than the MAC's 1 % per band, so a frame the ledger admits the MAC admits too. Other regions get no limit, but the airtime is still counted.
  - Frames and OTAA JoinRequests are charged their air at the DR they go at: 13 B of LoRaWAN overhead plus pending MAC answers plus the payload.
- **P2P** takes the budget of the EU868 sub-band of `p2p-frequency`: 1 % at 865–868.6 and 869.7–870 MHz (the 868.1 MHz default), 10 % at 869.4–869.65 MHz, 0.1 % anywhere else. **Changed:** 863–865 MHz and 868.6–869.4 MHz got 1 % before.
- Time on air follows LoRaMac's formula, rounded up; some P2P values are 1 ms longer than before (SF12 42 B: 2139 ms).
- `RadioState.airtime_hour_ms` (§24) is filled on both radios.
- RAM: the LoRaWAN-only debug image needs 384 B more (the ledger); the images with P2P are unchanged.
- **M-2 waits out a ledger hold** (fix from the HIL). A held frame waits for its hold in one go, up to the hour. The M-2 watchdog (§22) now takes the known end of that hold as its duty-cycle excuse, and no longer only the last held attempt plus one interval + 3 min. Without the fix, the DR0 bench run rejoined 4 min into a 41 min hold and then every ~5 min: fcnt restarted and nothing was sent for 45 min. The 75 min cap is unchanged.
- The Info / settings-info announce no longer re-encodes into a full answer queue on its 5 s retry.
- Hardware (0413, EU868 DR0, ADR off, 60 s, 2026-09-28): the ledger held at 34.9 s of 36 s. The MAC never refused a frame, M-2 did not rejoin, and the held frame went at the end of the hold on the same session.
- **Fixed: one ledger entry per 75 s slot** instead of per frame. Above 48 frames/h the F-P2P-1 fold of a full ring built one entry that never left the hour and summed all air, so a 60 s cadence with link checks hit the 1 % allowance every ~6 h and went silent ~21 min (TOWER bench 5722, 2026-10-07; LoRaWAN EU868 alike). Frames of the same fixed slot now share an entry (over-count ≤ 75 s, never under-count), at most 49 entries live per hour; RAM +8 B. doc/p2p.md §6.

## 32. Alarm bursts and the post-command reboot (#462)

Found in the Nodes test E6 (2026-09-28): six rules toggled by one `SetParam{…, save=true}` with `alarm-limit 0` gave six one-event batches at once, and the `SetParam`'s own reboot followed 8 s later.

- **Problem 1, a full alarm queue.** `alarm-limit 0` sends every edge as its own batch. The radio's alarm queue holds 4 frames, so a burst of more edges than that dropped the rest (`Alarm queue full; dropped`).
- **Problem 2, the reboot.** A deferred command action (doc/p2p.md, §30) waited only for the command's answer and a pending Ack retry. Alarm frames still queued, and a batch still collecting in its `alarm-limit` window, died in the reboot.
- **Fix, back-pressure.** A batch whose pages do not fit the free alarm slots waits, held like a batch waiting for the link or the boot announce. Later edges join it, so the burst leaves in fewer, fuller frames. Each alarm frame the radio takes from the queue releases it to try again. An empty queue takes a batch of any size, because no queued frame is left to release it.
- **Fix, the drain.** The deferred action also waits while alarm frames are queued or in flight. It sends a batch that is still collecting at once instead of at the end of its window. Its bound is unchanged: 8 s steps, at most 6 deferrals, then the action runs anyway.
- Not in scope: an unconfirmed alarm frame lost on the air (`radio-alarm-ack false`, §30) is still not repeated.
- Tests: `tests/alarm_eval` (burst hold, all pages must fit, empty queue, the early send of a collecting window, a batch held for the link), `tests/radio_common` (the action waits for queued alarm frames and for a collecting batch; taking an alarm frame releases a held batch).

Hardware (0413, P2P, Hub c60, E6 replay 2026-09-28 06:15Z, `alarm-limit 0`). Both phases went through a `SetParam{…, save=true}` from the Hub:
- **Setup** (6 rules that fire at once, applied live): 4 frames queued, then the batch was held for room. Released on dequeue, it went as one frame of 2 events. That is 6 events in 5 frames, and the post-command reboot was deferred once, until they were out.
- **Revert** (6 rules → 1): 6 clear edges in 5 frames, deferred once.
- The Hub decoded all 18 events. Nothing was dropped (the unpatched run had lost 3 of 6).

## 33. `radio-deveui` / `radio-appkey` (the DevEUI and the AppKey are shared)

The DevEUI and the AppKey are not LoRaWAN-only any more. P2P builds its JoinRequest, its session-key KDF (#417) and its uplink phase from them. They are renamed like the link-check parameters (§26), but without losing the stored value:

| v1.4 / before | v1.5 | NVS key | proto (`lorawan` group) |
|---|---|---|---|
| `lrw-deveui` | `radio-deveui` | `config/lrw-deveui` (unchanged) | `deveui = 6` (unchanged) |
| `lrw-appkey` | `radio-appkey` | `config/lrw-appkey` (unchanged) | `appkey = 9` (unchanged) |

- **What changes:** the shell command (`config radio-deveui`, `config radio-appkey`), the `config show` label, the C field (`g_app_config.radio_deveui` / `radio_appkey`) and the log texts.
- **What does not change:**
  - The wire: SetParam, GetParam, GetConfig and the settings-info dump use field numbers.
  - The generated nanopb names (`deveui`, `appkey`), so the Manager-App and Hub code are unaffected.
  - The NVS key. A v1.4 unit keeps its identity across the upgrade, and a downgrade still reads it.
- **Why the NVS key stays:** §26 renamed the key itself. A value under an unknown key is ignored at boot and the default applies, which was harmless there (5 / 5). For the DevEUI and the AppKey it would leave an all-zero identity: P2P refuses to start, and LoRaWAN cannot join. The only way to fix that is a physical touch (NFC or shell) on every unit.
- **configen `stored_as`:** a new parameter attribute naming the key the value is stored under. `filter_nvs_key()` feeds `h_set` / `h_export`. configen refuses two parameters on one key and a `stored_as` that repeats the name. The proto name is kept by the existing `proto_name` override.
- **Other `lrw-*` keys** (region, sub-band, network, ADR, activation, JoinEUI, NwkKey, DevAddr, the ABP session keys, datarate) are read only by the LoRaWAN backend and keep their names.
- **Breaking for scripts that type the shell name.** Production and bench scripts using `config lrw-deveui` / `config lrw-appkey` must switch to the new names. No alias is kept.
- Tests: `scripts/west_commands/tests/test_configen.py` checks that the old key is kept, the shell takes the new name and the proto names stay, plus the `stored_as` validation and clash checks.
- HIL (2026-09-28, STICKER 2162190413, P2P, paired): flashed without an erase from the #462 image to this one and back. The DevEUI and the AppKey survived both ways, under `config radio-*` after the upgrade and `config lrw-*` after the downgrade. The session resumed with no JoinRequest, and the Info and telemetry frames were acked.

## 34. Network time through `app_radio` (`TIME_REQ`)

Before, `app_clock` called the LoRaWAN stack directly: the DeviceTimeReq on join, the weekly re-sync (#96) and the GPS → Unix conversion. P2P took the time only from the Ack tail, when the central chose to send it, and could not ask for it. The weekly re-sync did not run on P2P at all.

Every time request now goes through `app_radio`, whatever the radio (Hynek, 2026-09-28: "zavolat app_radio a to rozhodne").

| Who asks | How |
|---|---|
| A link-up with no network time since boot (LoRaWAN join, P2P JoinAccept or a boot with a stored pairing) | `app_radio_link_up()` |
| The weekly re-sync, armed by the first network time from either radio | `app_clock` → `app_radio_time_request()` |
| `clock_sync` (radio or NFC) | `app_radio_clock_sync(seq)`, unchanged (§23) |
| The shell `clock sync` | `app_radio_time_request()` |

- **`app_radio`** keeps one "time wanted" state. It is set on a request and cleared by `app_radio_time_event()` when a time lands. It asks the backend through `time_request()` on the radio work queue.
- **LoRaWAN** (`app_radio_lrw.c`):
  - the DeviceTimeReq rides the next uplink, at most one per 60 s (#340 L11, now for every request);
  - the DeviceTimeAns is converted from GPS to Unix and passed to `app_clock_set_network_time()`.
- **P2P** (`app_radio_p2p.c`):
  - while a time is wanted, every fresh confirmed uplink carries `FCtrl` bit 1 `TIME_REQ` (doc/p2p.md §3);
  - the next reports go confirmed, at most 3 per request (unchanged, PF-2);
  - a retry keeps its first FCtrl byte for byte;
  - the TX log shows `time-req`.
- **Central:** answers with the Unix tail (B5). The Hub sends the tail on every Ack today (`deliver_time` true) and ignores the bit, so it needs no change. The bit is registered in the Hub-side spec (`p2p_link_check.md` §3.2).
- **`app_clock`** has no LoRaWAN code left.
  - `app_clock_request_sync()`, `app_clock_force_resync()` and `app_clock_handle_downlink()` are gone.
  - `RTC synced from network` is logged by the backend: on LoRaWAN for each DeviceTimeAns, on P2P only for a time it asked for.
    The Hub sends the tail on every Ack, and a log per Ack was noise (found in the HIL).
  - The first network time arms the weekly re-sync.
- **Cost:** release +24 B flash, RAM unchanged. P2P bench +544 B flash, +64 B RAM. A build without a radio is 144 B smaller.
- **Tests:**
  - `tests/radio_common`: a link-up without a time asks and one with a time does not, `app_radio_time_request()` asks on the queue, and a clock_sync wants the time until it lands.
  - `tests/p2p_logic`: `TIME_REQ` rides only confirmed uplinks, the retry keeps its FCtrl, and a fresh frame drops the bit once the time has landed.
- HIL (2026-09-28, STICKER 2162190413, P2P, paired to the Hub; debug bench build with a HIL-only 240 s re-sync period):
  - **Boot:** `TIME_REQ` came up at 6.2 s after the reboot. The first confirmed uplink (Info, counter 6144) carried `time-req`, and its Ack `[time]` set the RTC. There was one `RTC synced` log and no command was needed. The next confirmed frames (6145 Info page, 6146 link-check telemetry) had no `time-req`, and 6147 was unconfirmed again.
  - **`clock_sync` + re-sync:** the Hub queued `clock_sync` seq 242. It arrived on 6150, 4 s before the periodic re-sync, which fired 240 s after the first time. One request served both. Telemetry 6151 went confirmed `time-req`, and the Hub acked it with flags 0x02 (the tail). The Info answer on 6152 carried seq 242 and `unix_time` 2 s before its reception.
  - **Shell `clock sync`:** the next report (6154) went confirmed `time-req`, and the time landed. `clock get` matched host UTC to 2 s (rttt latency, plus the debug build's clock drift).
  - **Re-sync again:** the next periodic re-sync fired 240 s after the first, on the timer armed by the first network time. Telemetry 6156 went confirmed `time-req`, and the time landed.
- HIL (2026-09-28, STICKER 2162190413, LoRaWAN EU868 via the Hub's ChirpStack; debug build with the 240 s re-sync and the time logs raised to WRN, HIL-only):
  - **Join:** `app_radio_link_up()` queued the DeviceTimeReq, and the DeviceTimeAns set the RTC 33 s later, on the next uplink, to host UTC.
  - **Shell `clock sync` + cooldown:** the first request was queued and landed on the next uplink. A second one 3.6 s later logged `cooldown active, ignoring`.
  - **Re-sync:** `Periodic time re-sync` fired 240 s after the first time and queued a DeviceTimeReq, which was answered on the next uplink. `clock get` matched host UTC to 1 s.

## 35. Claiming and reset tiers (#471)

Four changes to the claim lifecycle and the reset ladder.

### Fail-closed claim latch

Before, every unexpected `clm/state` value opened the claim window, so a claimed
unit with corrupted NVS disclosed its `claim_token` again through
`get_claim_info`. Now only a missing key (fresh factory NVS) opens it:

| Stored `clm/state` | Window |
|---|---|
| key missing | `active` (factory default) |
| `0`, `1` (legacy `unset` / `pending`) | `active` |
| `2` | `done` |
| any other byte, wrong length, read error, subtree load failure | **`done`** + `WRN` |

A window closed by mistake is reopened with `claim_active` (owner, NFC) or
`ats claim active`.

### `vendor_reset` closes the window

`vendor_reset` still wipes the `claim_token` (it is not in its persistent tier),
but it now sets the window to **`done`** instead of `active`. Before, the unit
ended up `active` without a token (`get_claim_info` → `NOT_READY "no claim
token"`) and could not be claimed through ATELOS. Now nothing claim-related is
readable after a vendor reset (`get_claim_info` → `NOT_READY "claimed"`) until
the owner re-opens the window with `claim_active`.

### `claim_active` generates the token and answers `ClaimInfo`

| `claim_active` request | Stored token | Token after the reboot |
|---|---|---|
| non-zero `new_claim_token` | any | `new_claim_token` |
| no / zero `new_claim_token` | non-zero | unchanged |
| no / zero `new_claim_token` | zero (after `vendor_reset`) | **new 128-bit token from the CSPRNG** (`sys_csrand_get()`, STM32 RNG) |

The answer is now **`Response.claim_info {serial_number, claim_token}`** (field 9)
with the token that holds after the reboot, instead of `Ack`. It goes over the
secret_key channel (`0x01`) or the shell only (`claim_active` stays
`transports: [nfc, shell]`), so the token is never sent in clear. The phone
forwards it to ATELOS, which is how a unit gets claimable again after a vendor
reset. If the RNG fails, the command answers `NOT_READY "no entropy"` and changes
nothing.

The deferred action (`APP_CMD_ACTION_CLAIM_ACTIVE_SAVE`) runs after the answer is
read, as before, but in a new order: save the config, **then** flip the latch to
`active`, then reboot. If the save fails, the unit reboots with the old token and
the old latch, so it never ends up `active` with a token the phone was told about
but the unit lost.

**Host impact:** the Manager-App must accept `claim_info` as the answer to
`claim_active` (an app that expects `Ack` reports an error although the unit
re-opened the window), and push the token to ATELOS.

### `factory_reset` clears counters and history

`factory_reset` hands a unit to a new owner, so it now also resets the hall /
input pulse totalizers and erases the history ring, as `vendor_reset` already
did. `device_reset` keeps both.

### Reset tiers

| | `device_reset` | `factory_reset` | `vendor_reset` | `settings erase` |
|---|---|---|---|---|
| Transports | NFC, shell | NFC, shell | vendor NFC channel, shell | shell |
| serial, `nonce_counter`, `vendor_token` | keep | keep | keep | wiped |
| `secret_key` | keep | keep | replaced (from the command) | wiped |
| `claim_token` | keep | keep | wiped | wiped |
| Claim window | keep | keep | **→ `done`** | → `active` |
| `vendor_reset_allow` | keep | keep | default | default |
| DevEUI / JoinEUI | keep | keep | wiped | wiped |
| Other LoRaWAN config and keys, `radio_mode` | keep | default | default | default |
| Rest of the config, alarm rules | default | default | default | default |
| Pulse counters, history | keep | **cleared** | cleared | counters wiped |
| LoRaMac NVM (DevNonce, frame counters) | keep | wiped | wiped | — |

### Upgrade from v1.4.x: wipe the old `hio.stck:clm` record

v1.5.0 never writes the NFC user EEPROM (§18), so a unit upgraded from v1.4.x
keeps the old plaintext `hio.stck:clm` NDEF record (serial + `claim_token`),
readable without power even after `claim_done`. Part of the upgrade: wipe the
NDEF area once, with `nfc clear` on a debug build or by writing an empty NDEF
message from the phone.

### Cost and tests

Release +136 B flash, RAM unchanged. `tests/nfc_hw` `test_clm_latch_fails_closed`
(0/1 → `active`, 2 / 0x7F / 0xFF / wrong length / read error → `done`);
`tests/cmd` `test_claim_active` (kept, replaced and generated token, always
`claim_info`, rejected over LoRaWAN without generating).

HW-verified (2026-10-09, STICKER 2162190413, PR #474 head): the shell and NFC
reset tiers (G6a), NFC `factory_reset` (N9), the NFC claim flow (`get_claim_info`,
`claim_active` generating and then keeping a token, `claim_done`,
`new_claim_token`, N10) and the LoRaWAN refusal (`NOT_READY`).

---

## 36. Onboard SHT4x capability flag `cap_sht` (#465)

The onboard SHT4x was the only sensor without a runtime switch: it was always
read, and its temperature/humidity were always on the wire (`null` on a fault).
A host could neither see nor turn it off. `cap_sht` adds the switch.

| | |
|---|---|
| Key | `cap_sht` = `sensors` **22**, `bool`, default **`true`**; shell `config cap-sht`, NVS `cap-sht` |
| proto_id | 22, not 20: draft PR #407 (analog inputs) claims `sensors` 20/21 |
| Access | the same as every other `cap_*` flag |
| Reset tiers | not persistent: a device/factory/vendor reset restores `true` |
| Upgrade | the key is new; a unit with no stored value gets the default `true`, so it behaves as before. An older image ignores the key on a downgrade |

With `cap_sht` = `false`:

- `app_sensor_sample()` does not call `app_sht4x_read()`, and the skipped read is
  left out of the wedged-I2C accounting (`i2c_tried` / `i2c_failed`).
- Telemetry carries no `temperature` / `humidity` fields (the onboard group is
  absent, not `null`).
- The no-data watchdog does not watch the onboard temperature/humidity, so no
  `no_data` alarm fires. Turning the flag off while a `no_data` alarm is latched
  sends its deactivate edge. A threshold rule on onboard temperature/humidity sees
  `NaN` and stays inactive.
- History drops the onboard channels (`history_sensors` bits 0/1) from the active
  mask, like any other channel whose capability is off, instead of storing `NaN`.
- The production test (`ats`) and the calibration still read the SHT4x directly,
  regardless of the flag.

The battery ADC stays the only sensor without a switch: undervoltage,
`battery_level` and `Info.battery` depend on it.

**Settings-info.** The boot settings-info (§4) and `GetSettings` (§14) now carry
`cap_buzzer` (19) and `cap_sht` (22) as well, so the host sees every capability
flag. `cap_buzzer` reports the effective value: `app_sensor_init()` clears it when
`cap_pir_detector` is also set. The two flags add 6 B. The worst case (every
flag `true`, `interval_sample` 3600, `interval_report` 86400) is 40 B without
1-Wire (measured by `cmd/test_build_config_status_worst_case_dr0`) and 46 B with
the four `w1_slot_type` entries, so it still fits one EU868 DR0 frame (51 B).
There is no hard 51 B limit anyway: the settings-info pages when it does not
fit (§13).

**Decoder:** `ttn.js` learns `_SEN_NAMES[22] = "cap_sht"` (decode + encode).

**Consumers:** the Manager-App needs a `cap_sht` toggle; Hub / Portal get the new
`sensors` field, and `cap_buzzer` now also arrives in the boot announce.

Tests: `compose/test_cap_sht_gating`, `history/test_cap_sht_off_drops_onboard_channels`,
`alarm_eval/test_cap_sht_gates_onboard_nodata`,
`cmd/test_build_config_status_worst_case_dr0`, the settings-info field counts
(12 → 14) and the `ttn.js` decoder tests.

HW-verified (2026-10-09, STICKER 2162190413, PR #475 head): `cap_buzzer` and
`cap_sht` in the boot settings-info (L4b) and the `cap_sht` gating (S10b).

---

## 37. Periodic Info + settings-info announce (#445)

The network side (LNS, Hub central, Portal) keeps a retained copy of each
node's identity, firmware and effective configuration. Until now it was
refreshed only by the boot/join announce (§4, §23) or by polling (GetInfo,
`GetSettings`, §14). The copy went stale after a config change without
a reboot (Hub, 2026-09-26: `interval_sample` stayed 60 after a set-config to
45), after a node-remove / node-add or a DB restore, and on a node that never
reboots.

- **New parameter `interval-announce`** (`application.interval_announce`,
  proto_id 8): hours, default **24**, range 1..168, **0 = off**. Writable over
  shell, NFC and the radio, like the other intervals.
- **Behaviour** (`app_radio.c`, one path for both radios):
  - every boot/join announce (`app_radio_announce()`) also arms the periodic
    one; a re-join therefore restarts the period;
  - the period ends at a random point of its last 10 % (24 h: 21.6–24 h), so
    a fleet powered on together drifts apart and every node still announces
    at least once per period; the next period is anchored on this announce;
  - it sends the same frames as the boot/join announce: `Info` (seq 0), then
    the settings-info `ConfigDump` (seq 0), paged (#425), deferred while the
    answer queue is full or a page stream runs, re-armed when the budget drops
    under a queued frame. A duty-cycle hold delays it like any queued answer;
    nothing is dropped;
  - unlike the boot/join announce it holds no data: there is no spread, and
    alarms and telemetry are not held (the frames simply queue as answers,
    which go ahead of telemetry);
  - when the link is not ready at the end of the period (joining,
    reconnecting, unpaired) nothing is sent and nothing retries: every return
    to a ready link runs `app_radio_announce()`, which announces and restarts
    the period anyway;
  - nothing is reset: counters, history and the link state stay as they are.
- **A changed value** takes effect at the next arming. `settings save` reboots,
  so a persisted change applies at once; a staged change without save applies
  at the end of the running period (from 0, only at the next boot/join).
- **Cost:** release +456 B flash, +64 B RAM. Two answers per period, i.e. ~2 frames per day at the default
  (more when paged at a low LoRaWAN DR).
- **Tests:** `tests/radio_common`, both profiles: repeats within [0.9, 1.0] of
  the period, off at 0, deferred to the link-up while the link is down,
  restarted by a re-join; the delay bounds. `ttn.test.js`: the
  `set_param application.interval_announce` round-trip. Manual: L4d.
- **HIL so far** (2026-10-09, STICKER 2162190413, LoRaWAN EU868, PR #472 head):
  boot announce Info → settings-info → telemetry, then the first periodic
  announce after `interval-announce 1` carrying a staged `interval-sample`.
- **Not covered yet:** the full L4d run on the `v1.5.0` head (link-down defer,
  `interval-announce 0`) and P2P.

---

*Applies to firmware v1.5.0. Reflects changes relative to v1.4.0 — see `doc/version 1.4.md` for the full v1.4.0 feature set this builds on.*
