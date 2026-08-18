# Manual Test Plan — STICKER firmware v1.4.0

> Target firmware: **v1.4.0**. The scenarios below cover the v1.4.0 feature set
> (remote control protocol, protobuf telemetry/alarms, RTC sync, history store-and-forward).

A canonical, versioned checklist of manual / hardware test scenarios for release
verification. Each task carries a ready-to-use **prompt for Claude**: copy it into a
Claude Code session that has the sticker attached over `rttt` and access to TTN
(TTS MCP) and ChirpStack, and Claude will drive the scenario autonomously and report
pass/fail. The tester only watches.

> Agent-driven automated/HIL execution of these scenarios (bench intake, tooling, coverage
> matrix, adversarial & power extensions): see [`automated-test-playbook.md`](automated-test-playbook.md).

---

## How to use

1. Flash a **debug build** (RTT log + shell) onto the device — see `CLAUDE.md` build variants.
2. Attach the device over `rttt`. The repo ships `.rttt.yaml` (`STM32WLE5CC` @ `0x20000800`),
   so a bare `rttt` connects. Keep `rttt` running in your own terminal — it is reliable only
   in the user's TTY.
3. For each task: copy the **Prompt for Claude** block into a Claude session, let Claude run
   the scenario, then tick the checkbox based on Claude's verdict.
4. Record the firmware version under test and the date at the top of your run.

## Prerequisites & setup

- **Build**: debug variant. Some shell commands used below (`ats cmd lrw <hex>`,
  `ats cmd nfc <hex>`) exist only when `CONFIG_APP_CMD_DEBUG_SHELL=y` (debug builds).
- **RTT shell**: available commands are `ats`, `config`, `clock`, `history`, `settings`,
  `join`, `send`. Note there is **no** `ats device` command — device info is obtained via the
  GetInfo downlink command (fPort 85), not the shell.
- **Networks**: the device must be provisioned on **both** TTN and ChirpStack. Claude sends
  downlinks / reads uplinks through the TTS MCP tools (`send_downlink`, `send_downlink_json`,
  `get_uplinks`, `get_device`, …) for TTN, and through the ChirpStack API for ChirpStack.
- **Safety rules**: never `pkill -9 JLink`; `rttt --reset` is broken (reboot via shell/downlink
  instead); flash without `--erase` to preserve NVS keys/config unless a clean state is required.
- **RTT shell timing gotchas**: (1) `ats sensors sample` with a 1-Wire machine probe attached can
  take well over 15–20 s (multiple sequential 1-Wire/I²C reads) — the RTT tooling may return an
  empty result before the real output lands even with a generous timeout; prefer `ats cmd lrw
  08032200` (local `GetInfo`) when only battery/alarm/status fields are needed, since it responds
  fast. (2) Commands that persist to NVS or reboot (`reboot`, `settings save`,
  `device_reset`/`factory_reset`/`vendor_reset`, `w1 enroll`) are **deferred actions** when
  injected via `ats cmd lrw <hex>` / `ats cmd nfc <hex>` — the debug-shell injection path
  deliberately does not execute them (only real transport-delivered commands do), so use the
  native shell command (`settings save`, etc.) or a real downlink to actually exercise the
  persist/reboot behavior; the local injection path is for protocol/response-shape checks only.
- **History backend**: check `history info`'s `backend:` field before planning any multi-step
  history scenario (H6/H8/H9/K5/K5b/C1-style checks) — a **RAM** backend means any `settings
  save`-triggered reboot wipes accumulated records, so sequence config changes that need a reboot
  *before* starting to accumulate records, not in the middle.

### fPort map (quick reference)

| fPort | Direction | Payload |
|-------|-----------|---------|
| 2  | uplink   | Periodic / event Telemetry (protobuf) |
| 3  | uplink   | AlarmReport (protobuf, rate-limited) |
| 10 | uplink   | Calibration data (24 B, fixed ABP keys) |
| 85 | both     | Command / Response (protobuf), history replay |

### Ready-to-use hex downlinks (fPort 85)

From `doc/version 1.4.md` §1 — paste straight into a downlink or `ats cmd lrw <hex>`. The leading
byte is the `seq` and is echoed in the reply.

| Command | Hex |
|---|---|
| `get_info` | `08032200` |
| `settings_save` | `08043200` |
| `clock_sync` | `08056200` |
| `force_send` | `08064a00` |
| `sample` | `0805aa0100` |
| `reboot` | `08083a00` |
| `reset_counters` (hall-left + input-a) | `0807520408011801` |
| `set_param`: ADR on, `interval_report`=120 s, `alarm_0`=onboard temp 5–30 °C (hyst 1) | `0801121d0a022001120218782a131a1100000100000000a0400000f0410000803f` |
| `lrw_reset` | `0801820100` |
| `lrw_join` | `08018a0100` |

### Legend

- **Goal** — what the task verifies.
- **Observable** — the concrete signal a tester (and Claude) looks for.
- **Prompt for Claude** — paste verbatim.

---

## General

### G1 — Firmware version & build type

**Goal:** Boot reports the correct version and build type.
**Observable:** RTT boot line `Firmware version: X.Y.Z (MAIN, release)` and `Build time: ...`.

**Prompt for Claude:**
> Over the `rttt` RTT shell, reboot the sticker (`ats lrw reset`, or power-cycle if you can't),
> then read the boot log. Confirm a line matching `Firmware version: <major>.<minor>.<patch>
> (<build_type>, <release|debug>)` appears, followed by `Build time:`. Report the exact version
> string and whether the build type and debug/release flag are what we expect for this build.

- [ ] Pass

### G2 — Boot LED carousel

**Goal:** Visual boot indicator runs.
**Observable:** Red (≈0.5 s) → Yellow (≈0.5 s) → Green (≈1.5 s), ~5 s total right after boot.

**Prompt for Claude:**
> Trigger a reboot over the RTT shell (`ats lrw reset`). I (the tester) will watch the LEDs.
> Tell me exactly what sequence to expect (colors, order, approximate timing) and at what point
> in the boot log it starts, so I can confirm the carousel visually. Collect the boot log to
> correlate timing.

- [ ] Pass

### G3 — Shell reachable over RTT

**Goal:** Interactive shell works over RTT.
**Observable:** `help` lists `ats`, `config`, `clock`, `history`, `settings`, `join`, `send`.

**Prompt for Claude:**
> Connect to the RTT shell and run `help`. Confirm the root commands `ats`, `config`, `clock`,
> `history`, `settings`, `join`, `send` are all present. Then run `ats` with no args and confirm
> the `led`, `sensors`, `lrw` (and in debug builds `cmd`) subcommands are listed. Report anything
> missing.

- [ ] Pass

### G4 — GetInfo command (device info)

**Goal:** Device answers GetInfo with version, serial, uptime, clock, build type, battery.
**Observable:** `Response.Info` on fPort 85 with `fw_major/minor/patch`, `build_type`,
`serial_number`, `uptime_s`, `unix_time`, `debug`, `battery` (mV).

**Prompt for Claude:**
> Send a GetInfo command to the joined device. In a debug build you may inject it locally with
> `ats cmd lrw <hex>` over the RTT shell (the device queues the Response on fPort 85), or send it
> as a real downlink via the TTS MCP `send_downlink` on fPort 85. Then read the fPort 85 uplink
> via `get_uplinks` and decode the `Response.Info`. Confirm `fw_*` matches the boot version,
> `serial_number` is non-zero, `uptime_s` is plausible, `build_type`/`debug` are correct, and
> `battery` is a plausible supply voltage in mV (e.g. ~3000 on a 3 V bench supply). `ats device
> info` prints the same `Battery:` line locally.

- [ ] Pass

### G4b — Sample command (fresh reading on demand)

**Goal:** `sample` takes a fresh reading, sends it as fPort-2 telemetry **and** (over NFC) returns
the readings synchronously; over LoRaWAN it emits no fPort-85 body.
**Observable:** NFC → `Response.sample` (a `Telemetry`) with the current readings. LoRaWAN → a
fPort-2 `Telemetry` frame and **no** fPort-85 response.

**Prompt for Claude:**
> In a debug build, inject `ats cmd nfc 0805aa0100` over the RTT shell and confirm the printed
> `RESP` decodes to `Response.sample` (`Telemetry`) whose values match `ats sensors sample`.
> Then inject `ats cmd lrw 0805aa0100`: confirm the `RESP` body is **empty** (no fPort-85 reply)
> and a fPort-2 telemetry frame is sent (when joined). Note: with `cap-w1-sensors` /
> `cap-accelerometer` enabled but no hardware attached, `app_sensor_sample()` is slow — disable
> those caps for a quick bench check.

- [ ] Pass

### G5 — Reboot command

**Goal:** Remote reboot works and is acknowledged.
**Observable:** `Response.Ack` on fPort 85, then the boot log repeats (`Firmware version: ...`).

**Prompt for Claude:**
> Send a Reboot command as a real downlink on fPort 85 (TTS MCP `send_downlink`). While watching
> the RTT log, confirm the device sends a `Response.Ack`, then reboots — i.e. the `Firmware
> version:` boot line reappears. Report the time between the downlink and the reboot.

- [ ] Pass

### G6 — Reset keeps identity (`settings device-reset` / DeviceReset)

**Goal:** `settings device-reset` (renamed from `settings reset`, #299) and the DeviceReset command
(same wire id as the old, single FactoryReset) restore application config + alarm rules to defaults
but **keep** the device identity (`serial-number`, `secret-key`) and LoRaWAN provisioning
(`lrw-deveui`, keys, region, …), so the unit stays provisioned and on the network (issue #108).
**Observable:** changed app/alarm values back to defaults; DevEUI/keys/serial unchanged; device
stays joined / rejoins with the same credentials.

**Prompt for Claude:**
> First capture the identity + a couple of app values via `config` (e.g. `config lrw-deveui`,
> `config serial-number`, `config interval-report`) and change `interval-report` to a non-default.
> Then run `settings device-reset` over the RTT shell. After reboot confirm: `interval-report` is
> back to default, but `config lrw-deveui` / `config serial-number` are **unchanged**, and the device
> rejoins with the same DevEUI. Repeat using a DeviceReset downlink on fPort 85 and confirm the
> `Response.Ack` precedes the reboot and identity again survives.

- [ ] Pass

### G6a — Reset ladder's narrower tiers (`factory_reset` / `vendor_reset` / `set_secret_key`, #299)

**Goal:** `factory_reset` (new, narrower than device_reset above) keeps identity only and drops the
LoRaWAN session/keys — the device must re-join after it. `vendor_reset` keeps only
`serial-number` + `vendor-token` (goes through the live settings API, not a raw storage erase — only
`history` is raw-erased), and is refused unless the caller supplies a replacement `secret-key` in the
same call, or if `vendor-reset-allow` is false. `set_secret_key` rotates `secret-key` over the
already-encrypted nfc/shell channel, then reboots so the new key is live (#322); an all-zero
replacement is refused.
**Observable:** `factory_reset` — identity survives, DevEUI/keys/region reset to defaults, device
re-joins. `vendor_reset` without a key, or with `vendor-reset-allow false`, is refused (no reboot,
nothing erased). `vendor_reset` with a key — only serial+vendor-token survive, new secret_key is
live after reboot. `set_secret_key` — the device saves and cold-reboots, and the new key is in
effect once it comes back (old key no longer decrypts); an all-zero key is rejected with
`BAD_REQUEST` and nothing is saved or rebooted (#322).

**Prompt for Claude:**
> `settings factory-reset` over the RTT shell: confirm `config serial-number`/`config secret-key`
> survive but `config lrw-deveui` and the LoRaWAN keys reset to all-zero/defaults, and the device
> re-joins. Then `config vendor-reset-allow false` + `settings save`, and confirm
> `settings vendor-reset <32-hex-key>` is refused (shell reports failure, no reboot). Set
> `config vendor-reset-allow true` + `settings save`, then `settings vendor-reset` with **no**
> argument — confirm it's rejected (missing key) — then with a key: confirm after reboot
> `config serial-number`/`config vendor-token` are unchanged but everything else (incl.
> `config secret-key`, which should now read the supplied key) is back to defaults/blank.

- [ ] Pass

### G6a-NFC — vendor_reset over the `hio.stck:vnd` channel (#299, #316)

**Goal:** the same `vendor_reset` operation as G6a above, but driven over NFC through the
vendor-token-authenticated record (`hio.stck:vnd`) instead of the shell. Since #316 this is a normal
protobuf `Command` (`vendor_reset`, `transports: [vendor]`) dispatched on the vendor transport — the
same generic Command/Response path as `hio.stck:cmd`, only decrypted/encrypted with `vendor_token`,
and never reachable over `hio.stck:cmd` or LoRaWAN.
**Observable:** the tag holds a plaintext info record, then a `hio.stck:vnd` write, then a
`hio.stck:rsp` reply (`Ack` on success, `Error{NOT_READY}` if `vendor-reset-allow` is false,
`Error{BAD_REQUEST}` for a missing key) — the actual reset only fires after the phone acks the reply
(same ack-before-reboot handshake as every other reset), never immediately from the tap. With
`vendor-reset-allow=false`, first send `set_param{ application{ vendor_reset_allow=true } }` over
`hio.stck:vnd` (always accepted — the field is `writable: [vendor]` and its write is not gated on the
current value), then re-send `vendor_reset`.

**Needs HIL re-verification for #316** (the prior 2026-07-13 result was for the removed `hio.stck:rst`
magic-byte channel, which no longer exists). The frame is now a protobuf `Command{ vendor_reset{ key } }`
sealed under `vendor_token` — see the `nfc_crypto` `test_vendor_channel_vector` golden vector
(`VND_REQ_PLAIN`/`VND_REQ_WIRE`) for the exact construction — injected into ST25DV memory via the
`nfc write <offset> <hex>` shell command (`ats cmd nfc <hex>` would NOT work — it injects a *plaintext*
`Command` straight into `app_cmd_handle` over the NFC transport, bypassing the tag/encryption and the
vendor transport). Confirm: (1) a valid request is recognized ("vendor command record"), decrypted,
dispatched on the vendor transport, accepted, and the reply written — the device does **not** reset
until a `hio.stck:ack` record is written back, at which point the deferred action fires and
`serial_number`/`vendor_token` survive with the new `secret_key` live; (2) with `vendor-reset-allow=false`,
rejected (`Error{NOT_READY}`), and the `set_param(vendor_reset_allow=true)` recovery step above then
unblocks it; (3) a stale/reused nonce is rejected by `decrypt()` same as the `cmd` channel. Long
`nfc write` hex strings silently truncate — split into multiple writes at sequential offsets.

- [x] Pass (HIL-verified via hand-crafted frame, 2026-07-13)

### G6b — Full erase un-provisions (`settings erase`)

**Goal:** `settings erase` is the only path that wipes the whole NVS partition (identity + LoRaWAN
credentials included). It is **shell-only** — no LoRaWAN or NFC command can reach it (issue #108).
**Observable:** after `settings erase` the DevEUI/keys/serial are gone (back to all-zero/defaults);
the device no longer joins until re-provisioned.

**Prompt for Claude:**
> ⚠️ Destructive — confirm it's acceptable to un-provision this bench unit first (you'll need to
> re-flash provisioning afterwards). Run `settings erase` over the RTT shell. After reboot confirm
> `config lrw-deveui` and `config serial-number` are wiped and the device fails to join. Then
> re-provision and confirm join works again.

- [ ] Pass

### G6c — Re-flash preserves provisioning (J-Link regression)

**Goal:** A normal `west flash` (no `--erase`) never touches the `storage` NVS partition, so
re-flashing firmware keeps the device provisioned (issue #108 partition-map contract).
**Observable:** identity + LoRaWAN credentials survive a firmware re-flash.

**Prompt for Claude:**
> Capture `config lrw-deveui` / `config serial-number` / `config interval-report` (set the latter
> to a non-default and `settings save`). Re-flash the firmware with a plain `west flash` (no
> `--erase`). After reboot confirm all three values are **unchanged** and the device rejoins with
> the same DevEUI — i.e. the flash preserved the `storage` partition at `0x3C000`.

- [ ] Pass

### G7 — Heartbeat log

**Goal:** Main loop is alive.
**Observable:** `Alive` logged roughly every 3 s.

**Prompt for Claude:**
> Watch the RTT log for ~15 s and confirm an `Alive` line appears at roughly 3 s intervals.
> Report the observed interval and whether it is steady.

- [ ] Pass

### G8 — Watchdog recovery (optional)

**Goal:** A fatal fault leads to a controlled reboot.
**Observable:** `Rebooting in 60 seconds due to fatal error` then reboot (if `CONFIG_WATCHDOG`).

**Prompt for Claude:**
> This is an optional/destructive check. Tell me whether the current build has the watchdog
> enabled (look for the relevant Kconfig/log evidence). If a safe way exists to provoke a fatal
> error on the bench, describe it; otherwise mark this task as not-applicable for this build and
> explain why.

- [ ] Pass / N/A

---

## LoRaWAN

### L1 — OTAA join on TTN

**Goal:** Device joins via OTAA on TTN and reaches HEALTHY.
**Observable:** RTT `Using OTAA activation`; `ats lrw status` → state HEALTHY; join event visible
on TTN.

**Prompt for Claude:**
> Ensure the device is configured for OTAA against TTN. Trigger a join (`join` over the RTT shell,
> or reboot). Confirm the RTT log shows `Using OTAA activation` and the join completes. Run
> `ats lrw status` and confirm the state is HEALTHY. Cross-check on TTN via the TTS MCP that a
> join-accept / first uplink was received for this device. Report DR/RSSI/SNR from the status.

- [ ] Pass

### L2 — OTAA join on ChirpStack

**Goal:** Same join succeeds against ChirpStack.
**Observable:** `Using OTAA activation`; HEALTHY; join visible in ChirpStack.

**Prompt for Claude:**
> Repeat the OTAA join but pointed at ChirpStack (switch the device's network keys/config if
> needed and note what you changed). Confirm `Using OTAA activation`, HEALTHY state via
> `ats lrw status`, and that ChirpStack shows the join and a first uplink (use the ChirpStack
> API). Report any differences from the TTN run.

- [ ] Pass

### L3 — ABP join

**Goal:** ABP activation works.
**Observable:** RTT `Using ABP activation`; uplinks accepted.

**Prompt for Claude:**
> Configure the device for ABP (set DevAddr / NwkSKey / AppSKey via `config`, note the values).
> Reboot and confirm the RTT log shows `Using ABP activation` (and not OTAA). Send an uplink
> (`send`) and confirm it is received on the network server with the matching DevAddr.

- [ ] Pass

### L4 — GetInfo-on-join

**Goal:** Device autonomously sends its Info right after joining.
**Observable:** A `Response.Info` on fPort 85 appears before the first periodic Telemetry.

**Prompt for Claude:**
> Force a fresh join (reboot or `join`). Watching the network server uplinks (TTS MCP
> `get_uplinks`), confirm the device sends a fPort 85 `Response.Info` autonomously before the
> first fPort 2 Telemetry. Decode it and confirm the version/serial match the device.

- [ ] Pass

### L5 — Periodic telemetry

**Goal:** Telemetry is sent on the configured interval.
**Observable:** fPort 2 uplinks every `interval_report` seconds; RTT `Snapshot complete; next
report in <N> s`.

**Prompt for Claude:**
> Read the current `interval_report` via `config interval-report`. Optionally set it low (e.g.
> 60 s) for a faster test and note the change. Confirm via the RTT log (`Snapshot complete; next
> report in <N> s`) and via fPort 2 uplinks on the network server that telemetry is emitted at
> that cadence. Report the measured interval vs configured.

- [ ] Pass

### L6 — Multi-frame telemetry

**Goal:** A snapshot larger than one payload is split across frames.
**Observable:** Multiple fPort 2 uplinks back-to-back with ~3 s gap for one report.

**Prompt for Claude:**
> Enable enough sensors/fields (via the `cap_*` config flags) that one telemetry snapshot exceeds
> the single-frame payload budget for the current DR. Trigger a report (`send` or ForceSend) and
> confirm on the network server that the snapshot arrives as multiple fPort 2 frames spaced by
> ~3 s. Report how many frames and the spacing.

- [ ] Pass

### L7 — Link check

**Goal:** LinkCheckReq is sent periodically and answered.
**Observable:** Every 5th message carries a LinkCheckReq; LinkCheckAns within 10 s; visible in
RTT LC logs.

**Prompt for Claude:**
> With the device HEALTHY and a gateway in range, send several uplinks (or wait through several
> periodic reports). Confirm from the RTT log that roughly every 5th message includes a link
> check and that a LinkCheckAns is received (look for the `LC OK` log lines). Report the observed
> margin/gateway count if logged.

- [ ] Pass

### L8 — LC state machine (HEALTHY → WARNING → RECONNECT)

**Goal:** Link-check failures escalate state correctly.
**Observable:** RTT `LC FAIL in HEALTHY (streak: n/3)` → `State: HEALTHY -> WARNING` after 3
consecutive fails; `LC FAIL in WARNING (total: n/5)` → `State: WARNING -> RECONNECT` after
`lrw-link-check-fail-rejoin` fails; `ats lrw status` mirrors the counters.

**Prompt for Claude:**
> On a debug build, drive the failures deterministically with `ats lrw lc fail` (space them ~2 s
> apart — the hook reuses one work item, rapid injects coalesce); set
> `config lrw-link-check-interval 0` + `settings save` first so real link-checks don't reset the
> streak. Watching the RTT log / `ats lrw status`, confirm HEALTHY → WARNING (3 consecutive) →
> RECONNECT (after `lrw-link-check-fail-rejoin` more). Then `ats lrw lc ok` and confirm one success
> returns WARNING → HEALTHY. (Alternatively provoke real failures by taking the gateway out of
> range — note the method.) Report the observed thresholds.

- [ ] Pass

### L9 — Rejoin with exponential backoff (OTAA)

**Goal:** In RECONNECT the device retries join with growing backoff.
**Observable:** RTT `Rejoin in <s> s (attempt <N>)` with increasing delay (60 → ×2 → capped
3600 s); a successful rejoin returns to HEALTHY with counters reset. ABP logs
`ABP mode - cannot rejoin, staying in WARNING`.

**Prompt for Claude:**
> Drive an OTAA device into RECONNECT (L8). Confirm from the RTT log the rejoin timer arms with
> exponential backoff — `Rejoin in 60 s (attempt 1)`, then 120/240/… on repeated failures (base
> 60 s, ×2, capped 3600 s). With the gateway reachable, confirm the rejoin succeeds (real
> JoinRequest/JoinAccept) and returns to HEALTHY — cross-check the LNS shows a fresh session (TTN:
> f_cnt resets to 1; ChirpStack: a new `dev_addr` via `GetActivation`). On an ABP device, confirm
> rejoin is skipped with `ABP mode - cannot rejoin, staying in WARNING`.

- [ ] Pass

### L9b — Recovery after a rapid rejoin storm (bench congestion / duty-cycle stress)

**Goal:** A short burst of back-to-back reboot/rejoin-triggering events (e.g. several `reboot` /
`device_reset` / `settings save` cycles fired minutes apart while iterating on other tests)
should not leave the device stuck outside HEALTHY indefinitely — it must keep retrying with
backoff and eventually recover once a clear TX/RX window is available, bounded by the L9 backoff
schedule (base 60 s, ×2, capped 3600 s).
**Observable:** After the storm, `ats lrw status` cycles through `RECONNECT`/join attempts and
returns to `HEALTHY` within the expected backoff-schedule bound — it should not sit at
`devaddr=00000000`/`fcnt up` frozen far beyond one full backoff cap (3600 s) with zero visible
join attempts. A filtered RTT log (`lorawan|Join|MlmeConfirm`) taken on a **freshly-booted**
session (so it can't be stale) should show periodic `JoinReq`/`MlmeConfirm` activity, not silence.
**Prompt for Claude:**
> Deliberately trigger 4–5 reboot/rejoin events within a short window (a mix of `reboot`,
> `device_reset`, and `settings save`, spaced ~1–2 min apart — e.g. while exercising G5/G6/S-caps
> in the same session). Afterward, check `ats lrw status` repeatedly over several minutes. If it
> stays in `RECONNECT` well past the point a single 60 s (or even a few escalated) backoff cycle
> should have resolved it: (a) confirm the device is otherwise alive (GetInfo/config keep working
> locally — this is NOT a crash), (b) pull a **fresh** (post-reboot) filtered log for
> `lorawan|Join|MlmeConfirm` and report exactly what it shows (e.g. `MlmeConfirm failed: Rx 2
> timeout` indicates a JoinReq WAS sent but no JoinAccept arrived — check whether this is
> gateway/RF congestion — e.g. a shared bench gateway busy with other devices' traffic, visible via
> the LNS's raw gateway-frame log — rather than the device failing to transmit at all). Note
> whether a genuine PPK2 power-cycle (not just a soft reboot) changes anything. If the device
> never recovers on a bench with an otherwise-idle/dedicated gateway, this is a candidate real
> defect in the rejoin/backoff logic or the EU868 join-channel duty-cycle accounting under rapid
> repeated join attempts — worth its own issue with the captured log evidence.

- [ ] Pass

### L10 — Downlink reception

**Goal:** Device receives and logs downlinks.
**Observable:** RTT `Port <p>, Flags 0x.., RSSI <r> dB, SNR <s> dBm` on each downlink.

**Prompt for Claude:**
> Send a downlink on a known fPort via the TTS MCP `send_downlink` (e.g. a GetInfo on fPort 85).
> Confirm the RTT log shows the `Port <p>, Flags ..., RSSI ..., SNR ...` line and the port matches.
> In a debug build, also verify the local inject path with `ats cmd lrw <hex>`. Report RSSI/SNR.

- [ ] Pass

### L11 — `ats lrw` shell commands

**Goal:** LoRaWAN shell utilities work.
**Observable:** `ats lrw status` prints state/FCnt/DR/RSSI/SNR; `ats lrw check` sends with link
check; `ats lrw reset` resets counters + DevNonce and reboots.

**Prompt for Claude:**
> Run `ats lrw status` and report the fields. Run `ats lrw check` and confirm an uplink with a
> link check is sent (verify on the server). Finally run `ats lrw reset` and confirm the frame
> counters / DevNonce reset and the device reboots. (Reset is destructive to FCnt — confirm it's
> OK on this bench.)

- [ ] Pass

### L12 — No legacy bitmap on fPort 1

**Goal:** The legacy v1.3.x bitmap is no longer emitted; the device transmits only the new ports.
**Observable:** A report produces a protobuf uplink on **fPort 2** only — no fPort 1 frame
(`doc/version 1.4.md` §2). The decoder retains a legacy fPort-1 branch for old captures only.

**Prompt for Claude:**
> Trigger a telemetry report (`send` or ForceSend) and inspect the network-server uplinks. Confirm
> the report arrives on **fPort 2** and that **no fPort 1** frame is emitted. Report the ports seen.

- [ ] Pass

### L13 — Configurable link-check cadence & rejoin threshold

**Goal:** `lrw-link-check-interval` and `lrw-link-check-fail-rejoin` drive the state machine.
**Observable:** `ats lrw status` reports `healthy->warning: n/3` and `warning->reconnect: n/M`
where M = `lrw-link-check-fail-rejoin`; a LinkCheckReq is sent every Nth uplink (0 = none).

**Prompt for Claude:**
> Set e.g. `config lrw-link-check-interval 1`, `config lrw-link-check-fail-rejoin 3`,
> `settings save`. Confirm `ats lrw status` shows `warning->reconnect: n/3`. With interval 1,
> confirm a link check rides every uplink; with interval 0, confirm none are requested. Then drive
> failures (L8) and confirm RECONNECT now triggers after 3 (not 5) WARNING fails.

- [ ] Pass

### L14 — Late link-check in RECONNECT does not wedge (HIGH-1 regression)

**Goal:** A link-check result arriving while in RECONNECT is ignored and never cancels the rejoin
(the root cause of the old "TX stops" bug).
**Observable:** In RECONNECT, `ats lrw lc ok`/`fail` is logged as ignored; state stays RECONNECT,
the rejoin timer keeps running and the device rejoins.

**Prompt for Claude:**
> Drive the device into RECONNECT (L8). While it waits for the rejoin timer, inject `ats lrw lc ok`
> and `ats lrw lc fail`. Confirm via `ats lrw status` the state stays RECONNECT (not back to
> HEALTHY/WARNING) and the rejoin still fires on schedule → HEALTHY. This must NOT wedge or stop TX.

- [ ] Pass

### L15 — Radio-silent on zero DevEUI (#98, #175)

**Goal:** An un-provisioned device (DevEUI all-zero) does not burn power on impossible joins, and
(#175) does not even bring up the radio.
**Observable:** RTT `DevEUI is all-zero: skipping LoRaWAN bring-up (radio-silent, #98/#175)`;
`ats lrw status` state **DISABLED**; **no** `lorawan_start`/region/JoinRequest activity at all, no
rejoin timer; on a power trace (PPK2, J-Link detached) **no boot radio burst** in the first second.

**Prompt for Claude:**
> Set `config lrw-deveui 0000000000000000`, `settings save`. After reboot confirm the boot RTT log
> shows `skipping LoRaWAN bring-up (radio-silent, #98/#175)` (debug build) and **no** region /
> `lorawan_start` / join lines follow — `app_lrw_init` takes the radio-silent path. Confirm
> `ats lrw status` = `DISABLED`. Restore a real DevEUI + `settings save` and confirm it joins again.

- [ ] Pass

> **HW-verified (2026-06-23, #175):** debug build on Base Compact, `lrw-deveui = 00…00` — RTT showed
> `skipping LoRaWAN bring-up (radio-silent, #98/#175)`, no radio/region/join logs, `ats lrw status`
> = DISABLED. The `DIAG_NO_RADIO` build (skips the whole `app_lrw_init` call) confirmed via a
> sentinel log that `app_lrw_init` is never even entered.

### L16 — Release-FW sustained TX (TX-stop regression, decisive)

**Goal:** The original *"TX stops after 4–5 messages"* bug stays fixed under its exact repro
conditions (release build masks-off: no `CONFIG_LOG`, `PM=y`).
**Observable:** On the LNS, f_cnt climbs continuously well past 5 (≥10–15) with link-check active
(`lrw-link-check-interval 5`), no stall — including across the msg-5/10 link-checks.

**Prompt for Claude:**
> Provision OTAA, `config lrw-link-check-interval 5`, `settings save`. Flash the **plain release**
> build (no debug overlay) and let it run. Watch the LNS uplinks (TTS/ChirpStack) and confirm f_cnt
> climbs continuously past ~13 with no stop. (Release has PM=y → SWD sleeps; reflash via a
> `west flash` retry loop or power-cycle.) Report the highest f_cnt reached.

- [ ] Pass

---

## Sensors

### S1 — Temperature & humidity (SHT4x)

**Goal:** Internal T/RH are sampled and reported.
**Observable:** `ats sensors sample` prints temperature/humidity; telemetry fields `temperature`
(°C×100) and `humidity` (%RH×2).

**Prompt for Claude:**
> Run `ats sensors sample` over the RTT shell and report the temperature and humidity readings;
> sanity-check they're plausible for room conditions. Then trigger a telemetry uplink (`send`)
> and confirm the decoded fPort 2 payload carries `temperature` and `humidity` consistent with
> the shell sample (remember the ×100 / ×2 scaling).

- [ ] Pass

### S2 — Accelerometer orientation

**Goal:** Orientation (1–6) is detected.
**Observable:** Telemetry field `orientation` in 1–6; changes when the device is reoriented.

**Prompt for Claude:**
> Tell me to place the device in a couple of known orientations. For each, run `ats sensors
> sample` and read `orientation`, and confirm it lands in 1–6 and changes between orientations.
> Confirm the same value appears in a fPort 2 uplink. Report the mapping you observed.

- [ ] Pass

### S3 — Accelerometer any-motion count

**Goal:** Any-motion events increment a counter.
**Observable:** Telemetry field `accel_motion_count` increases when the device is moved.

**Prompt for Claude:**
> Read `accel_motion_count` via `ats sensors sample`. Ask me to gently shake/move the device,
> then re-read and confirm the count increased. Confirm the new value also appears in a fPort 2
> telemetry uplink.

- [ ] Pass

### S4 — Free-fall → alarm

**Goal:** Free-fall raises an `accel-motion` alarm.
**Observable:** AlarmReport on fPort 3 with source `accel-motion`, edge ACTIVATE; orange LED blink.

**Prompt for Claude:**
> Ask me to perform a short, safe free-fall (drop onto a cushion). Confirm an AlarmReport arrives
> on fPort 3 with an event whose source decodes to `accel-motion` and edge ACTIVATE, and that the
> RTT log / orange LED reflect the alarm. Decode and report the event fields.

- [ ] Pass

### S5 — PIR motion

**Goal:** PIR detects motion and counts it.
**Observable:** Telemetry `motion_count` increments (requires `cap_pir_detector`).

**Prompt for Claude:**
> Confirm `cap_pir_detector` is enabled in config (enable and save it if needed, noting the
> change). Read `motion_count`, ask me to wave in front of the PIR, then re-read and confirm the
> count increased. Confirm it propagates to fPort 2 telemetry.

- [ ] Pass

### S6 — 1-Wire thermometers EXT1/EXT2

**Goal:** External DS18B20 probes are read.
**Observable:** Telemetry `ext1_temperature` / `ext2_temperature` (°C×100); requires
`cap_w1_sensors`.

**Prompt for Claude:**
> Confirm `cap_w1_sensors` is enabled and the probes are wired. Run `ats sensors sample` and
> report `ext1`/`ext2` temperatures; sanity-check them. Ask me to warm one probe (e.g. by hand)
> and confirm that channel's reading rises. Confirm the values reach fPort 2 telemetry.

- [ ] Pass

### S6b — First-ever 1-Wire sensor enrollment on an untaught unit (regression guard, #M7)

**Goal:** A fresh unit with `cap_w1_sensors` on but **no slot taught yet** can still discover and
enroll its first sensor — the sensor driver devices must be initialised whenever the capability
is on, not only after a slot is already taught (that was a chicken-and-egg bug: the scan callback
needs the sensor device ready, but the device was only made ready once a slot was taught).
**Observable:** On a unit with all four `sensorN-rom` still all-zero, `w1 scan` finds the
connected sensor and `w1 enroll <slot>` successfully binds it (not "0 slot(s) bound" / `-EAGAIN`
forever); `ats sensors serial` shows a non-zero DS18B20/Machine Probe count.
**Prompt for Claude:**
> Start from (or force via `settings device-reset`/`factory-reset`) a state where
> `cap_w1_sensors` is on but all `sensorN-rom` are zero. With a DS18B20 or machine probe
> connected, run `w1 scan` and `w1 enroll <slot>`. Confirm the sensor is found and bound (not "no
> devices found" / "0 slot(s) bound"), `config sensorN-rom` shows the new ROM, and after
> `settings save` the sensor's readings reach fPort 2 telemetry.

- [ ] Pass

### S7 — 1-Wire machine probe MP1/MP2

**Goal:** Machine-probe temp/humidity and tilt flag work.
**Observable:** Telemetry `mp1_temperature`/`mp1_humidity` (+`mp1_flags` tilt bit), same for MP2;
requires `cap_w1_sensors`.

**Prompt for Claude:**
> Confirm `cap_w1_sensors` is enabled and the probe(s) connected. Read MP1/MP2 temp and
> humidity via `ats sensors sample` and sanity-check. Ask me to tilt the probe and confirm the
> tilt-alert bit in the probe flags sets. Confirm the fields and flags reach fPort 2 telemetry.

- [ ] Pass

### S8 — Hall sensors (left/right)

**Goal:** Hall counters and flags track magnet events.
**Observable:** Telemetry `hall_left_count`/`hall_right_count` and `*_flags` (bit0 act, bit1
deact, bit2 active); requires `cap_hall_left`/`cap_hall_right`.

**Prompt for Claude:**
> Confirm the hall capabilities are enabled. Read the current `hall_left_count`/`hall_right_count`,
> ask me to pass a magnet over each hall sensor, then confirm the respective counter increments
> and the active/notify flag bits behave on activate/deactivate. Confirm via fPort 2 telemetry.

- [ ] Pass

### S9 — Input A/B

**Goal:** GPIO inputs count edges.
**Observable:** Telemetry `input_a_count`/`input_b_count` and flags; requires `cap_input_a`/
`cap_input_b`. Note: PIR and Input share pins — if PIR is enabled, Input is skipped.

**Prompt for Claude:**
> Confirm `cap_input_a`/`cap_input_b` are enabled **and** that PIR is disabled (they share GPIO —
> if PIR is on, inputs are skipped). Read the input counters, ask me to toggle each input, and
> confirm the counters and flags update. Confirm via fPort 2 telemetry. Flag the PIR/Input
> mutual-exclusion explicitly in your report.

- [ ] Pass

### S10 — Barometer & light sensor

**Goal:** Pressure/altitude and illuminance are read.
**Observable:** Telemetry `pressure` (hPa×1000), `altitude` (m×10), `illuminance` (lux/2);
requires `cap_barometer` / `cap_light_sensor`.

**Prompt for Claude:**
> Confirm `cap_barometer` and `cap_light_sensor` are enabled. Run `ats sensors sample` and report
> pressure, altitude and illuminance; sanity-check against ambient. Ask me to cover/uncover the
> light sensor and confirm illuminance changes. Confirm all three reach fPort 2 telemetry.

- [ ] Pass

### S11 — Battery voltage

**Goal:** Voltage field is reported.
**Observable:** Telemetry `voltage` (V×50) present every uplink.

**Prompt for Claude:**
> Confirm every fPort 2 uplink includes a `voltage` field and that it decodes to a plausible
> battery voltage (note: on some builds the battery callback is stubbed to 255/unknown — if so,
> report that rather than failing).

- [ ] Pass

### S12 — Calibration mode

**Goal:** Both magnets at boot enters calibration mode and sends calibration uplinks.
**Observable:** RTT `Both magnets detected at boot — entering calibration mode`; 24 B uplinks on
fPort 10 every 30 s using fixed ABP keys.

**Prompt for Claude:**
> Ask me to hold magnets on both hall sensors, then trigger a reboot. Confirm the RTT log shows
> `Both magnets detected at boot — entering calibration mode` and that the device emits 24-byte
> uplinks on fPort 10 roughly every 30 s (verify on the network server; note the fixed ABP keys).
> Confirm normal telemetry is suppressed while in calibration mode. Also verify the
> config-flag path: `config calibration on` + reboot logs `Calibration flag set in config`.

**Regression note (#352):** this path reads the hall pins via `app_calibration.c`'s own
`read_hall_gpio()`, which never had the double-`GPIO_ACTIVE_LOW`-negation bug that `app_hall.c`
had — so dual-magnet detection should be unaffected by the #352 fix. Re-run this scenario once
after the fix regardless, since it's the one boot-time path that reads raw hall GPIOs outside
`app_hall.c` and is cheap to confirm didn't regress alongside it.

- [ ] Pass

### S12b — `enter_calibration` command (remote)

**Goal:** The `enter_calibration` command (#141) drops the device into calibration mode over
LoRaWAN and NFC, with the same end state as the magnet/flag path.
**Observable:** `Response.Ack` on fPort 85; after the deferred action (≈8 s) the device cold-reboots
and the boot log shows `Calibration flag set in config — entering calibration mode`, then fPort-10
calibration uplinks. The flag is one-shot, so the next reboot returns to normal.

**Prompt for Claude:**
> Send the `enter_calibration` command (downlink hex `0801920100` on fPort 85 via the TTS MCP, or
> over NFC). Confirm the Ack, then that the device reboots into calibration mode (boot log
> `Calibration flag set in config — entering calibration mode`, fPort-10 uplinks). Reboot once more
> and confirm it returns to normal telemetry (the flag self-clears). Note: a shell `ats cmd lrw
> 0801920100` only acks and reports the deferred action without executing it — drive the real entry
> via an actual downlink or the NFC tag.

- [ ] Pass

### S13 — `ats sensors` utilities

**Goal:** Sensor shell helpers work.
**Observable:** `ats sensors reset` zeroes counters; `serial` prints sensor serials; `check
<sensor> [timeout]` monitors for changes.

**Prompt for Claude:**
> Run `ats sensors serial` and report the serial numbers. Run `ats sensors reset` and confirm the
> counters (hall/input/motion/accel) zero out on the next `ats sensors sample`. Then run
> `ats sensors check <sensor> [timeout]` for one sensor, stimulate it, and confirm the monitor
> reports the change.

- [ ] Pass

---

## History

### H1 — Enable / disable

**Goal:** Master history switch works.
**Observable:** `history enable on|off`; reflected in `history info`; config `history_enable`.

**Prompt for Claude:**
> Run `history info` and note the enabled state. Toggle with `history enable off` then
> `history enable on`, confirming `history info` reflects each change. Confirm the `history_enable`
> config value matches.

- [ ] Pass

### H2 — Sensor selection

**Goal:** Per-sensor recording can be toggled.
**Observable:** `history sensors` lists sensors + selection; `history sensors <name> on|off` toggles.

**Prompt for Claude:**
> Run `history sensors` and report the list with current selection. Toggle one sensor off then on
> (e.g. `history sensors temperature off` / `... on`) and confirm the listing updates accordingly.
> Note which sensors are gated by capability flags.

- [ ] Pass

### H3 — Record & read

**Goal:** Records accumulate and can be read back.
**Observable:** `history count` grows over time; `history read [N]` lists records with timestamps
and values; `history info` summary.

**Prompt for Claude:**
> With history enabled and at least one sensor selected, note `history count`, wait through a
> couple of sampling intervals, and confirm the count increased. Run `history read 5` and confirm
> records list timestamps and sensor values that look consistent with recent samples. Report the
> `history info` summary (backend, count, oldest/newest).

- [ ] Pass

### H4 — Statistics

**Goal:** Per-sensor min/max/avg are computed.
**Observable:** `history stats` table with min/max/avg/n per sensor.

**Prompt for Claude:**
> Run `history stats` and confirm it prints a per-sensor table with min/max/avg and a sample
> count, and that the values are consistent with the records seen in `history read`. Report the
> table.

- [ ] Pass

### H5 — Clear

**Goal:** Buffer can be erased.
**Observable:** `history clear` → `history count` returns 0.

**Prompt for Claude:**
> Run `history clear`, then confirm `history count` reports 0 and `history info` shows an empty
> buffer. (Destructive to stored history — confirm it's OK on this bench.)

- [ ] Pass

### H6 — ReqHistory replay

**Goal:** Stored records replay over the air on demand.
**Observable:** ReqHistory downlink (fPort 85) → multiple `Response.HistoryFrame` uplinks on
fPort 85 with `frame_index/frame_count/t0_unix/interval_s/present/samples`.

**Prompt for Claude:**
> Ensure several records are stored (let it sample for a bit, or check `history count`). Send a
> ReqHistory command on fPort 85 (real downlink via TTS MCP, or local `ats cmd lrw <hex>` in a
> debug build). Confirm the device streams the records back as one or more fPort 85
> `Response.HistoryFrame` uplinks. Decode at least one frame and confirm `frame_count`,
> `t0_unix`, `interval_s`, the `present` mask and the packed samples reconstruct the records seen
> via `history read`.

- [ ] Pass

### H6b — ReqHistory replay at a high data rate (large-payload regression guard, #C1)

**Goal:** The replay's per-frame staging buffer is sized to the DR payload budget, not a stale
fixed bound — a prior bug (`HISTORY_SAMPLES_MAX` fixed at 48 B while the real per-frame budget at
higher DRs is 150–220+ B) overran a stack buffer and could hard-fault the device. This scenario
pins that class of regression.
**Observable:** With ADR converged to a high DR (EU868 DR3+, ideally DR5) and ≥15–20 stored
records, `ReqHistory` returns one or more `HistoryFrame`s with **no reset/hard-fault** and the
device stays fully responsive (RTT shell/GetInfo keep working) throughout and immediately after.
**Prompt for Claude:**
> Enable ADR (`config lrw-adr true`, `settings save`) and let it converge to DR3 or higher (check
> `ats lrw status`). Accumulate ≥15–20 history records (raise `interval_report` beforehand if
> needed for speed — note this is a **RAM-backend debug build**, so any `settings save` reboot
> wipes accumulated records; do config changes needing a reboot *before* starting the count).
> Send `req_history` covering the whole stored range (`to_unix` must be a valid `uint32` — use
> `4294967295` for "no upper bound", not a JS-native huge number, or the device correctly rejects
> it with `Error{BAD_REQUEST,"integer too large"}` rather than crashing). Confirm: (a) the device
> does not reset (RTT `status`/a shell command still responds immediately before and after), (b)
> the returned frame(s) decode correctly and byte-match `history read`'s records one-for-one
> (temperature/humidity/timestamps). Report the DR used, record count, and frame count.

- [ ] Pass

### H7 — ReqHistory empty window

**Goal:** Requesting an empty range returns a clean error.
**Observable:** `Response.Error` with code `HISTORY_UNAVAILABLE` on fPort 85.

**Prompt for Claude:**
> Clear history (`history clear`) or request a time window with no records, then send ReqHistory.
> Confirm the device replies with a single `Response.Error` whose code decodes to
> `HISTORY_UNAVAILABLE`, and that it does **not** emit empty HistoryFrames.

- [ ] Pass

### H8 — Backend persistence (RAM vs Flash)

**Goal:** Flash/NVS backend survives reboot; RAM backend does not.
**Observable:** With flash backend, `history count` is non-zero after a reboot; with RAM backend
it resets to 0.

**Prompt for Claude:**
> Determine the active history backend (RAM vs flash/NVS) from `history info` and the build
> config. Store some records, note `history count`, reboot (`ats lrw reset`), and confirm the
> count behaves as the backend implies — preserved on flash, reset to 0 on RAM. Report which
> backend is active and the observed behavior.

- [ ] Pass

### H9 — Pressure / illuminance / orientation / accel-motion channels (#311)

**Goal:** the 4 new history channels (barometer pressure, light-sensor illuminance, accelerometer
orientation, accelerometer any-motion event count) record and read back correctly, gated on their
own capability flags (`cap_barometer`, `cap_light_sensor`, `cap_accelerometer` — the last one gates
both `orientation` and `accel-motion`), and are absent (not recorded) when the capability is off.
**Observable:** `history sensors` lists all 4 new names; enabling them + capturing records values
consistent with a live sensor read; disabling the capability drops the channel from both the
selection list and stored records.

**Prompt for Claude:**
> Confirm the board's `cap_barometer`/`cap_light_sensor`/`cap_accelerometer` are on (`config` shell
> or `get_config`). Run `history sensors` and confirm `pressure`, `illuminance`, `orientation`,
> `accel-motion` are all listed and available. Enable all 4 (`history sensors pressure on`, etc.),
> `history capture`, then `history read 1` and confirm the printed values are in a plausible range
> (pressure ~950–1050 hPa, illuminance a small non-negative number, orientation 0–5, accel-motion a
> non-negative count) and roughly match a fresh sensor reading (`sample` command or `get_info`).
> Then flip `cap_accelerometer` off via `config` + `settings save`, reboot, and confirm
> `orientation`/`accel-motion` no longer appear in `history sensors` and are silently dropped from
> the selection mask (no crash, no stale values). Report all observations.

- [ ] Pass

---

## Alarms

> **Alarms are dynamic rules** in 16 fixed slots (`0…15`). Arm/change/clear them locally with the
> `alarm` shell command (`alarm set <i> <source> <quantity> <args>`, `alarm new …`,
> `alarm clear <i>|all`, `alarm list`), or over the air with **SetParam** writing the slot config
> parameter `alarm_<i>` (a packed 17-byte rule as hex) — the same message works on **fPort 85
> (LoRaWAN)** and **NFC**. There are no per-source `*-notify-*` flags or `*_alarm_*` config keys
> any more, and no separate `AlarmRule`/`ReqAlarmRules` commands. See `doc/version 1.4.md` §7 for
> the source/quantity enums, the kinds (threshold / state / count) and the packed-slot layout.
> `alarm-limit` (rate-limit) still applies globally.
>
> **#348: `dwell` is a per-rule dwell/hold duration in seconds, not a hysteresis band** —
> `alarm-notif-time` and the illuminance-only `alarm-light-confirm-delay` config keys are gone.
> `dwell` behaves differently per kind (see `doc/version 1.4.md` §7 for the full per-kind
> description); the scenarios below (A1, A3, A5, A6) each call out what `dwell` does for that kind
> and give a concrete recipe to observe it, including the "canceled by an early revert" case that a
> naive check-once-at-expiry implementation would get wrong.
>
> **Shell syntax gotcha:** `alarm new <source> <quantity> <kind-args>` / `alarm set <i> <source>
> <quantity> <kind-args>` — `<source>`/`<quantity>` are **names** (e.g. `onboard`,
> `temperature`), not numeric indices, and there is **no** literal `threshold`/`state`/`count`
> keyword in the actual command line — the kind is inferred from the quantity. Threshold args are
> `<lo> <hi> [dwell]` (e.g. `alarm new onboard temperature 0 20 1`); adding a `threshold` token as
> if it were a positional argument shifts everything and fails with "wrong parameter count".
>
> **Bench tip — testing alarms without a network join:** the alarm-poll loop in the main
> application only runs while the LoRaWAN state is HEALTHY, so on a device that hasn't joined (or
> has dropped to RECONNECT) `alarm new`/`alarm set` + the local **`alarm poll`** command (force a
> sample + one evaluation pass now) still fully exercises rule activation — check the result via
> `alarm list` (shows `active=0/1`) or a local `GetInfo` (`ats cmd lrw 08032200`, works without a
> radio session) and read its `active_alarms`/`device_status_flags`. This decouples alarm-engine
> correctness from LoRaWAN connectivity entirely — useful when the bench gateway is congested or
> the device is mid-rejoin.

### A1 — Threshold alarm (onboard temperature): band + dwell

**Goal:** Crossing a plain `[lo, hi]` band raises an alarm only after it has stayed outside the
band continuously for `dwell` seconds; a value that dips back inside the band before `dwell` elapses
must NOT fire, and must NOT get credit toward a later attempt (the dwell window resets).
Deactivation is always immediate.
**Observable:** AlarmReport on fPort 3, source `onboard`, quantity `temperature`, edge + side
(LO/HI); RTT alarm log; red LED while active.

**Prompt for Claude:**
> Arm `alarm set 0 onboard temperature <lo> <hi> <dwell>` with bounds near the current room
> temperature and `dwell` a few seconds (note the values); confirm with `alarm list`. Then, in order:
> (1) push the sensor just past the bound and back inside within less than `dwell` seconds — confirm
> **no** AlarmReport fires (the dwell was interrupted); (2) push it past the bound and hold it there
> for longer than `dwell` — confirm an AlarmReport fires only after roughly `dwell` seconds have
> elapsed, with source `onboard`/quantity `temperature` and the correct side (LO/HI); (3) bring the
> value back inside the band and confirm the alarm clears **immediately** (no matching dwell on the
> way down). Decode and report all three events/non-events with their timing.

- [ ] Pass

### A2 — Threshold alarms: humidity / pressure / 1-Wire slots

**Goal:** The other analog quantities behave like temperature (band + dwell + immediate clear).
**Observable:** AlarmReport fPort 3 with the matching source and side.

**Prompt for Claude:**
> For each present analog quantity (onboard humidity/pressure, and 1-Wire slot s1…s4 temperature/
> humidity), arm `alarm set <i> <source> <quantity> <lo> <hi> [dwell]`, stimulate a crossing held past
> `dwell`, and confirm an AlarmReport on fPort 3 with the correct source/quantity and LO/HI side after
> the dwell. Summarize per source; mark any sensor not fitted as N/A.

- [ ] Pass

### A3 — Binary alarm: Hall (state edge & level, with confirm + hold)

**Goal:** A `state` rule with `dwell > 0` requires the transition (edge) or the target state (level)
to persist for `dwell` seconds before it fires; an edge that fires then also **holds** the alarm
active (and blocks re-arming) for that same `dwell`. `dwell = 0` reproduces the old immediate
behavior. Level deactivation is always immediate.
**Observable:** AlarmReport fPort 3, source `hall-left`/`hall-right`, quantity `state`.

**Polarity (#352):** `state 1` = magnet **present**, `state 0` = magnet **absent** —
`ats sensors sample`/`alarm poll` reading `hall_left=1` while no magnet is applied (or `=0` while
one is) is a regression of the double-inverted-`GPIO_ACTIVE_LOW` bug this issue fixed.

**Prompt for Claude:**
> First confirm the raw polarity: `ats sensors sample` with no magnet must show `hall_left=0`, and
> `=1` only while a magnet is held against it. Then arm an **edge** rule with a confirm+hold
> window, `alarm set 0 hall-left state 0 1 5` (fires on 0→1, 5 s confirm+hold) and confirm with
> `alarm list` it reads `0->1 (edge) dwell=5.00`. Ask me to briefly tap the magnet on and off within
> less than 5 s — confirm **no** AlarmReport fires (the confirm was interrupted). Then ask me to
> apply the magnet and hold it past 5 s — confirm one AlarmReport fires roughly 5 s after the raw
> transition, i.e. when the magnet is **applied**, not removed, and that reapplying the magnet
> within the next 5 s produces **no** second report (holding/re-arm-blocked). Then arm a **level**
> rule `alarm set 0 hall-left state 1 1 5` (`1->1 (level) dwell=5.00`) and confirm it only activates
> after the magnet has been present continuously for ~5 s, and clears immediately on removal.
> Report all four checks with timing.

**Reverse direction (edge on removal, `alarm set 0 hall-left state 1 0 5`):** symmetric to the
`0→1` case above — confirmed to fire ~5 s after the magnet is removed (not reapplied), with the
same early-revert-cancel on a quick remove+reapply within 5 s. Worth testing explicitly at least
once, not just assuming symmetry: a bounce-prone reed switch has different release vs. close
mechanical characteristics, and a **falling** edge gives no counter-increment tell (the hall
counter only counts rises), so `ats sensors sample`'s `active` field, not the counter, is the only
way to confirm the physical transition actually settled before timing the dwell.

- [ ] Pass

### A4 — Binary alarm: Input A/B (state)

**Goal:** Input edges/levels raise `state` alarms (same confirm/hold model as A3).
**Observable:** AlarmReport fPort 3, source `input-a`/`input-b`, quantity `state`.

**Polarity (#352):** `state 1` = input **asserted** (shorted to GND), `state 0` = input **idle** —
same fix/regression check as A3's hall polarity note.

**Prompt for Claude:**
> With PIR disabled (shared pins), confirm `ats sensors sample` shows `input_a=0`/`input_b=0` idle,
> and `=1` only while shorted to GND. Then arm `alarm set <i> input-a state 0 1 [dwell]` (and
> `input-b`). Toggle each input (past `dwell` if set) and confirm AlarmReports on fPort 3 with source
> `input-a`/`input-b` fire on **assertion**, not release. Report results.

- [ ] Pass

### A5 — Momentary alarm: PIR / accel (state one-shot, #150) — dwell as hold/re-arm only

**Goal:** PIR and accel `state` alarms fire **immediately** on each pulse (nothing to debounce —
the sensor already reports a discrete event, not a raw level) and then use `dwell` purely as the
hold/re-arm window: a further pulse within `dwell` seconds of the last is suppressed. `dwell = 0` re-
arms on the very next poll.
**Observable:** AlarmReport fPort 3, source `pir`/`accel`, quantity `state`, edge ACTIVATE; one
report per pulse, suppressed for `dwell` seconds, then re-armed; a motion burst is flood-suppressed
(no permanent latch).

**Gotcha (accel):** `cap-accelerometer true` alone is not enough — `accel-motion-sensitivity`
defaults to `off` and must also be set (`low`/`medium`/`high`) for the LIS2DH12 any-motion
interrupt to fire at all; both need `settings save` + reboot (deferred init). A real shake
produces many rapid any-motion interrupts (the `accel-motion` counter jumps by dozens per shake) —
only the first activates, confirming the rest are correctly hold-suppressed, not each attempting
its own fire.

**Prompt for Claude:**
> Enable the sensor (`config cap-pir-detector true` / `cap-accelerometer true` **and**
> `accel-motion-sensitivity medium`, save). Arm
> `alarm set 0 pir state 0 1 <dwell>` (or `accel state 0 1 <dwell>`, note the value) — edge and level
> behave alike for these momentary sources. Ask me to trigger motion repeatedly; confirm the FIRST
> pulse fires an AlarmReport immediately (no confirm delay, unlike A1/A3), that reports within
> `dwell` seconds of it are suppressed, that it re-arms and fires again once `dwell` has elapsed, and
> that a sustained burst produces only periodic reports (not a flood, not a stuck `active`). Report
> the cadence against the configured `dwell`.

- [ ] Pass

### A6 — Count / rate alarm (hall / input) — dwell as hold/re-arm

**Goal:** A `count` rule fires when a counter exceeds the per-interval rate, then holds/re-arm-
blocks for `dwell` seconds (same role as A5). `dwell = 0` re-arms on the next report interval.
**Observable:** AlarmReport fPort 3, source `hall-left`/`hall-right`/`input-a`/`input-b`, quantity
`count`.

**Gotcha:** `interval_report` has a hard shell-enforced minimum of 60 s (`config interval-report`,
`cmd_int` min=60) — it cannot be shrunk for a faster manual test cycle, so each rate-window
iteration costs a real 60+ s wait. Also: re-arming a rule via `alarm clear all` immediately
followed by `alarm new <same source+quantity>` does **not** reset the runtime dwell/window
state — `rt_sync()` only resets on a `(source, quantity)` mismatch, and nothing re-syncs while the
rule is briefly absent — so a "fresh" rule can silently inherit a stale window baseline from the
previous test run. Insert an `alarm poll` **between** `alarm clear all` and `alarm new` to force a
true reset before timing a fresh window.

**Prompt for Claude:**
> Arm `alarm new hall-left count <N> <dwell>` (small N, note both values; `alarm list` shows
> `rate>=N/interval dwell=…`) — if re-arming an existing rate rule, `alarm clear all` then
> `alarm poll` then `alarm new` (not `clear` immediately followed by `new`, see gotcha above).
> Ask me to pulse the hall sensor more than N times within a report interval and confirm an
> AlarmReport on fPort 3 for that source/quantity, then confirm a second over-rate interval
> within `dwell` seconds of the first does **not** produce a second report while one after `dwell`
> has elapsed does. Report the result.

- [ ] Pass

### A7 — Set & read alarms over LoRaWAN & NFC (SetParam / GetParam)

**Goal:** Alarm slots are written/read as `alarm_<i>` config parameters over both transports
(native protobuf bytes, not hex strings on the wire).
**Observable:** `set_param.alarms.alarm_<i>` arms a slot (Ack); `get_param.alarms_field=[54+i]`
returns the packed rule in `config_dump`; `alarm list` matches; identical behaviour on fPort 85
and NFC.

**Prompt for Claude:**
> Author a SetParam with `ttn.js encodeDownlink` setting e.g. `alarm_0` to a packed onboard-
> temperature rule (hex), send it over LoRaWAN (fPort 85), and confirm an Ack and that `alarm list`
> shows the rule. Then GetParam `alarms_field:[54]` and confirm the returned hex matches what was
> set. Repeat the SetParam over NFC and confirm the same result. Report both transports.

- [ ] Pass

### A8 — Change / delete / deactivate a rule

**Goal:** A slot can be overwritten, cleared, and disabled-without-losing-its-definition.
**Observable:** Overwriting `alarm_<i>` changes the rule; `alarm clear <i>` (or `alarm_<i>` = 34
zero hex chars) removes it; a packed rule with flags = present-only (enabled bit clear, e.g.
`01…`) keeps the slot listed with `en=0` and is **not** evaluated. Since the 2026-08-18
final-review fix, clearing/disabling/editing a rule whose latch is currently ACTIVE also emits
the matching fPort-3 deactivate edge on the next poll (previously the activate was left
dangling for edge-pairing backends).

**Prompt for Claude:**
> Using slot 0: (1) **change** it — `alarm set 0 onboard temperature 0 10 5` then re-set to
> `5 30 5`, confirm `alarm list` reflects each. (2) **deactivate** it over SetParam by writing
> `alarm_0` with the same rule but flags `01` (present, not enabled); confirm `alarm list` shows
> `en=0` and that crossing the bound raises **no** alarm. (3) **delete** it (`alarm clear 0`, or
> SetParam `alarm_0` = all zeros); confirm the slot disappears from `alarm list`. Report all three.

- [ ] Pass

### A9 — Multi-level (two slots, same source + quantity)

**Goal:** Several slots may carry the same `(source, quantity)` as independent rules (e.g. a warning
band and a critical band), each latching/reporting on its own.

**Prompt for Claude:**
> Arm two onboard-temperature rules — slot 0 a wide "warning" band and slot 1 a tighter "critical"
> band (note both). Stimulate crossings into each band and confirm each slot raises its own
> AlarmReport independently (the `slot`/event fields distinguish them) and clears independently.
> Report.

- [ ] Pass

### A10 — AlarmReport structure

**Goal:** AlarmReport fields are well-formed.
**Observable:** `base_time`, `total`, `events[]` with `source`, `edge`, `side`, `rel_s`, and
optional scaled `value` (×100 temp/hum, ×10 pressure; absent for discrete).

**Prompt for Claude:**
> Capture any AlarmReport on fPort 3 and fully decode it. Confirm `base_time` and `total` are
> sensible, each event has a valid `source`/`edge`/`side`/`rel_s`, threshold events carry a scaled
> `value` (×100 for temp/hum, ×10 for pressure) while discrete events omit it, and that no more
> than 8 events appear per frame. Report the decoded structure.

- [ ] Pass

### A11 — Alarm rate-limiting

**Goal:** `alarm_limit` throttles uplinks while still latching events.
**Observable:** First alarm sends immediately; subsequent within the window are batched/suppressed
on the air (LED + counters still update); RTT `Alarm uplink rate-limited; next batch in <N> s`.

**Prompt for Claude:**
> Set `alarm_limit` to a small non-zero window (note the value). Trigger several alarms in quick
> succession. Confirm the first produces an immediate fPort 3 uplink, further ones within the
> window are batched (RTT shows `Alarm uplink rate-limited; next batch in <N> s`) and arrive
> together when the window closes, and that the LED/counters still reflect every event. Report the
> observed batching.

- [ ] Pass

### A12 — Event LED (orange, commissioning-only diagnostic)

**Goal:** The orange event LED is a **separate, commissioning-only** indicator, distinct from the
red alarm-active blink (see A14) — it fires on every discrete input activation/release regardless
of whether any alarm rule is armed for that source, and only during the first hour after boot. It
is driven by fixed firmware constants, not by any config parameter (in particular, **not** by
`dwell` or the removed `alarm-notif-time`).
**Observable:** A short green→orange (activation) or orange→green (release) two-colour sequence
(orange = red+green together, ~50 ms per phase) on any discrete source event (hall/input/pir/accel,
independent of whether an alarm rule exists for it); rate-limited to at most one sequence per
~500 ms; stops firing entirely once uptime exceeds 60 minutes.

**Prompt for Claude:**
> Within the first few minutes after a fresh boot, toggle a hall or input line (no alarm rule
> needs to be armed) and confirm a brief green-then-orange blink on activation and orange-then-
> green on release, each phase ~50 ms. Toggle it rapidly a few times and confirm bursts faster than
> ~500 ms apart are rate-limited to one sequence. Then — this part takes time, only if asked — wait
> past 60 minutes of uptime and confirm the same toggle produces **no** blink anymore. Report all
> three observations, and confirm this sequence is independent of any `dwell` value on an alarm rule
> for that source (see A14 for the *other*, alarm-driven red blink).

- [ ] Pass

### A14 — Red alarm-active LED tracks the latch, not a separate timer

**Goal:** Unlike A12, the **red** alarm LED has no timer of its own — the main loop blinks it red
on every ~3 s poll for as long as `app_alarm_poll()` reports any rule `active`. Post-#348, "how
long the red blink appears to hold" for a given rule is now entirely a function of that rule's own
`dwell` (via the dwell-before-activate delaying the start, and the hold/re-arm delaying the end for
edge/momentary/count kinds) — there is no independent LED hold duration to configure.
**Observable:** Red blink starts only once a rule's `active` latches (after any configured dwell)
and stops the poll cycle after `active` clears (after any configured hold), tracking `alarm list`'s
`active=` column throughout.

**Prompt for Claude:**
> Arm a momentary rule with a several-second `dwell`, e.g. `alarm set 0 pir state 0 1 8`. Trigger one
> pulse and, in parallel, watch both the red LED and repeated `alarm list` output. Confirm the red
> blink starts immediately (momentary sources have no dwell) and `alarm list` shows `active=1`,
> then confirm both the blink and `active=1` persist for ~8 s and clear together (not
> independently) once the hold elapses. Repeat with a threshold rule that has a dwell (e.g. A1's
> slot 0) and confirm the red blink only starts once the dwell has elapsed and `active` has
> latched, not at the moment the value first crossed the bound. Report the correlation between the
> LED, `alarm list active=`, and the configured `dwell` in each case.

- [ ] Pass

### A13 — Dual uplink per edge & `alarm-limit = 0`

**Goal:** An alarm edge produces an immediate fPort 2 telemetry **and** a delayed fPort 3 batch;
`alarm-limit = 0` disables windowing.
**Observable:** With `alarm-limit > 0`: first edge → immediate fPort 2, then fPort 3 AlarmReport at
window end; `truncated`/`total` correct when events exceed the carried list. With `alarm-limit = 0`:
each edge sent immediately as its own one-event fPort 3 frame, fPort 2 not rate-limited
(`doc/version 1.4.md` §3, §7).

**Prompt for Claude:**
> With `alarm-limit` set to e.g. 20 s, trigger one alarm edge and confirm the network server shows
> an **immediate fPort 2** telemetry on the first edge, followed by the **fPort 3** AlarmReport
> ~20 s later. Fire many edges in the window and confirm `total` counts them all and `truncated`
> is set if more than ~8 are carried. Then set `alarm-limit = 0`, trigger edges, and confirm each
> edge is sent immediately as its own single-event fPort 3 frame with no fPort 2 rate-limiting.
> Report both modes.

- [ ] Pass

---

## Config / Remote control

### C1 — SetParam

**Goal:** A parameter set over the air is acknowledged and staged.
**Observable:** `Response.Ack` on fPort 85; staged value readable via `config`.

**Prompt for Claude:**
> Send a SetParam command (fPort 85) changing one parameter (e.g. `interval_report`) — real
> downlink via TTS MCP, or `ats cmd lrw <hex>` in a debug build. Confirm a `Response.Ack` and that
> the staged value is reflected by the corresponding `config` shell read. Report old vs new.

- [ ] Pass

### C2 — Staged config + commit

**Goal:** Changes commit only on `save=true` / SettingsSave.
**Observable:** Staged changes survive only after commit + reboot; `Response.Ack`.

**Prompt for Claude:**
> Send SetParam **without** save and confirm the change is staged but not yet persisted. Then send
> SetParam with `save=true` (or a SettingsSave command). Confirm a `Response.Ack`, that the device
> persists and reboots, and that the new value survives the reboot (read back via `config`). Report
> the before/after across the reboot.
>
> **Negative check:** stage a SetParam **without** `save=true`, then trigger a **plain `reboot`**
> command (not SettingsSave, not a save-triggering command like `device_reset`). Confirm the
> staged value has **reverted** to the last genuinely-saved value after the reboot — a plain
> reboot must not accidentally commit an unsaved stage.

- [ ] Pass

### C3 — GetParam

**Goal:** Specific fields can be queried.
**Observable:** `Response.ConfigDump` on fPort 85 containing the requested fields.

**Prompt for Claude:**
> Send a GetParam for one or two specific config fields (fPort 85). Confirm the device replies with
> a `Response.ConfigDump` containing exactly those fields and that the values match the shell
> `config` reads. Report the decoded dump.

- [ ] Pass

### C4 — GetConfig (paginated)

**Goal:** Full config dumps across pages.
**Observable:** Multiple `Response.ConfigDump` frames, page N of M.

**Prompt for Claude:**
> Send a GetConfig command (fPort 85). Confirm the device returns the full config as one or more
> paginated `Response.ConfigDump` frames (page index/count present and consistent) and that the
> pages reassemble into a complete config. Report the number of pages.

- [ ] Pass

### C5 — Config validation

**Goal:** Bad commands are rejected with a useful error.
**Observable:** `Response.Error` with a code (1 BAD_REQUEST, 2 OUT_OF_RANGE, 3 NOT_READY,
4 HISTORY_UNAVAILABLE, 5 UNSUPPORTED_FIELD, 6 PERSIST_FAILED), an optional `fault_field`, and a
`detail` string (`doc/version 1.4.md` §1).

**Prompt for Claude:**
> Send a SetParam with a deliberately invalid value (out of range) for a known-bounded field.
> Confirm the device replies with a `Response.Error` whose code decodes to `OUT_OF_RANGE` (2), whose
> `fault_field` points at the offending field, with a `detail` string, and that the config is
> **not** changed. If easy, also provoke an `UNSUPPORTED_FIELD` (5) by addressing an unknown field
> and a `BAD_REQUEST` (1) with a malformed payload, and report each code/`fault_field`/`detail`.

- [ ] Pass

### C6 — ForceSend

**Goal:** ForceSend triggers an immediate telemetry uplink.
**Observable:** `Response.Ack` then an fPort 2 Telemetry shortly after.

**Prompt for Claude:**
> Send a ForceSend command (fPort 85). Confirm a `Response.Ack` and that an fPort 2 Telemetry
> uplink follows promptly (outside the normal interval). Report the latency from command to uplink.

- [ ] Pass

### C7 — ResetCounters

**Goal:** Counters can be zeroed remotely.
**Observable:** `Response.Ack`; selected hall/input counters reset to 0 in next telemetry.

**Prompt for Claude:**
> Note current hall/input counters via `ats sensors sample`. Send a ResetCounters command (fPort
> 85), optionally selecting specific counters. Confirm a `Response.Ack` and that the selected
> counters read 0 afterward (shell and next fPort 2 telemetry), while unselected ones are
> untouched. Report which reset.

- [ ] Pass

### C8 — Config shell round-trip

**Goal:** `config` get/set works for the parameter set.
**Observable:** `config <param>` reads; `config <param> <value>` sets; value persists after
`settings save`.

**Prompt for Claude:**
> Pick a few representative `config` parameters across categories (lrw-*, interval-*, alarm-*).
> For each: read it, set a new value, read it back to confirm the change, then `settings save` and
> reboot and confirm it persisted. Report each round-trip. Restore original values at the end.

- [ ] Pass

### C9 — Counter persistence across power loss (#49)

**Goal:** Hall/input pulse totalizers survive a reboot/power loss, are backed up at the
`interval_sample` cadence, and do so independently of the LoRaWAN join state.
**Observable:** After a reboot **without** `settings save`, the counters resume from their last
periodically-saved value (not zero).

**Prompt for Claude:**
> Enable a hall (or input) counter. Set `interval-sample` short (e.g. 30 s) and `interval-report`
> long (e.g. 900 s), then `settings save`. Note the baseline counts. Ask me to generate some pulses,
> then read the new RAM counts. Wait one `interval_sample` for the periodic backup, then reboot the
> device **without** `settings save` (`ats device reset`) to simulate a power loss. Confirm the
> counts persisted (≈ the pre-reboot value, not reset to ~1) — this proves the backup ran at the
> sample cadence, decoupled from the long report interval. On a device with DevEUI all-zero
> (LoRaWAN disabled) this also proves join-independence. Then `ats sensors reset`, wait one
> `interval_sample`, reboot without save again, and confirm the zeroed value persisted (the dirty
> flag handles decrements too). Restore `interval-sample`/`interval-report` at the end.

- [ ] Pass

### C10 — Read LoRaWAN keys back over NFC only (#162)

**Goal:** The LoRaWAN crypto keys (`nwkkey`/`appkey`/`nwkskey`/`appskey`) are readable over the
encrypted NFC channel but never over LoRaWAN.
**Observable:** A `GetParam`/`GetConfig` selecting a key returns it when sent over NFC; the same
request over a LoRaWAN downlink comes back with the key **omitted**. `secret_key` is never returned.

**Prompt for Claude:**
> Provision known LoRaWAN keys. Over a **LoRaWAN downlink**, send a `GetParam` selecting `nwkkey`
> (and `appkey`) and confirm the `config_dump` response does **not** contain them (and `GetConfig`
> never includes them on any page). Then over **NFC** (Manager-App, encrypted) send the same
> `GetParam` and confirm the keys come back with the provisioned values. Confirm `secret_key` is
> never returned on either transport. (LRW omission is also covered by the native_sim unit test
> `test_get_param_keys_nfc_only`; this is the on-air / NFC confirmation.)

- [ ] Pass

---

## Clock / RTC

### K1 — `clock get` before sync

**Goal:** Unset RTC reports clearly.
**Observable:** `clock get` indicates the RTC is not set yet.

**Prompt for Claude:**
> On a freshly booted device that hasn't synced time, run `clock get` and confirm it reports the
> RTC as not set (rather than a bogus time). Report the exact output.

- [ ] Pass

### K2 — `clock set`

**Goal:** Manual time set works.
**Observable:** `clock set <unix>` then `clock get` returns the set time (advancing).

**Prompt for Claude:**
> Run `clock set <unix>` with a current UNIX timestamp, then `clock get` a few seconds later and
> confirm it returns approximately that time and is advancing. Report the values.

- [ ] Pass

### K3 — Network time sync (DeviceTimeReq)

**Goal:** Time syncs automatically on join, and on demand via `clock sync` / ClockSync.
**Observable:** RTT `DeviceTimeReq queued`; after DeviceTimeAns downlink, `RTC synced from
network: unix=<...>`. Per `doc/version 1.4.md` §5 the sync is **requested automatically on join**.

**Prompt for Claude:**
> First verify the automatic path: trigger a fresh join and confirm the device requests time on its
> own (`DeviceTimeReq queued` in the RTT log without any manual command) and eventually logs
> `RTC synced from network: unix=<value>`. Then verify the on-demand path: run `clock sync` over
> the RTT shell (or send a ClockSync command on fPort 85) and confirm the same sync sequence. Then
> `clock get` and confirm it matches real UTC. Report the synced time and which path(s) worked.

- [ ] Pass

### K4 — `unix_time` in GetInfo

**Goal:** Synced time surfaces in GetInfo.
**Observable:** `Response.Info.unix_time` is non-zero and matches real time after sync.

**Prompt for Claude:**
> After a successful time sync, send GetInfo (fPort 85) and confirm the decoded `Response.Info`
> has a non-zero `unix_time` matching real UTC (before sync it should be 0). Report both states if
> you can capture them.

- [ ] Pass

### K5 — History timestamps

**Goal:** Records carry real wall-clock time after sync.
**Observable:** `history read` timestamps are real UNIX times (not uptime) once RTC is synced.

**Prompt for Claude:**
> After syncing the clock, let a few records accumulate, then `history read` and confirm the
> record timestamps are real UNIX times consistent with the synced clock (not boot-relative
> uptime). Report a couple of timestamps.

- [ ] Pass

### K5b — Off-cadence triggers never disturb the history interval (regression guard, #H1)

**Goal:** History captures only on the fixed `interval_report` cadence — an ad-hoc trigger
(alarm uplink, `force_send`, `sample`) must sample and send but must **not** append an extra
history record or shift the cadence timer, since replay reconstructs each record's time as
`base + ordinal * interval_report` with no per-record timestamp on the wire.
**Observable:** `history count` is unchanged immediately after firing several off-cadence
triggers mid-interval, and the next periodic record still lands exactly `interval_report` seconds
after the previous one (no skip, no extra entry, no skew) — checked over several consecutive
intervals, not just once.
**Prompt for Claude:**
> Let 2–3 records accumulate on a known cadence and note `history read`'s timestamps. Mid-way
> through the next interval, fire `force_send` and `sample` two or three times in quick
> succession (`ats cmd lrw <hex>` locally, or real downlinks). Immediately re-check `history
> count` — it must be unchanged. Wait for the next periodic tick and confirm it lands exactly
> `interval_report` seconds after the prior record (not early, not skewed). Repeat once more to
> confirm the cadence holds over multiple intervals despite the interleaved triggers. Report the
> full timestamp sequence.

- [ ] Pass

### K6 — Set RTC over NFC (`clock_sync` with `unix_time`)

**Goal:** A phone can bootstrap the wall-clock over NFC before/without a network (#107).
**Observable:** A `clock_sync` command carrying `unix_time` sets the RTC and answers with an `info`
response whose `unix_time` matches; `clock get` then returns that time. An out-of-range epoch is
rejected with `error` `BAD_REQUEST` "bad epoch".

**Prompt for Claude:**
> On a freshly booted device that has **not** synced time (so `clock get` reports unset), inject a
> `clock_sync` command with a `unix_time` over the NFC transport. On a debug build use the debug
> shell: `ats cmd nfc 0801620608808bd2bb06` (ClockSync `unix_time = 1735689600`, i.e.
> 2025-01-01T00:00:00Z) — or present an NFC command record from the manager app. Confirm the
> decoded response is an `Info` with `unix_time = 1735689600` (not an `ack`), then `clock get`
> returns ~that time and is advancing. Finally inject an out-of-range epoch (e.g. `unix_time = 1`)
> and confirm the response is `error` code 1 (BAD_REQUEST) with detail "bad epoch", and the RTC is
> left unchanged. Report all three results.

- [x] Pass — HW-verified 2026-06-16 (debug build, RTT shell). Fresh boot `RTC not set yet`; NFC
  `clock_sync{unix_time}` set the RTC and `clock get` returned/advanced it (3×); out-of-range
  `unix_time=1` was rejected, RTC left at ~2026 (no jump to 1970).

---

## NFC

### N1 — NFC config delivery: SAVE

**Goal:** A SAVE NFC tag applies and persists config.
**Observable:** RTT `NFC action: SAVE`; ~10 yellow blinks; config persisted.

**Prompt for Claude:**
> Tell me how to present a SAVE NFC config tag (NDEF MIME
> `application/vnd.hardwario.sticker-config.v1`) to the device. On tap, confirm the RTT log shows
> `NFC action: SAVE`, the yellow LED blinks ~10×, and the delivered config is persisted (read back
> via `config`). Report what was applied.

- [ ] Pass

### N4 — NFC channel encryption (`CONFIG_APP_NFC_ENCRYPTION`)

**Goal:** The command/config channel is AES-CCM encrypted by default; only info reads without a key. A validation build (`=n`) accepts plaintext (#135).
**Observable:** On a default build, a plaintext `hio.stck:cmd` record is rejected (no response / decrypt error) while a properly encrypted one is answered with an encrypted `hio.stck:rsp`. On a `CONFIG_APP_NFC_ENCRYPTION=n` build, the boot log shows the `NFC ENCRYPTION DISABLED - VALIDATION BUILD ONLY` banner and plaintext command/config records are accepted.

**Prompt for Claude:**
> Validation build (`-DCONFIG_APP_NFC_ENCRYPTION=n`, debug): confirm the boot banner, then inject
> `ats cmd nfc 08032200` (get_info) and confirm a plaintext `Response.Info` comes back. Default
> build (encryption on): present a plaintext command record and confirm it is rejected; present an
> AES-CCM record (serial + nonce > last) and confirm an encrypted response is written back, and that
> the info record (`hio.stck:inf`) is still readable without the key. Report all results.

> Note: the request/response nonce construction and anti-replay behaviour are covered in detail by **N8**.

- [ ] Pass

### N5 — LoRaWAN reset & forced join over NFC (#109)

**Goal:** A phone can reset the LoRaWAN counters and force a join via the NFC command channel, completing the set-params → `lrw_reset` → `lrw_join` commissioning flow without a shell/J-Link.
**Observable:** `lrw_reset` writes an `ack` back to the tag, then RTT shows `Command: LoRaWAN reset (NVM wipe) + reboot` and the device cold-reboots (frame counter / `DevNonce` back to 0). `lrw_join` writes an `ack` and RTT shows `Command: forced LoRaWAN join` with a fresh join (no reboot).

**Prompt for Claude:**
> On a default (encrypted) build: present an AES-CCM `hio.stck:cmd` record carrying `lrw_join`
> (`08018a0100`) and confirm the `ack` is written back to the tag and RTT logs `Command: forced
> LoRaWAN join` followed by a join attempt — with no reboot. Then present `lrw_reset` (`0801820100`),
> confirm the `ack` is readable first, then RTT logs the NVM wipe + reboot and the LoRaWAN frame
> counter restarts at 0 after reboot. Also verify both commands work as fPort-85 downlinks. Report results.

- [ ] Pass

- [x] N/A (feature removed)

### N7 — Provisioning while powered off (boot-staged config, #147)

**Goal:** A STICKER written over NFC while **unpowered** self-configures on the next boot, before
the LoRaWAN stack starts, with nonce anti-replay.
**Observable:** A config/command record written to the tag with the MCU off is applied on the next
boot (yellow NFC carousel), persisted, and cleared from the tag (info record restored); a stale/replay
record is rejected and the device still boots normally.

**Prompt for Claude (needs the Manager-App phone + a way to remove device power — not bench/J-Link testable):**
> With the device **fully powered off** (battery out, J-Link disconnected so SWD can't back-power it),
> use the Manager-App to write an (encrypted) `set_param` (e.g. `lorawan.adr` toggled, `save=true`) to
> the tag and confirm the bytes read back. Power the device on with **no further phone interaction**
> and confirm: the yellow NFC carousel blinks, the new value is persisted (read it back over shell/NFC),
> the tag no longer holds the staged record (info record restored), and — because the early boot check
> runs before LoRaWAN — staged LoRaWAN keys take effect on the first join. Then power-cycle again and
> confirm the config is **not** re-applied (nonce anti-replay) and the device boots normally. Report
> results, including that the **encrypted** path (decrypt + nonce at boot) works end-to-end.

- [ ] Pass

### N8 — NFC crypto hardening: nonce separation, anti-replay, response cache (#179, #184)

**Goal:** The encrypted channel no longer reuses a `(key, nonce)` pair across a request and its
response (#179), the anti-replay counter survives a power-cycle (#184), the counter high-water is
exposed in the plaintext info record so a phone can resync, and a same-counter retransmission is
idempotent via a response cache.
**Observable:**
- **Direction-separated nonce:** the CCM nonce is `serial ‖ nonce_counter ‖ direction` (9 bytes), with
  the direction byte `0x00` for the request and `0x01` for the response. Request and response carry the
  *same* counter in the header but use different keystreams. A phone on the new codec (9-byte nonce +
  header-as-AAD) decrypts the response; the old 8-byte-nonce / no-AAD codec fails to decrypt or verify.
- **Counter in info record:** `hio.stck:inf` is format `0x02`, 15-byte payload, with the last-accepted
  `nonce_counter` (big-endian) at payload bytes `[11..14]`. It tracks the live counter.
- **Idempotent retransmission (response cache):** re-sending the **same** counter (e.g. the phone never
  read the reply) replays the cached encrypted response **without re-running** the command — no double
  execution of a `set_param`/action.
- **Anti-replay persists across reboot:** the accepted counter is durable; a counter `<=` the stored
  high-water is rejected (`-EACCES`). After a reboot the response cache is empty, so even a same-counter
  retry is rejected and the phone resyncs from the info-record counter. `lrw_reset` (which reboots
  immediately) cannot be replayed.

**Prompt for Claude — bench/J-Link verifiable parts (FW agent):**
> On the default (encrypted) build, over RTT: `nfc dump` and confirm the `hio.stck:inf` record has
> format byte `0x02`, payload length `0x0f` (15), and a 4-byte counter field after the debug flag.
> Set `config nonce-counter <N>` to a recognizable value, force an info rewrite (`nfc clear` then a few
> `nfc check`), `nfc dump` again and confirm the counter field shows `<N>` big-endian. Restore
> `config nonce-counter 0`. (The wire-format contract — golden request/response vectors, direction
> separation, AAD binding — is also asserted by the `tests/nfc_crypto` native_sim unit suite.)

**Prompt for Claude — round-trip & replay parts (needs the Manager-App phone):**
> With the Manager-App on the matching codec: send an encrypted `get_info` with counter = last+1,
> confirm the encrypted `hio.stck:rsp` decrypts on the phone and that `config nonce-counter` (shell) and
> the info-record counter both advanced. Re-send the **same** counter (simulating a lost reply) and
> confirm the device **replays the identical cached response without re-running** the command (RTT shows
> `retransmission … replaying cached response`; a `set_param` value is not applied twice). Power-cycle the
> device, re-send the same counter, and confirm it is now **rejected** (`-EACCES`, cache gone) and the
> phone resyncs from the info-record counter (`stored+1`). Capture a request and its response and confirm
> they cannot be cross-decrypted (direction separation). Report results.

- [ ] Pass

### N9 — Reset ladder over `hio.stck:cmd`: `device_reset` / `factory_reset` / `set_secret_key`, ack-before-reboot (#299)

**Goal:** unlike `vendor_reset` (its own `hio.stck:vnd` vendor channel, see G6a-NFC), `device_reset` and
`factory_reset` are ordinary `Command`s dispatched over the standard encrypted `hio.stck:cmd`
channel — `factory_reset` is additionally **nfc/shell-only** (rejected as a LoRaWAN downlink,
since a downlink that drops its own LoRaWAN session could never confirm delivery). `set_secret_key`
is also nfc/shell-only and reachable the same way, and since #322 it reboots too — that reboot is
what makes the rotated key live, because the encrypted channel authenticates from the boot-time
`g_app_config` copy. All three must follow
the same **ack-before-reboot** handshake as `lrw_reset`/`lrw_join` (N5): the device writes its
encrypted response to the tag *first*, and only reboots once the phone reads it (`hio.stck:ack`)
or a ~10 s quiet-field timeout fires — never immediately off the tap.
**Observable:** `device_reset` — encrypted `ack` written back, RTT shows the deferred action
staged, reboot only after the ack/timeout, then config/alarm defaults restored but identity +
LoRaWAN provisioning intact (same postconditions as G6, driven over NFC instead of shell/LRW).
`factory_reset` — same ack-before-reboot gate, but LoRaWAN keys/session also reset and the device
re-joins after reboot (same postconditions as G6a's `factory_reset`, driven over NFC); presenting
it as a LoRaWAN downlink is rejected with `Error{NOT_READY}` (transport not allowed), never
silently accepted. `set_secret_key` — encrypted `ack` written back **first** (still under the *old*
key, since the rotation is not live yet), reboot only after the ack/quiet-field timeout, and *after*
that reboot the **new** key decrypts while the old one is rejected (#322). An all-zero key is
refused with `Error{BAD_REQUEST}` — no save, no reboot, key unchanged.

**Prompt for Claude:**
> Build an AES-CCM `hio.stck:cmd` frame carrying `device_reset` (mirror the golden-vector
> construction in `tests/nfc_crypto` / `reference_nfc_rst_hil_test_299`, encrypted with the
> current `secret-key`) and inject it via sequential `nfc write` calls (long hex truncates
> silently past ~128 chars — split into ~40-byte chunks). Confirm the encrypted `ack` is on the
> tag **before** any reboot happens; only after reading it back (or waiting out the ~10 s
> quiet-field timeout) does RTT show the reboot. After reboot confirm config/alarm defaults are
> restored but `config lrw-deveui`/`config serial-number` are unchanged. Repeat for
> `factory_reset`: confirm the same ack-then-reboot ordering, and that LoRaWAN keys reset and the
> device re-joins after reboot. Then present `factory_reset` as a fPort-85 LoRaWAN downlink instead
> and confirm it is rejected with `Error{NOT_READY}` rather than silently executed. Finally send
> `set_secret_key` with a new 16-byte key over `hio.stck:cmd`: confirm the `ack` is written to the
> tag **before** the reboot and is still decryptable with the *old* key, that the reboot fires only
> after the ack/quiet-field timeout, and that after it a frame encrypted with the *old* key is
> rejected while one encrypted with the *new* key succeeds (#322). Repeat `set_secret_key` with an
> all-zero key and confirm `Error{BAD_REQUEST}`, no reboot, and `config secret-key` unchanged.
> Report all results.

**`set_secret_key` portion HIL-verified 2026-07-27** (#322), same frame-construction recipe as
G6a-NFC above — hand-crafted AES-CCM `hio.stck:cmd` records injected with chunked
`nfc write <offset> <hex>` and driven with `nfc check`. Confirmed: (1) an all-zero key is refused
with `Error{BAD_REQUEST}` detail `"zero key"`, deferred action `none`, no reboot, `secret-key`
unchanged; (2) a valid rotation reports deferred action `secret-key-save+reboot`, the `Ack` is
written to the tag **first** and still decrypts under the *old* key, then the device cold-reboots
and `config secret-key` reads the new key — i.e. the new key is live immediately rather than at
some later unrelated reboot; (3) after that reboot a frame sealed with the *old* key is refused
(`command rejected: -5`, nonce high-water not advanced) while the same frame sealed with the *new*
key is handled normally; (4) `nonce-counter` is preserved across the rotation reboot (persisted by
`decrypt()` before the command runs). The `device_reset` / `factory_reset` legs of N9 were **not**
re-exercised in this session — unchanged by #322.

- [ ] Pass

### N10 — Claim window: `hio.stck:clm` lay-down + all three end-of-claim triggers (#247, #308)

**Goal:** once `claim_token` is provisioned, the firmware publishes `hio.stck:clm` alongside the
plaintext info record on every resting-tag write (including after a reboot/reflash), and the claim
window ends via **any** of three independent triggers: (1) the phone deletes `clm` off the tag
(#247, unauthenticated over RF, kept for backward compatibility), (2) the phone sends the explicit
`clm_ack` command over the encrypted `hio.stck:cmd` channel (#308), or (3) the phone sends **any**
other authenticated command at all — decrypting it already proves `secret_key` possession (#308).
All three latch the same persisted `UNSET → PENDING → CONSUMED` state; once `CONSUMED`, `clm` is
never republished, even across reboot/reflash — only a full NVS erase or `vendor_reset` reopens it.
**Observable:** `nfc clm` shell command reports the latch state throughout. `nfc dump` shows a
two-record `[inf, clm]` NDEF message while `PENDING`, `inf`-only once `CONSUMED`.

**Prompt for Claude:**
> `settings erase`, then `config claim-token <32-hex>` + `settings save`. After reboot confirm
> `nfc clm` reports `PENDING` and `nfc dump` shows the two-record `[inf, clm]` message. Reflash the
> firmware (plain `west flash`, no `--erase`) and confirm `PENDING` and the two-record tag survive
> the reflash unchanged (#308 "publish until claimed" guarantee).
>
> Trigger 1 (delete-detection, #247): rewrite the tag with an info-only NDEF message (simulating
> the phone deleting `clm` after claiming) via `nfc write`, then `nfc check`. Confirm `nfc clm`
> latches `CONSUMED` and reboot doesn't resurrect `clm`.
>
> Reset to `PENDING` again (`settings erase` + re-provision) for the next two triggers so each is
> tested from a clean arm. Trigger 2 (`clm_ack`, #308): build an AES-CCM `hio.stck:cmd` frame
> carrying `clm_ack` (mirror the golden-vector construction in `tests/nfc_crypto` /
> `reference_nfc_rst_hil_test_299`; split into sequential `nfc write` calls — long hex truncates
> silently past ~128 chars) and inject it. Confirm the encrypted `ack` comes back and `nfc clm`
> latches `CONSUMED` — with **no** RF delete needed.
>
> Trigger 3 (implicit consume, #308): re-arm to `PENDING` once more, then send an *unrelated*
> authenticated command (e.g. `get_info`) over `hio.stck:cmd` instead of `clm_ack`. Confirm that
> merely decrypting this command also latches `CONSUMED`, even though the command itself was never
> `clm_ack`. Report all three trigger outcomes and the reflash-survival result.

**HIL-verified 2026-07-14** (debug image, J-Link 822005109), hand-crafted AES-CCM frames (no phone,
same recipe as `reference_nfc_rst_hil_test_299`): `settings erase` → `config claim-token` +
`config secret-key` + `settings save` → reboot arms `PENDING`; `nfc dump` confirmed the two-record
`[inf, clm]` message (`TLV len=0x5b`, second record `54 0c 12 68 69 6f 2e 73 74 63 6b 3a 63 6c 6d`
= `hio.stck:clm`, payload `12 10` + the 16-byte test token, byte-exact). **Reflash survival**:
re-flashed the same image (no `--erase`) and confirmed `PENDING` + the identical two-record content
survived unchanged. **Trigger 2 (`clm_ack`)**: injected an encrypted `hio.stck:cmd` frame carrying
`clm_ack` → `handled, response 5 B` (bare ack, `deferred action: none`) → `clm state: consumed (2)`.
**Trigger 3 (implicit consume)**: re-armed to `PENDING`, injected an encrypted `get_info` command
instead (67 B `Info` response, clearly not `clm_ack`) → `clm state: consumed (2)` all the same,
confirming any authenticated command ends the window. Trigger 1 (delete-detection) was not
re-exercised standalone this session — its logic is unchanged from #247 (only moved into the shared
`clm_consume()` helper also used by triggers 2/3, both of which passed) — see the original #247
HW-validation note above for its own direct HIL run.

- [x] Pass (HIL-verified via hand-crafted frames, 2026-07-14; triggers 2 and 3 + reflash survival)

### N11 — Rejected tap blinks red, not green (#315)

**Goal:** a command that fails authentication is visually distinguishable from one that succeeded.
Both encrypted channels (`hio.stck:cmd` keyed by `secret_key`, `hio.stck:vnd` keyed by
`vendor_token`) write **nothing** back to the tag when the frame is rejected — wrong key, stale or
out-of-window `nonce_counter`, unprovisioned (all-zero) key, malformed frame — so before #315 the
green "servicing" blink simply kept running until the RF-quiet backstop and a failed tap looked
exactly like a successful one.
**Observable:** on rejection the green fast blink is replaced by a **red fast blink** (same ~90 ms
cadence) held ~2 s, then the LED clears; RTT shows `-> command rejected: <errno>` (or
`-> vendor command rejected:`) for the same tap. An *authenticated* command that merely fails at the
application level (e.g. `NOT_WRITABLE`) is **not** a rejection: it returns an encrypted `error`
response and still shows the reply-ready green+yellow (`doc/version 1.4.md` §16).

**Prompt for Claude:**
> On the debug build over RTT, inject a *tampered* encrypted `hio.stck:cmd` frame (take a valid
> hand-crafted frame — same recipe as N9/N10 — and flip one ciphertext byte so the CCM tag fails)
> via sequential `nfc write` calls, then `nfc check`. Confirm RTT reports the rejection
> (`handle_encrypted_cmd` failed / `-> command rejected: -5`) and that the red LED is driven
> instead of green: read the LED GPIO state over J-Link (or watch the unit) during the ~2 s window,
> then confirm all three channels are off afterwards. Repeat with a **stale** counter (`<=` the
> stored high-water → `-EACCES`) and with a `hio.stck:vnd` frame under a wrong `vendor_token`.
> Finally send one *valid* command and confirm the normal green → green+yellow sequence still
> happens (no red, no orange blend from a leftover red channel). Report each outcome.

- [ ] Pass

---

## Payload formatter (`ttn.js`)

The v1.4.0 payload formatter (`app/decoder/ttn.js`) must decode the new ports and encode/decode
downlink commands on the live network servers (`doc/version 1.4.md` §8). Upload it to TTN and
ChirpStack before these tasks.

### F1 — `decodeUplink` on TTN & ChirpStack

**Goal:** The formatter decodes fPort 1/2/3/85 correctly on the live consoles.
**Observable:** Decoded uplinks show legacy bitmap (fPort 1), protobuf telemetry (fPort 2),
`AlarmReport` (fPort 3), and command responses `info`/`ack`/`config_dump`/`history_frame`/`error`
(fPort 85).

**Prompt for Claude:**
> Confirm `app/decoder/ttn.js` is uploaded as the payload formatter on both TTN and ChirpStack.
> Drive uplinks on fPorts 1, 2, 3 and 85 (telemetry via `send`, an alarm, and a `get_info`
> response), then confirm each console's decoded output is well-formed and matches the raw payload
> on **both** servers. Report any decode discrepancy between TTN and ChirpStack.

- [ ] Pass

### F2 — `encodeDownlink`

**Goal:** Commands authored as JSON encode to correct fPort 85 bytes.
**Observable:** `encodeDownlink` returns `{ bytes, fPort: 85, warnings, errors }`; the device acts
on the resulting downlink.

**Prompt for Claude:**
> Using the formatter's `encodeDownlink`, author a few commands as JSON (e.g. `{"command":
> "get_info","seq":1}`, a `force_send`, and a `set_param` changing `interval_report`). Confirm it
> returns `fPort: 85` and bytes matching the known-good hex in the prerequisites table, send them,
> and confirm the device responds/acts correctly. Report bytes vs expected hex.

- [ ] Pass

### F3 — `decodeDownlink`

**Goal:** Queued commands are human-readable on the network server.
**Observable:** `decodeDownlink` renders a queued command back to its JSON/description.

**Prompt for Claude:**
> Queue a downlink command on the network server and confirm `decodeDownlink` renders it back to a
> readable command (matching what was encoded). Report the round-trip (JSON → bytes → JSON).

- [ ] Pass

---

## PR #358 regression scenarios (2026-08-13 critical-analysis campaign)

PR #358 merged into `v1.4.0` on 2026-08-17 (`5d14b24`): 2 CRITICAL, 12 HIGH, and 16 of issue
[#340](https://github.com/hardwario/sticker-firmware/issues/340)'s MEDIUM findings
(M1/M2/M3/M4/M5/M6/M8/M9/M13/M15/M17/M18/M19/M22/M24/M27). **None of these have HIL confirmation
yet** — code-review/native_sim/build-verified only. This section is the checklist for closing that
gap before `v1.4.0 → main`. Test IDs `X1`-`X20`; `[HIL-only]` items have no automated regression
coverage at all (native_sim can't reach them), the rest already have a unit/native_sim test but
still need HW confirmation that the real device behaves the same way.

### X1 — C1: history OOB write on flash write failure

**Goal:** A `flash_area_write()` failure inside `backend_append()`'s streaming loop no longer
leaves `m_stage_len` stuck, so the next capture doesn't index `m_stage[]` out of bounds.
**Observable:** No crash/HardFault after a forced write failure; the device keeps sampling and
capturing normally afterward.

**Prompt for Claude:** Enable `history` (flash backend, release or debug), force a flash write
error (temporarily bad `flash_area_write` return, or an out-of-range history flash offset via
shell if available), capture a few more records, confirm no HardFault/reset and `history stats`
stays sane afterward.

- [x] Pass

> **HW-verified (2026-08-17, SN 2162199999)**: temporary debug-only build
> (`debug-history-flash.conf`/`.overlay`, `CONFIG_APP_HISTORY_FLASH=y`) + a temporary
> `history debugfail <n>` fault-injection shell hook wrapping `flash_area_write()` in
> `backend_append()`. Forced ~5 injected write failures across the ordinary (non-rollover)
> append path — every one produced `"history append failed — record dropped"`, count did NOT
> advance, no crash, and the very next capture succeeded normally. `m_stage_len` handling (C1)
> confirmed correct on real hardware. See X5 for the rollover-path finding (#384).

### X2 — C2: SetParam vendor-transport write gate

**Goal:** `vendor`-transport `SetParam` can no longer write fields excluded from `vendor` (e.g. any
`alarm_0..15` slot, `lorawan` identity/session fields), only `nfc`/`lrw`-writable ones.
**Observable:** A vendor-authenticated `SetParam` targeting `alarm_0` (or a LoRaWAN identity field)
is rejected; the same field set via `nfc`/`lrw` succeeds.

**Prompt for Claude:** Using the vendor channel (`hio.stck:vnd`, vendor_token auth — see
[[reference_nfc_inf_record]] framing), attempt `SetParam alarm_0=...`; confirm rejection. Repeat
the identical write over NFC secret_key auth; confirm success.

- [x] Pass

> **HW-verified (2026-08-17, SN 2162199999, phone-free hand-crafted AES-CCM frames via
> `nfc write`/`nfc check`)**: vendor-channel `SetParam{alarms.alarm_0=...}` rejected —
> `error{code:NOT_WRITABLE, fault_field:403, detail:"transport not allowed"}`. Identical write
> over NFC `secret_key` auth succeeded (`ack{}`), `alarm list` confirmed slot 0 updated to the new
> rule. Decisive.

### X3 — H: history replay-active flag cleared on RECONNECT abort (M4)

**Goal:** A history replay aborted by a RECONNECT transition clears `app_history_set_replay_active
(false)` too, so `app_history_capture()` doesn't self-skip for the whole rejoin backoff.
**Observable:** After a RECONNECT-triggered replay abort, new samples ARE captured into history
during the backoff window (`history stats` count climbs), not frozen until rejoin.

**Prompt for Claude:** Trigger a history replay (`ReqHistoryPage` or `ats` equivalent), force a
RECONNECT mid-replay (radio-silence / bad gateway), then keep sampling during the backoff and
confirm `history stats` count keeps climbing.

- [x] Pass

> **HW-verified (2026-08-17, SN 2162199999, TTN)**: enabled history, captured 3 records, sent a
> raw `ReqHistory` LRW downlink (device-driven streaming replay), then forced WARNING→RECONNECT
> via `ats lrw lc fail` (needs the injections spaced ~3s apart — rapid-fire submits coalesce on
> `m_work_q`'s single debug-injection work item, only the last one before it runs counts).
> Confirmed `state: RECONNECT`, then ran 3 more `history capture` — count went **3 → 6** while
> still in RECONNECT, i.e. capture kept running through the backoff window, not frozen. Decisive.



### X4 — H: `ats lrw compose` runs on `m_work_q`, no longer races real TX

**Goal:** The debug `ats lrw compose` shell command composes on `m_work_q` instead of the shell
thread, so it can't race a real TX in flight.
**Observable:** Running `ats lrw compose` while a real telemetry send is in flight does not corrupt
the frame or crash; both complete cleanly.

**Prompt for Claude:** With a short `interval-report`, fire `ats lrw compose` repeatedly while
telemetry is actively sending; confirm no corruption/crash and both the manual and periodic frames
land on the LNS.

- [x] Pass

> **HW-verified (2026-08-17, sticker SN 2162199999, debug build @ `5d14b24`):** fired `ats lrw
> compose` three times back-to-back over RTT shell while the periodic 60 s telemetry cadence was
> live; a real periodic uplink landed concurrently (`fcnt up` 10→11 mid-sequence). All composes
> returned clean fPort-2 hex frames, `ats lrw status` stayed HEALTHY throughout, no crash/corruption.

### X5 — H `[HIL-only]`: `advance_page()` doesn't commit a ring page on flash erase/write failure

**Goal:** A flash erase/write failure during page eviction no longer commits the (bad) page to the
ring — success-path behavior is unit-tested, only the error path needs HW confirmation.
**Observable:** Forcing a page-evict failure leaves the ring state consistent (`history stats`
doesn't show a phantom committed page); no OOB/corruption on subsequent captures.

**Prompt for Claude:** Fill history to force page eviction, inject a flash erase/write failure at
that exact moment (may need a temporary build hook), confirm no page falsely committed and no
follow-on corruption.

- [x] Pass (partial — found a real, DECISIVE bug, filed as #384)

> **HW-verified (2026-08-17, SN 2162199999)**: `advance_page()`'s own erase/header-write correctly
> propagate failure (no phantom page commit) — that half is confirmed. But injecting one write
> failure exactly at the page-1→page-2 rollover hit **`flush_stage_pad()`** (called from inside
> `advance_page()` to close the outgoing page) — that call's `void` return type silently
> swallows the failure, so `advance_page()` still reported success. Follow-on `history read`
> showed the predicted corruption right at the boundary: records 1164-1168 read back
> implausible temperatures (195-203°C, clearly uninitialized/garbage flash bytes decoded as a
> value), then records 1169-1175 read back as the no-data sentinel (`-0.01`/`--`). Filed/updated
> as **#384** (was static-only, now HIL-decisive) — this is a real release blocker, not a
> theoretical concern.

### X6 — H: deferred NFC actions run after counters/sensor init (boot-staged `reset_counters`, #340 M8)

**Goal:** A boot-staged offline `reset_counters{hall_left:true}` no longer wipes ALL totalizers —
the deferred-action dispatch now runs after `app_counters_init()`/`app_sensor_init()`.
**Observable:** After staging `reset_counters` for hall_left only (NFC while powered off, then
boot), only `hall_left` resets to 0; `hall_right`/input counters keep their pre-boot NVS values.

**Prompt for Claude:** With non-zero counters on ≥2 channels, stage `reset_counters{hall_left}`
over NFC while powered off, reboot, confirm ONLY hall_left is zeroed — the others must survive.

> **Code-verified (2026-08-17)**: confirmed in `main.c` at the `5d14b24` tip that
> `nfc_run_deferred_cmd_actions()` is called at line 548, after `app_sensor_init()`/
> `app_counters_init()` (lines 532/539) and before `app_lrw_join()` (line 555) — the exact ordering
> the fix describes.
>
> **HIL-verified selectivity, decisive (2026-08-18, SN 2162199999)**: with a real magnet, got
> hall-left and hall-right both to nonzero counts (5/1 in one run, 1/1 in a follow-up).
> Hand-crafted an encrypted `reset_counters{hall_right:true}` Command via
> `nfc_helper.py`/`sticker_nfc_frame.py` (owner `hio.stck:cmd` channel, live nonce), wrote it to
> the tag, and forced processing with `nfc check` — RTT log showed
> `staged command action 9 (applied by poll thread)` (action 9 =
> `APP_CMD_ACTION_COUNTERS_SAVE`, confirming the fix's deferred-action dispatch actually ran), and
> `ats sensors sample` afterward showed the TARGETED channel zeroed while the OTHER channel's
> count was untouched — confirms `app_cmd_handle_reset_counters()` only zeros what it's told to,
> and the deferred persist doesn't clobber the sibling channel. Reused nonces/replayed frames
> along the way surfaced the NFC anti-replay cache (`M1`) behaving correctly (a byte-identical
> retransmission just replays the cached response instead of re-executing) — consistent with
> prior N8/N11 findings, not a new issue.
>
> **Genuine boot-staged-while-off race NOT independently reproduced**: writing the command frame
> via `nfc write` (raw I2C, not real RF) and then rebooting did not get picked up automatically —
> the NFC poll thread's own boot-time step unconditionally re-lays the plaintext info record over
> the tag before a manual `nfc check` ever sees the staged command, so this specific repro path
> can't distinguish "processed before `app_counters_init()`" from "never discovered at all"
> without real RF/phone hardware. The ordering fix itself is verified two independent ways
> instead: the code-level call-site ordering above, and (newly found while investigating this)
> the NFC poll thread's `K_THREAD_DEFINE(..., NFC_POLL_START_DELAY_MS)` = **3000 ms** startup
> delay (`main.c`), which structurally guarantees no NFC command can be discovered/dispatched
> before `main()`'s init sequence (well under 3s) reaches `app_counters_init()` — this delay,
> not `nfc_run_deferred_cmd_actions()`'s position, is what actually closes the boot-race window
> #340 M8 describes.
>
> **Bonus finding (not a bug)**: while chasing this, both hall magnets ended up near the board
> during a reboot and the device correctly entered calibration mode (temporary DevEUI
> `02403b84fd451f37`, `Device status: nfc-down`) per `app_calibration_detect_magnets()` —
> confirms that detection path works on real hardware too.

- [x] Pass — code-verified ordering + HIL-verified selective-reset behavior (see above); the
  exact at-boot race timing not independently reproduced without real RF/phone hardware

### X7 — H + M3 + M15 + M24: clm arm/rearm persist-after-confirmed-write + vendor decrypt doesn't consume + `m_clm_state` locked

**Goal:** (a) `clm_consume()` no longer fires on a vendor-authenticated decrypt (only `clm_ack` /
successfully-decrypted `hio.stck:cmd`); (b) the arm sequence (M3) and rearm sequence (M15) persist
`CLM_PENDING`/`UNSET` only after the tag write / config-save is confirmed, reverting instead of
latching a bad terminal state on failure; (c) `m_clm_state` is now locked against the shell
`ats claim active/done` commands racing the NFC poll thread (M24).
**Observable:** A vendor-channel decrypt on a claimed device does NOT flip `clm` state; an
inf-write failure during arm reverts to `CLM_UNSET` (retries next poll) instead of latching
`CLM_CONSUMED`; concurrent shell claim commands + NFC poll don't corrupt `clm` state.

**Prompt for Claude:** Reproduce the vendor-decrypt-doesn't-consume path per
[[project_pr362_hil_verify_0814]]'s 3 scenarios (already HW-confirmed once for the PR#362 test
suite itself — this is confirming the same behavior survived the subsequent M24 lock rework
merged in via PR#358). Then race `ats claim active`/`ats claim done` against live NFC field
activity and confirm no corrupted `clm` state (check via `ats claim status` / NVS readback).

- [x] Pass (partial — (a) confirmed, (c) not attempted)

> **HW-verified (2026-08-17, SN 2162199999)**: provisioned `claim-token` (was unset from an
> earlier `vendor_reset` this session) + `ats claim active` + `settings save` (a reboot was
> needed for the re-armed state to actually take — `ats claim active` alone didn't visibly change
> `ats claim status` until after save+reboot) → `clm state: pending (1)`. Sent a vendor-channel
> (`hio.stck:vnd`) `GetInfo` — `clm state` stayed `pending (1)` (not consumed). Immediately after,
> sent the identical `GetInfo` over NFC `secret_key` auth — `clm state: consumed (2)`. Confirms
> (a) decisively: only the owner-authenticated channel consumes the claim window, vendor-channel
> traffic does not. (c) (`ats claim active`/`done` raced against live NFC activity) not attempted
> this round — needs genuinely concurrent execution, not just back-to-back sequential calls.

### X8 — H: reset tiers reboot on mid-sequence failure; LoRaMac NVM wiped on factory/vendor reset

**Goal:** `settings reset`/`factory_reset`/`vendor_reset` now reboot (never return) even on a
mid-sequence failure, and wipe LoRaMac's own NVM (frame counters/DevNonce) alongside the key reset.
**Observable:** After `factory_reset`/`vendor_reset`, a fresh OTAA join uses `DevNonce`/frame
counters starting from 0 (no "already used" rejection from a network server that remembers the
old ones — see [[reference_cs_otaa_devnonce_flush]] for the failure mode this prevents).

**Prompt for Claude:** Join OTAA, exchange a few frames, `factory_reset` (or `vendor_reset`),
rejoin OTAA on the SAME network server without a manual DevNonce flush, confirm the join succeeds
cleanly (proving LoRaMac NVM was actually wiped, not just the app config).

- [x] Pass

> **HW-verified (2026-08-17, SN 2162199999, TTN)**: performed both a real `factory_reset` and a
> real `vendor_reset` over NFC this session (see X12). Both correctly zeroed `lrw-appkey` (+
> devaddr/nwkskey/appskey) while preserving `lrw-deveui`/`lrw-joineui`; after restoring the
> original AppKey via NFC `SetParam{lorawan.appkey}+save=true`, the device rejoined OTAA cleanly
> both times (fresh `devaddr`, `state: HEALTHY`) with no manual DevNonce flush and no "already
> used" rejection. Caveat: TTN is known to be more tolerant of DevNonce reuse than ChirpStack
> (#346) — a clean TTN rejoin is good supporting evidence but not as decisive as a ChirpStack
> repro would be; the underlying `lorawan_send`/NVM-wipe mechanism isn't network-specific though.

### X9 — H: alarm `rt_sync()` resets stale latch on any rule edit; RATE/COUNT holds after firing

**Goal:** Editing an alarm rule (not just source/quantity changes) resets its runtime latch; a
RATE/COUNT alarm holds after firing instead of re-firing every window.
**Observable:** Editing any field of an armed rule clears its latched state cleanly; a RATE/COUNT
alarm fires once per window then stays quiet (no report spam) until the condition genuinely
re-triggers.

**Prompt for Claude:** Arm a RATE or COUNT alarm (hall/input), trigger it, confirm it does NOT
re-fire every subsequent window while the condition persists — only once, then re-arms cleanly
after edit. Regression-tested in `tests/alarm_eval` already; this is HW confirmation only.

> **HW-verified, decisive (2026-08-18, SN 2162199999)**: armed `alarm new hall-left count 1 90`
> (`interval-report` shortened to 60s, its config min, for fast window cycling; `alarm-limit 0`
> for immediate uplink). User swiped a real magnet past hall-left (`hall-left count 0→1`); at the
> next 60s window boundary `alarm list` showed `active=1` — fired. A second swipe
> (`count 1→2`, still within the 90s dwell) produced **no** re-fire — `active` stayed `1`
> throughout the hold, confirming the `!rt->active` one-shot guard holds on real hardware, not
> just in `tests/alarm_eval`. Then `alarm set 0 hall-left count 1 45` (only `dwell` changed,
> 90→45) — `alarm list` immediately showed `active=0`: the stale latch was reset by the edit,
> exactly as `rt_sync()`'s memcmp-any-field-change design intends. A further magnet swipe after
> the edit (`count 4→5`) fired again cleanly (`active=1`) at the next window — confirms the reset
> re-arms properly rather than getting stuck. Full fire→hold→edit-reset→re-fire chain observed on
> real hardware. (Note: `CONFIG_LOG_MAX_LEVEL=2` on this debug build filters out the `LOG_INF`
> "Alarm batch" line — `alarm list`'s `active` field was the load-bearing observable, not the RTT
> log.)

- [x] Pass

### X10 — H: SetParam snapshot/apply/rollback mutex-guarded against cross-transport races

**Goal:** `app_cmd_handle_set_param()`'s snapshot/apply/rollback sequence can't be torn by a
concurrent `SetParam` on a different transport.
**Observable:** No corrupted config after firing `SetParam` on two transports back-to-back/
concurrently (e.g. NFC + LoRaWAN downlink landing close together).

**Prompt for Claude:** Fire a `SetParam` over NFC and a different `SetParam` over a LoRaWAN
downlink as close together as practically achievable; confirm both apply cleanly with no
torn/partial config afterward (`config show`/`GetConfig`).

- [x] Pass

> **HW-verified (2026-08-17, SN 2162199999)**: fired `SetParam{application.interval_sample=15}`
> as a raw (unencrypted) fPort-85 downlink via TTN and `SetParam{application.interval_report=90}`
> over NFC secret_key auth close together (downlink queued, NFC write+check issued immediately
> after). Both landed on the LNS/tag with `ack{}` and `config show` afterward showed
> `interval-sample=15` AND `interval-report=90` — both applied cleanly, no torn/partial config.
> LRW delivery took one uplink cycle to actually reach the device (fPort-85 downlinks piggyback on
> the next scheduled/forced uplink, not instantaneous), so the overlap wasn't sub-millisecond, but
> demonstrates the cross-transport race resolves correctly.

### X11 — M1: NFC encrypted-cmd response cache requires byte-exact match

**Goal:** A forged 8-byte `BE32(serial)||BE32(counter)` header frame at the cached counter is
rejected — the cache only serves a retransmission on a byte-exact ciphertext match.
**Observable:** Writing just the 8-byte header (no valid CCM tag) at the last-used counter does
NOT get served a cached reply / does NOT light the success LED.

**Prompt for Claude:** Send one legitimate encrypted command, note the response + counter. Craft
an 8-byte header-only frame `BE32(serial)||BE32(same counter)` (per
[[project_nfc_phone_free_testing_0811]]'s hand-crafted-frame technique) and write it; confirm it's
rejected (red LED / no cached reply), not silently accepted.

- [x] Pass

> **HW-verified (2026-08-17, SN 2162199999)**: sent a legit encrypted GetInfo, then wrote a bare
> 8-byte `BE32(serial)||BE32(same counter)` frame (no ciphertext/tag) at that counter —
> `-> command rejected: -22` (EINVAL), not served a cached reply. Decisive.

### X12 — M2: `nonce_counter` preserved across `vendor_reset`

**Goal:** `vendor_reset` no longer restarts the AES-CCM nonce counter from 0 under the unchanged
`vendor_token` key.
**Observable:** A previously-recorded `hio.stck:vnd` frame does NOT verify/replay after a
`vendor_reset`; `nonce_counter` reads back unchanged (not 0) post-reset.

**Prompt for Claude:** Record a legitimate vendor-channel frame + its nonce counter, perform
`vendor_reset`, replay the recorded frame — confirm rejection (nonce already used / out of window),
and confirm `nonce_counter` in `get_info`/`config show` did NOT drop to 0.

- [x] Pass

> **HW-verified (2026-08-17, SN 2162199999)**: recorded a legit vendor GetInfo at nonce 564,
> executed a real `vendor_reset` (new secret_key, over the vendor channel) — confirmed via reboot
> (DevEUI/claim_token wiped, secret_key changed, `serial_number`/`vendor_token` preserved).
> Post-reset `nonce-counter` read back as **565** (not 0). Replaying the recorded counter-564 frame
> was rejected (`-13`/EACCES, "nonce not greater than last used"). Fully decisive. LoRaWAN identity
> was then restored via NFC SetParam (deveui/joineui/appkey) and the device rejoined TTN cleanly.

### X13 — M5: telemetry trigger coalesced with a queued drain still composes

**Goal:** A telemetry-compose trigger that coalesces with an already-queued alarm/history drain on
`m_send_work` no longer gets silently dropped.
**Observable:** An interval boundary landing during an active drain still produces its fPort-2
telemetry frame on the LNS — no missing report for that interval.

**Prompt for Claude:** Arrange an alarm/history drain to be in flight right as `interval-report`
elapses (e.g. trigger several alarms right before the interval boundary), confirm the periodic
telemetry frame still appears on the LNS for that interval.

- [x] Pass (partial)

> **HW-verified, partial (2026-08-17, SN 2162199999, TTN)**: armed an `onboard temperature`
> threshold rule already violated by the live reading (immediate activation, no physical
> stimulus needed) and left it continuously active across several `interval-report` (60 s)
> cycles. LNS uplinks showed a steady repeating pair each cycle (plain fPort-2 telemetry +
> a second extended-group frame, ~10 s apart) with no missing interval over ~10 cycles. Confirms
> telemetry keeps flowing steadily with a live alarm condition in play; did not isolate the exact
> single-tick coalescing race (would need precise `m_send_work` timing control), so counted as
> supporting evidence rather than a fully isolated repro.

### X14 — M6: compose resets on frame abandon (regression-tested, HW confirmation only)

**Goal:** A frame abandoned after `FRAME_MAX_RETRIES` calls `app_compose_reset()`, so the next
cycle composes from a fresh snapshot instead of shipping the previous cycle's stale one.
**Observable:** After an abandoned frame (e.g. radio-silenced past max retries), the NEXT uplink's
sensor values reflect current readings, not the abandoned cycle's stale snapshot.

**Prompt for Claude:** Force a frame to exhaust `FRAME_MAX_RETRIES` (radio-silence during a
multi-frame send), then change a sensor value and confirm the next cycle's uplink reflects the NEW
value, not the abandoned frame's stale one. Already covered by `tests/compose`'s
`test_reset_after_abandon_forces_fresh_snapshot` — this is HW confirmation only.

- [ ] Pass

### X15 — M9: DevStatusReq reads cached battery voltage (ADC race eliminated)

**Goal:** `DevStatusReq`'s battery answer no longer triggers a live ADC read racing the sensor
thread — it reads the periodic sampler's cached voltage instead.
**Observable:** No hang/IWDG reset when a DevStatusReq lands during an active sensor sample; the
reported battery level in the LinkADRReq/DevStatusAns matches the last `ats sensors sample`
voltage reading (not stale/garbage).

**Prompt for Claude:** With a short `interval-sample`, request `DevStatusReq` from the network
server repeatedly while sampling is active; confirm no hangs/resets over an extended run, and the
reported battery level tracks `ats sensors sample`'s voltage.

- [x] Pass

> **HW-verified (2026-08-18, sticker SN 2162199999, debug build @ `ac9307f`, ChirpStack):**
> device-profile `device_status_req_interval` temporarily 1→96 (restored to 1 afterwards,
> verified by re-Get). `ats sensors sample` read 3.03 V; the FW mapping
> (`battery_level_callback()` reads `app_battery_last_sample()`,
> `raw = 1 + round(clamp((v−2.4)/1.2, 0, 1)·253)`) predicts raw 134 ≈ 52.6 %. Three
> uplink/downlink rounds ~60–90 s apart reported ChirpStack `device_status.battery_level`
> 53.14 % / 52.75 % / 52.75 % — all within one raw battery unit (~0.4 pp) of the prediction,
> rounds 2–3 byte-identical (consistent with a cached value, not per-request ADC noise).
> Uptime strictly increasing, reset cause unchanged, shell responsive throughout — no hang.

### X16 — M13: late `LinkCheckAns` not reprocessed

**Goal:** `lc_response_work_handler()` no longer double-applies a late `LinkCheckAns` that arrives
after the same link-check was already resolved implicitly by an intervening downlink.
**Observable:** In WARNING state, a downlink resolves LC implicitly, then a genuinely late
`LinkCheckAns` for that same request does NOT also count — no unearned WARNING→HEALTHY jump from
a single physical round trip.

**Prompt for Claude:** Drive into WARNING (L8), arrange a downlink to land right as a link-check
is pending (so it resolves LC implicitly), then confirm a late/duplicate `LinkCheckAns` doesn't
also increment `m_consecutive_lc_ok` a second time (`ats lrw status` consecutive-ok counter).

- [~] Pass (best-effort — race precondition achieved, magnitude structurally unobservable)

> **HW-attempted, best-effort PASS (2026-08-18, SN 2162199999, ChirpStack):** unlike the
> 2026-08-17 TTN/ChirpStack attempts, the precondition WAS achieved this time: in WARNING
> (3× `ats lrw lc fail`, ≥3 s apart), one enqueued fPort-85 downlink (`1a020801`, harmless
> GetParam) was delivered in the same RX window as the `ats lrw check` LinkCheckAns (queue
> confirmed drained). Result: exactly one clean WARNING→HEALTHY transition, `healthy->warning`
> stayed 0/3 (no spurious failure path), no reset, no bounce. Caveat: with
> `OK_THRESHOLD_HEALTHY = 1` a single LC success transitions immediately and resets
> `m_consecutive_lc_ok`, so +1 vs. a buggy +2 lands on the same visible `0/1` — the counter
> magnitude cannot distinguish the two on this config, and `debug.conf`'s
> `CONFIG_LOG_MAX_LEVEL=2` compiles out the guard's `LOG_DBG` line. The guard itself
> (`app_lrw.c` `lc_response_work_handler()`, `if (!m_link_check_pending) return;`) is
> statically confirmed; no anomaly was observable on hardware with the race forced.

### X17 — M17: `app_report_suspend()` cancels pending report work

**Goal:** `app_power_suspend()`'s call into `app_report_suspend()` now actually cancels
`m_periodic_work`/`m_trigger_work` on `m_work_q`, not just the timer — no radio touch after
suspend.
**Observable:** Triggering a report right before power-off/suspend does NOT cause a send after the
suspend call — no radio activity (TX log lines) following the suspend log line.

**Prompt for Claude:** Queue a report trigger, immediately invoke device suspend/poweroff, confirm
via RTT log that no compose/TX happens after the suspend log line.

- [x] Pass

> **HW-verified (2026-08-17, sticker SN 2162199999, debug build @ `5d14b24`):** `ats lrw check`
> (forces a link check + `app_report_trigger()`, queuing `m_trigger_work`) immediately followed by
> `power suspend`. Terminal log shows `Sending data with link check request` → `Suspending (deep
> sleep). Wake via NRST / power-cycle.` with **no** compose/TX line in between or after — the
> queued trigger work never ran. Woke via a full PPK2 power-cycle (OFF→ON), device rebooted cleanly
> (`Reset cause: pin, brownout`), rejoined LRW HEALTHY.

### X18 — M18: SHT4x implausible-but-valid reading no longer triggers a "no data" alarm

**Goal:** An out-of-range-but-CRC-valid SHT4x reading (`-ERANGE`) no longer overwrites the live
temperature/humidity with `NaN`, so it can't spuriously trip `APP_ALARM_NO_DATA_MS`.
**Observable:** Forcing one implausible-but-valid reading does not raise a no-data alarm; the
reported value holds the last known-good reading instead of going `NaN`.

**Prompt for Claude:** If a way exists to force an out-of-plausible-range SHT4x reading (extreme
thermal/humidity stimulus, or a debug hook), do one bad-but-valid sample and confirm no no-data
alarm fires and `ats sensors sample` shows the last good value, not NaN.

- [ ] Pass

### X19 — M19 + M27 `[HIL-only, best-effort]`: `m_i2c_fail_streak` + config reset ops locked (concurrency)

**Goal:** `m_i2c_fail_streak` is now lock-protected against concurrent sensor-WQ/report-timer/
shell/cmd-dispatch access (M19); `app_config_device_reset()`/`factory_reset()`/`vendor_reset()`
share a lock with `SetParam`'s snapshot/apply/rollback so neither can clobber the other (M27).
**Observable:** No missed/spurious I2C-bus-recovery trigger under concurrent shell+timer sampling
with forced I2C failures; no corrupted config from a `SetParam` racing a shell `settings reset`.
True races are inherently hard to force manually — treat this as a soak/stress best-effort, not a
guaranteed repro.

**Prompt for Claude:** Run `ats sensors sample` in a tight loop from the shell while the periodic
report timer also samples, with the I2C bus forced into failure (disconnect a sensor) — confirm
`i2c_recover_bus()` eventually fires (not stuck forever below threshold). Separately, fire
`SetParam` repeatedly while issuing `settings reset` from another channel — confirm no crash/torn
config.

> **Attempted (2026-08-18, SN 2162199999)**: physically disconnected the machine-probe (DS28E17
> bridge, `i2c1`, ROM `0000000553f7`, bound slot 1) mid-session and ran repeated
> `ats sensors sample`. Result: `ds28e17: w1_reset_select failed: -19` /
> `app_machine_probe: ds28e17_write_config failed: -19` every sweep (the bridge chip's own i2c
> address stops responding, as expected), but the onboard SHT4x readings kept succeeding
> throughout — so `i2c_failed != i2c_tried` every sweep and `m_i2c_fail_streak` never reaches
> `I2C_RECOVER_FAIL_THRESHOLD` (3). **This is correct, expected behavior, not a test failure**: a
> clean unplug makes one device stop ACKing, it does not wedge the shared bus (SDA held low),
> which is the specific failure mode `i2c_recover_bus()` targets. Reproducing a genuine bus-wedge
> would need a mid-transaction short/miswire, which carries real connector risk for uncertain
> payoff — not attempted, per the plan's own connector-risk caution for this item. Reconnected the
> probe afterward; `w1 scan` confirmed it re-detected cleanly (slot 1 restored).
>
> **Second half (SetParam vs. config reset race) not attempted**: `device_reset`/`factory_reset`/
> `vendor_reset` all reboot immediately as part of their deferred action, and forcing a genuine
> cross-transport race with hand-timed shell/NFC commands has low confidence of actually
> overlapping the lock window while being destructive to the current LoRaWAN identity/session —
> not a good time/risk trade-off for a best-effort item already covered by native_sim regression
> (per this item's own text). Left for a soak-test session with real concurrent transports
> instead of manual timing.

- [~] Partial — bus-wedge recovery path correctly NOT triggered by a clean single-device
  disconnect (expected); genuine wedge and the SetParam/reset race remain unattempted (soak-test
  material, not manual-timing material)

### X20 — M22: calibration TX decoupled from the blocking main loop

**Goal:** `app_calibration_run()` no longer calls the blocking `lorawan_send()` directly in its
main loop — a MAC-confirm stall can no longer wedge the calibration thread past its own watchdog
feed/deadline reboot.
**Observable:** Calibration mode stays responsive (LED blink continues, watchdog doesn't fire
unexpectedly) even if a send stalls; it self-recovers via its deadline reboot rather than hanging
silently.

**Prompt for Claude:** Enter calibration mode (S12/S12b), force a MAC-confirm stall (radio-silence
during calibration's send), confirm the calibration LED/heartbeat keeps running and the session
ends via its own deadline reboot rather than an unexplained hang/IWDG reset.

- [~] Partial

> **Partially HW-verified (2026-08-17, sticker SN 2162199999, debug build @ `5d14b24`):**
> `config calibration true` + `settings save` rebooted cleanly into calibration mode (temporary
> calibration DevEUI `02403b84fd451f37`, `Device status: nfc-down` as expected, `LRW state:
> healthy`) — the decoupled `app_lrw_run_on_work_q()` send path works with no hang on the normal
> (non-stalled) path, and a plain `ats device reboot` cleanly exited back to normal
> (`app_calibration_init()` auto-clears the flag). **Not yet confirmed:** the actual regression
> target — a forced MAC-confirm stall — needs reproducible radio silence (e.g. detach antenna or
> block the gateway) that wasn't set up this session.

- [ ] Pass (full stall scenario, still owed)

---

## New defects + fixes (2026-08-17 v1.4.0-final campaign follow-up: #383–#386)

Four additional defects found during this campaign, each fixed and (mostly) HIL-verified on the
same worktree/bench used for X1–X20 above (SN 2162199999, J-Link 822005109).

### #383 — `app_input.c` missing boot-time "prime" read (spurious edge on already-active input)

**Fix:** `app_input.c` gets the same `poll_impl(bool prime)` pattern `app_hall.c` already has
(#340 M14) — `app_input_init()` now seeds `m_input_data` from the real GPIO level once before
starting the periodic poll timer, without treating that seed as a rise/fall edge.

> **HW-verified, decisive A/B (2026-08-18, SN 2162199999)**: user physically shorted Input A
> (PB4) to GND (confirmed independent of firmware via direct J-Link `mem32 0x48000410,1` GPIOB
> IDR read = `0x00000000`, bit 4 low = active per `GPIO_ACTIVE_LOW`), then `cap-input-a`/
> `input-a-counter` were enabled and committed via `settings save` (reboots) while the short stayed
> in place across the reboot — the exact #383 scenario.
> - **Fixed build (this branch's actual code)**: post-boot `ats sensors sample` showed
>   `input-a: count=0 active=true` — level correctly read as active, but no rise counted. No
>   `LOG_WRN`/`LOG_DBG` rise event in the RTT log either.
> - **Negative control**: temporarily swapped in the pre-#383 `app_input.c` (parent commit
>   `abe4f4a^`, unfixed `poll()`/`app_input_init()`) with the same reboot conditions (Input A still
>   shorted) — this time the RTT log showed `XTEST: Input A activated (rise), count: 1`
>   immediately at boot, i.e. the bug reproduces exactly as described. (Note: the *final*, steady
>   -state `input-a-count` alone is not a usable observable for this — `app_counters_init()`
>   overwrites it from the persisted NVS totalizer shortly after boot (issue #340 L10) — the
>   transient rise had to be caught via a direct log at the decision point, not the settled count.)
> - Restored the real fix afterward and reconfirmed clean (`count=0 active=true`, no rise) before
>   moving on.
>
> This is a fully decisive fixed-vs-unfixed HIL comparison, not just a code-pattern match to the
> hall fix anymore.

- [x] Pass — HW-verified decisive (see above). Full `run_native.sh` (10 suites, 116 tests) and
  `clang-format --dry-run --Werror` both clean (there's no dedicated `app_input` ztest suite; this
  fix mirrors `app_hall.c`'s already-HIL-proven #340 M14 fix line-for-line, and is now separately
  HIL-confirmed itself, above).

### #384 — `flush_stage_pad()` silently swallows a flash-write failure at page rollover

**Fix:** `flush_stage_pad()` now returns the real `flash_area_write()` status instead of `void`,
and `advance_page()` propagates that failure instead of proceeding as if the outgoing page had
been durably closed.

> **HW-verified, pre-fix repro (2026-08-17, SN 2162199999, debug-history-flash build)**: fault
> injection landing exactly on the page-1→page-2 rollover produced the predicted corruption —
> records 1164-1168 read back as implausible temperatures (195-203°C), 1169-1175 as the no-data
> sentinel. This is the original decisive evidence the issue was filed on (see X5 above).
>
> **Post-fix re-verification, decisive (2026-08-18, SN 2162199999)**: the first re-verify attempt
> (still on the default temperature+humidity sensor set, `sample_size=3`) confirmed the injection
> point is structurally unreachable there — `records_per_page()*sample_size` is an exact multiple
> of `DW_DATA` (7) every rollover (588×3=1764=7×252), so `m_stage_len` is always exactly 0 and
> `flush_stage_pad()`'s early return (`if (m_stage_len == 0) return 0;`) bypasses the padded write
> this fix guards. Reconfigured the history sensor set to temperature+pressure+orientation
> (`sample_size=5`, via `history sensors humidity off`/`pressure on`/`orientation on`, backed by
> live-enabled `cap-barometer`/`cap-accelerometer`) — `records_per_page()=352`,
> `352×5=1760`, `1760 mod 7 = 3`, so `m_stage_len` is nonzero at every rollover in this config.
> Filled to the 3rd page-rollover boundary (record 1056→1057) with one fault armed
> (`history debugfail 1`): the injection fired on an *automatic* periodic capture (not a manual
> shell one) — RTT log showed `XTEST: injecting flush_stage_pad write failure (0 left)` followed
> by `history append failed — record dropped`, and the stored count correctly stayed at 1056 (not
> silently advanced). The very next capture attempt safely retried the identical rollover (fault
> already consumed) and succeeded cleanly, advancing to 1057. `history read` across the boundary
> (records 1043-1057) showed all-plausible temperatures (20.16-20.90°C) with no implausible
> garbage and no unexpected no-data run — a clean pass, in stark contrast to the original pre-fix
> repro's corruption at the same kind of boundary. This is now a fully decisive post-fix HIL
> confirmation, not just repro + review.
>
> Bench restored to its pre-test sensor/capability configuration (temperature+humidity,
> `cap-barometer`/`cap-accelerometer`/`cap-input-a` back to `false`) and the debug-only fault
> -injection hook (`history debugfail`) reverted out of the tree before the final clean reflash.

- [x] Pass — HW-verified decisive post-fix repro (see above), on top of the original pre-fix
  corruption repro + code review + `native_sim` history suites green.

### #385 — `vendor_reset` with an all-zero key returns a false-positive `Ack`

**Fix:** `app_cmd_handle_vendor_reset()` now calls `buffer_is_zero()` on the replacement key and
rejects synchronously with `BAD_REQUEST`, matching the sibling `set_secret_key` handler, instead
of Ack'ing and letting `app_settings_vendor_reset()`'s own `key_is_set()` check silently no-op
the deferred action later with no error surfaced to the caller.

- [x] Pass — new `tests/cmd` ztest `test_vendor_reset_rejects_zero_key` (35/35 `cmd` suite tests
  green): confirms `APP_CMD_ACTION_NONE` (no action staged) and a `BAD_REQUEST` error response for
  an all-zero vendor_reset key, instead of the previous no-op-behind-an-Ack.

### #386 — `clock set`/network time sync missing an upper (year-2100) bound

**Fix:** `app_clock_set_unix()` now rejects `unix_s >= APP_CLOCK_UNIX_MAX` (4102444800,
2100-01-01T00:00:00Z) before ever calling `gmtime_r()`/`rtc_set_time()` — closing the one path
(`clock set` debug shell) that lacked the bound already enforced on the real network-time
(`app_clock_handle_downlink()`) and NFC `clock_sync` Command paths.

> **HW-verified (2026-08-17, SN 2162199999)**: `clock set 4102444800` correctly rejected
> (`app_clock_set_unix failed: -22`), no reset. Caught an off-by-one in the first version of this
> fix via HIL re-test: `unix_s > APP_CLOCK_UNIX_MAX` let the exact boundary value through and it
> still set successfully — fixed to `>=` in a follow-up commit. Also confirmed the real
> `clock_sync` Command path (LoRaWAN/NFC) was **never actually vulnerable** — only the debug
> `clock set` shell command was, since `app_cmd_handle_clock_sync()` already had the same `>`
> bound check the network-time path uses.

- [x] Pass (HW-verified, boundary + off-by-one both confirmed)

---

## Final-review fixes (2026-08-18 pre-merge deep review)

A multi-agent static review of the whole `v1.4.0` surface (8 subsystem finders, each finding
adversarially verified) ran as the last gate before the merge to `main`. Five confirmed defects
were fixed; every fix carries native regression coverage where the logic is host-testable.

### FR-1 — deactivate edge lost when an ACTIVE rule is cleared/disabled/edited (`app_alarm.c`)

`rt_sync()` reset an active latch *before* `eval_threshold()`/`eval_state()` ran, so their
`!rule->enabled` deactivate branches could never see the pre-reset latch — the fPort-3
deactivate edge for that alarm instance was silently skipped (dangling activate for any backend
pairing edges; the LED/status snapshot cleared correctly, only the event was lost).
**Fix:** `rt_sync()` emits the deactivate edge itself when resetting an active latch (clear,
disable, and in-place edit all covered). Covered by 3 new `tests/alarm_eval` cases
(`test_clearing/disabling/editing_active_rule_emits_deactivate_edge*`) via a new event-capture
stub.

> **HW-verified (2026-08-18, SN 2162199999, debug build @ this branch, ChirpStack):**
> `alarm set 0 onboard temperature -50 10 0` with the bench at 22.7 °C + `alarm poll` →
> fPort-3 activate uplink captured live via the gRPC event stream
> (`{"event":"activate","quantity":"temperature","slot":0,...}`); `alarm clear 0` at unix
> 1787036234 → fPort-3 **deactivate** uplink captured with device-side event time 1787036235
> (1 s after the clear, decisively attributable):
> `{"slot":0,"event":"deactivate","quantity":"temperature","source":"onboard","type":"high"}`.
> No reset, uptime monotonic.

### FR-2 — mid-record flash-write failure misaligns the rest of the history page (`app_history.c`)

`backend_append()` streams a record as multiple double words; a write failure past the record's
first byte left the already-flushed DWs (and, on a first-DW failure, the previous record's
staged tail) as an unaccounted gap in the page byte stream — every later record in that page
read back shifted/garbage (sibling of the #384 rollover bug, one layer deeper).
**Fix:** on such a failure the page is closed (`m_head_full = true`), so the next append opens a
fresh page and the stream re-aligns; a no-bytes-lost failure keeps the page open. Blast radius
shrinks from "rest of the page" to "at most the failed record".

### FR-3 — command Ack lost when a duty-cycle backoff outlives the reboot deferral (`app_lrw.c`)

The post-command action (reboot/save/reset over the fPort-85 downlink port) fired at a fixed
8 s, but a failed `lorawan_send()` requeues the Ack with a 15 s retry backoff — the reboot always
won and dropped the RAM-only queued Ack. **Fix:** the action defers in 8 s steps (max 6, ~56 s
cap) while the response queue is non-empty or a TX retry is pending.

### FR-4 — `vendor_reset_allow` silently re-enabled by device/factory reset (`app_config.yml`)

The field had no `persistent:` tier, so `device_reset`/`factory_reset` reset an owner's
deliberate `false` back to the default `true`. **Fix:** `persistent: [device_reset,
factory_reset]` (+ configen regen). `vendor_reset` still restores the default (it only runs when
allow was true anyway). Guarded by the existing generic
`test_reset_ops_preserve_only_their_tier` pytest.

> **HW-verified (2026-08-18, SN 2162199999):** `config vendor-reset-allow false` +
> `settings save` → `settings device-reset` → `vendor-reset-allow` still `false` after the
> reset (other volatile config correctly back at defaults, identity/LoRaWAN keys untouched).
> Restored to `true` afterwards.

### FR-5 — debug `ats lrw compose` could feed the real uplink a stale snapshot (`app_compose.c`)

The debug probe and the real TX path share one snapshot state machine as interleavable per-frame
work items on `m_work_q`; a periodic report landing mid-debug-session drained the *debug*
session's remaining groups as the real over-the-air uplink (milder sibling of #340 M16).
**Fix:** the active session is tagged debug/real — the real path drops a leftover debug snapshot
and composes fresh; the debug probe returns `-EBUSY` during an in-flight real report.

### FR-6 — smaller items in the same batch

- `nfc read <off> <len>` shell: `off + len` integer wraparound bypassed the range check →
  potential 512 B+ read past `m_buf` (dev-shell only). Bounds now checked per-operand.
  HW-verified 2026-08-18: `nfc read 10 4294967286` rejected; boundary `496 16` accepted,
  `496 17` rejected; no crash, uptime monotonic.
- Boot path: extra IWDG feed after the 5 s LED carousel — the init chain below gets the full
  10 s budget instead of the remainder (thin-margin hardening, no observed overrun).
- Dead code removed: `app_history_is_enabled()`, `app_history_sensor_name()`,
  `app_settings_erase()` (the shell command uses the internal `erase()` directly),
  `app_machine_probe_enable/disable_tilt_alert()` (+ the orphaned static
  `lis2dh12_disable_alert()`; tilt alert is armed internally at scan time, `get` stays).
- Noted, deliberately NOT changed: `tx_send_queued()` retries responses/alarms without a cap —
  unlike bulk telemetry/history these are low-volume and precious, and the backoff retry
  eventually succeeds; a cap would only convert late delivery into silent loss.

---

## Run record

| Field | Value |
|-------|-------|
| Firmware version | v1.4.0 |
| Build variant | debug / release |
| Network(s) | TTN / ChirpStack |
| Tester | |
| Date | |
| Result | _N_ / _M_ passed |
