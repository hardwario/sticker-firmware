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
| `set_param`: ADR on, `interval_report`=120 s, `alarm_0`=onboard temp 5–30 °C (hyst 1) | `0801121e0a021801120220782a14b2031103000000000000a0400000f0410000803f` |
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
> `alarm-limit` (rate-limit) and `alarm-notif-time` (LED hold + one-shot re-arm) still apply.

### A1 — Threshold alarm (onboard temperature)

**Goal:** Crossing an analog bound raises an alarm with hysteresis.
**Observable:** AlarmReport on fPort 3, source `onboard`, quantity `temperature`, edge + side
(LO/HI); RTT alarm log; orange LED.

**Prompt for Claude:**
> Arm `alarm set 0 onboard temperature <lo> <hi> <hst>` with bounds near the current room
> temperature (note the values); confirm with `alarm list`. Ask me to warm/cool the sensor across a
> bound. Confirm an AlarmReport on fPort 3 with source `onboard`/quantity `temperature`, the correct
> side (LO/HI), and that hysteresis prevents immediate re-triggering. Decode and report the event.

- [ ] Pass

### A2 — Threshold alarms: humidity / pressure / 1-Wire slots

**Goal:** The other analog quantities behave like temperature.
**Observable:** AlarmReport fPort 3 with the matching source and side.

**Prompt for Claude:**
> For each present analog quantity (onboard humidity/pressure, and 1-Wire slot s1…s4 temperature/
> humidity), arm `alarm set <i> <source> <quantity> <lo> <hi> [hst]`, stimulate a crossing, and
> confirm an AlarmReport on fPort 3 with the correct source/quantity and LO/HI side. Summarize per
> source; mark any sensor not fitted as N/A.

- [ ] Pass

### A3 — Binary alarm: Hall (state edge & level)

**Goal:** A `state` rule on a hall sensor fires on the configured transition (edge) or while held
(level).
**Observable:** AlarmReport fPort 3, source `hall-left`/`hall-right`, quantity `state`.

**Prompt for Claude:**
> Arm an **edge** rule `alarm set 0 hall-left state 0 1` (fires on 0→1) and confirm with
> `alarm list` it reads `0->1 (edge)`. Ask me to apply/remove a magnet; confirm an AlarmReport on
> fPort 3 (source `hall-left`) on the rising edge. Then arm a **level** rule
> `alarm set 0 hall-left state 1 1` (`1->1 (level)`) and confirm it stays active while the magnet is
> present. Report both.

- [ ] Pass

### A4 — Binary alarm: Input A/B (state)

**Goal:** Input edges/levels raise `state` alarms.
**Observable:** AlarmReport fPort 3, source `input-a`/`input-b`, quantity `state`.

**Prompt for Claude:**
> With PIR disabled (shared pins), arm `alarm set <i> input-a state 0 1` (and `input-b`). Toggle
> each input and confirm AlarmReports on fPort 3 with source `input-a`/`input-b`. Report results.

- [ ] Pass

### A5 — Momentary alarm: PIR / accel (state one-shot, #150)

**Goal:** PIR and accel `state` alarms fire as per-pulse one-shots that re-arm (they only ever
pulse, never report a level).
**Observable:** AlarmReport fPort 3, source `pir`/`accel`, quantity `state`, edge ACTIVATE; one
report per pulse, suppressed within `alarm-notif-time`, then re-armed; a motion burst is
flood-suppressed (no permanent latch).

**Prompt for Claude:**
> Enable the sensor (`config cap-pir-detector true` / `cap-accelerometer true`, save). Arm
> `alarm set 0 pir state 0 1` (or `accel state 0 1`) — note edge and level behave alike for these
> momentary sources. Ask me to trigger motion repeatedly; confirm each pulse fires an AlarmReport
> (source `pir`/`accel`, ACTIVATE), that reports within `alarm-notif-time` are suppressed and it
> re-arms after, and that a sustained burst produces only periodic reports (not a flood, not a stuck
> `active`). Report the cadence.

- [ ] Pass

### A6 — Count / rate alarm (hall / input)

**Goal:** A `count` rule fires when a counter exceeds the per-interval rate.
**Observable:** AlarmReport fPort 3, source `hall-left`/`hall-right`/`input-a`/`input-b`, quantity
`count`.

**Prompt for Claude:**
> Arm `alarm new hall-left count <N>` (small N; `alarm list` shows `rate>=N/interval`). Ask me to
> pulse the hall sensor more than N times within a report interval and confirm an AlarmReport on
> fPort 3 for that source/quantity. Report the result.

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
`01…`) keeps the slot listed with `en=0` and is **not** evaluated.

**Prompt for Claude:**
> Using slot 0: (1) **change** it — `alarm set 0 onboard temperature 0 10 1` then re-set to
> `5 30 1`, confirm `alarm list` reflects each. (2) **deactivate** it over SetParam by writing
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

### A12 — Event LED (orange)

**Goal:** Orange LED indicates alarms and auto-off after 60 min.
**Observable:** Orange (red+yellow) blink on alarm, held `alarm_notif_time` s; auto-off 60 min
after boot.

**Prompt for Claude:**
> Set `alarm_notif_time` (1–60 s, note the value). Trigger an alarm and tell me what to watch:
> confirm the orange LED blinks and is held for ~`alarm_notif_time` seconds. Note the documented
> 60-minute post-boot auto-off (don't wait it out unless asked) and report your observation of the
> blink/hold timing.

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

### N2 — NFC config delivery: RESET — SUPERSEDED

The dedicated plaintext "RESET NFC tag" NDEF type this test described predates the reset ladder
(#299) and no longer exists in `app_nfc.c` (no `NFC action: RESET` string, no 10-blink pattern).
Resetting over NFC now goes through the encrypted `hio.stck:cmd` channel's `device_reset` /
`factory_reset` commands (ack-before-reboot, same as every other command) or the separate
vendor-token-authenticated `hio.stck:vnd` channel for `vendor_reset` — see **N9** and **G6a-NFC**.

- [x] N/A (superseded by N9 / G6a-NFC)

### N3 — NFC firmware update — REMOVED

The NFC firmware-update path (erase-in-place bootloader + DFU protocol + ST25DV FTM mailbox) was
implemented during v1.4.0 and **removed before release**: it could not be shipped safely without
asymmetric image signing, which does not fit the flash budget. The device is **not NFC-updatable**.
Revival is tracked in #237.

- [x] N/A (feature removed)

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

### N6 — Mailbox (Fast-Transfer-Mode) command channel — REMOVED

The ST25DV mailbox (Fast-Transfer-Mode) channel and the `enter_mailbox` / `exit_mailbox` commands
were removed together with the NFC firmware-update path (they shared the FTM machinery). All
config/command exchange goes over the NDEF channel only (see N4 / N5 / N8). Revival is tracked in #237.

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

## Run record

| Field | Value |
|-------|-------|
| Firmware version | v1.4.0 |
| Build variant | debug / release |
| Network(s) | TTN / ChirpStack |
| Tester | |
| Date | |
| Result | _N_ / _M_ passed |
