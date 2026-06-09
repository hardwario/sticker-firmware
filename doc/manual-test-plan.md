# Manual Test Plan — STICKER firmware v1.4.0

> Target firmware: **v1.4.0**. The scenarios below cover the v1.4.0 feature set
> (remote control protocol, protobuf telemetry/alarms, RTC sync, history store-and-forward).

A canonical, versioned checklist of manual / hardware test scenarios for release
verification. Each task carries a ready-to-use **prompt for Claude**: copy it into a
Claude Code session that has the sticker attached over `rttt` and access to TTN
(TTS MCP) and ChirpStack, and Claude will drive the scenario autonomously and report
pass/fail. The tester only watches.

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
| `reboot` | `08083a00` |
| `reset_counters` (hall-left + input-a) | `0807520408011801` |
| `set_param`: ADR on, `interval_report`=120 s, `alarm_temperature_hi`=50 °C | `0801120d0a021801120720783d00004842` |

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

**Goal:** Device answers GetInfo with version, serial, uptime, clock, build type.
**Observable:** `Response.Info` on fPort 85 with `fw_major/minor/patch`, `build_type`,
`serial_number`, `uptime_s`, `unix_time`, `debug`.

**Prompt for Claude:**
> Send a GetInfo command to the joined device. In a debug build you may inject it locally with
> `ats cmd lrw <hex>` over the RTT shell (the device queues the Response on fPort 85), or send it
> as a real downlink via the TTS MCP `send_downlink` on fPort 85. Then read the fPort 85 uplink
> via `get_uplinks` and decode the `Response.Info`. Confirm `fw_*` matches the boot version,
> `serial_number` is non-zero, `uptime_s` is plausible, and `build_type`/`debug` are correct.

- [ ] Pass

### G5 — Reboot command

**Goal:** Remote reboot works and is acknowledged.
**Observable:** `Response.Ack` on fPort 85, then the boot log repeats (`Firmware version: ...`).

**Prompt for Claude:**
> Send a Reboot command as a real downlink on fPort 85 (TTS MCP `send_downlink`). While watching
> the RTT log, confirm the device sends a `Response.Ack`, then reboots — i.e. the `Firmware
> version:` boot line reappears. Report the time between the downlink and the reboot.

- [ ] Pass

### G6 — Factory reset

**Goal:** Factory reset clears NVS and reboots.
**Observable:** Shell `settings reset` (or FactoryReset downlink) → NVS cleared, device reboots,
config back to defaults.

**Prompt for Claude:**
> First capture a couple of non-default config values via `config` (e.g. `config interval-report`).
> Then run `settings reset` over the RTT shell (this is destructive — confirm it's acceptable on
> this bench unit). Confirm the device reboots and the previously-changed values are back to
> defaults. Optionally repeat using a FactoryReset downlink on fPort 85 and confirm the
> `Response.Ack` precedes the reboot.

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
**Observable:** RTT `LC FAIL in HEALTHY (streak: n/3)`; WARNING after 3 fails; RECONNECT after 5.

**Prompt for Claude:**
> Provoke link-check failures (e.g. power down / move the gateway out of range, note the method).
> Watching the RTT log, confirm the state machine escalates: `LC FAIL in HEALTHY (streak: n/3)`,
> transition to WARNING after 3 fails, then `Entering RECONNECT state` after 5 total fails. Then
> restore the gateway and confirm one `LC OK` returns the device toward HEALTHY. Report the
> observed thresholds.

- [ ] Pass

### L9 — Rejoin with exponential backoff (OTAA)

**Goal:** In RECONNECT the device retries join with growing backoff.
**Observable:** RTT `Entering RECONNECT state - will rejoin in 5 seconds`, then `Rejoin attempt N`
with increasing delay (60→3600 s); ABP logs `ABP mode - rejoin not applicable`.

**Prompt for Claude:**
> With an OTAA device driven into RECONNECT (gateway unreachable), confirm from the RTT log the
> rejoin attempts fire with exponential backoff (base 60 s, ×2, capped 3600 s) and the
> `Rejoin attempt N` counter increments. Then bring the gateway back and confirm it rejoins and
> returns to HEALTHY. Separately, on an ABP device, confirm rejoin is skipped with
> `ABP mode - rejoin not applicable`.

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

### L12 — Legacy bitmap telemetry on fPort 1

**Goal:** Backward-compatible v1.3.x bitmap telemetry is still emitted alongside the new fPort 2
protobuf.
**Observable:** Each report produces a legacy bitmap uplink on **fPort 1** in addition to the
protobuf on fPort 2 (`doc/version 1.4.md` §2).

**Prompt for Claude:**
> Trigger a telemetry report (`send` or ForceSend) and inspect the network-server uplinks. Confirm
> that both a **fPort 1** legacy bitmap frame and a **fPort 2** protobuf frame are emitted for the
> same report, and that the fPort 1 frame still decodes with the legacy v1.3.x decoder. Report both
> frames.

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
`cap_1w_thermometer`.

**Prompt for Claude:**
> Confirm `cap_1w_thermometer` is enabled and the probes are wired. Run `ats sensors sample` and
> report `ext1`/`ext2` temperatures; sanity-check them. Ask me to warm one probe (e.g. by hand)
> and confirm that channel's reading rises. Confirm the values reach fPort 2 telemetry.

- [ ] Pass

### S7 — 1-Wire machine probe MP1/MP2

**Goal:** Machine-probe temp/humidity and tilt flag work.
**Observable:** Telemetry `mp1_temperature`/`mp1_humidity` (+`mp1_flags` tilt bit), same for MP2;
requires `cap_1w_machine_probe`.

**Prompt for Claude:**
> Confirm `cap_1w_machine_probe` is enabled and the probe(s) connected. Read MP1/MP2 temp and
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

---

## Alarms

### A1 — Temperature threshold alarm

**Goal:** Crossing the temperature bound raises an alarm with hysteresis.
**Observable:** AlarmReport on fPort 3, source `temperature`, edge + side (LO/HI); RTT alarm log;
orange LED.

**Prompt for Claude:**
> Enable the temperature alarm and set `temperature_alarm_lo`/`hi`/`hst` to bounds near the
> current room temperature (note the values). Ask me to warm/cool the sensor across a bound.
> Confirm an AlarmReport arrives on fPort 3 with source `temperature`, the correct edge and side
> (LO/HI), and that hysteresis prevents immediate re-triggering. Decode and report the event.

- [ ] Pass

### A2 — Threshold alarms: humidity / pressure / T1 / T2

**Goal:** The remaining threshold alarms behave like temperature.
**Observable:** AlarmReport fPort 3 with the matching source and side.

**Prompt for Claude:**
> For each of humidity, pressure, T1 and T2 (whichever sensors are present on this unit), enable
> the alarm with bounds around the current reading, stimulate a crossing, and confirm an
> AlarmReport on fPort 3 with the correct source and LO/HI side. Summarize results per source;
> mark any sensor not fitted as N/A.

- [ ] Pass

### A3 — Discrete alarm: Hall

**Goal:** Hall activate/deactivate edges raise alarms.
**Observable:** AlarmReport fPort 3, source `hall-left`/`hall-right`, edge ACTIVATE/DEACTIVATE
per `*_notify_act`/`*_notify_deact`.

**Prompt for Claude:**
> Enable hall notify-on-activate and notify-on-deactivate. Ask me to apply and remove a magnet on
> a hall sensor. Confirm AlarmReports on fPort 3 with source `hall-left`/`hall-right` and the
> ACTIVATE then DEACTIVATE edges. Report the decoded events.

- [ ] Pass

### A4 — Discrete alarm: Input A/B

**Goal:** Input edges raise alarms.
**Observable:** AlarmReport fPort 3, source `input-a`/`input-b`, edge per notify config.

**Prompt for Claude:**
> Enable input notify-on-activate/deactivate (and ensure PIR is disabled — shared pins). Toggle
> each input and confirm AlarmReports on fPort 3 with source `input-a`/`input-b` and correct
> edges. Report results.

- [ ] Pass

### A5 — PIR alarm (activate-only)

**Goal:** PIR motion raises an activate-only alarm.
**Observable:** AlarmReport fPort 3, source `pir`, edge ACTIVATE only (no DEACTIVATE); requires
`pir_notify_act`.

**Prompt for Claude:**
> Enable `pir_notify_act`. Ask me to trigger motion. Confirm an AlarmReport on fPort 3 with source
> `pir` and edge ACTIVATE, and confirm there is **no** DEACTIVATE edge for PIR. Report the event.

- [ ] Pass

### A6 — AlarmReport structure

**Goal:** AlarmReport fields are well-formed.
**Observable:** `base_time`, `total`, `events[]` with `source`, `edge`, `side`, `rel_s`, and
optional scaled `value` (×100 temp/hum, ×10 pressure; absent for discrete).

**Prompt for Claude:**
> Capture any AlarmReport on fPort 3 and fully decode it. Confirm `base_time` and `total` are
> sensible, each event has a valid `source`/`edge`/`side`/`rel_s`, threshold events carry a scaled
> `value` (×100 for temp/hum, ×10 for pressure) while discrete events omit it, and that no more
> than 8 events appear per frame. Report the decoded structure.

- [ ] Pass

### A7 — Alarm rate-limiting

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

### A8 — Event LED (orange)

**Goal:** Orange LED indicates alarms and auto-off after 60 min.
**Observable:** Orange (red+yellow) blink on alarm, held `alarm_notif_time` s; auto-off 60 min
after boot.

**Prompt for Claude:**
> Set `alarm_notif_time` (1–60 s, note the value). Trigger an alarm and tell me what to watch:
> confirm the orange LED blinks and is held for ~`alarm_notif_time` seconds. Note the documented
> 60-minute post-boot auto-off (don't wait it out unless asked) and report your observation of the
> blink/hold timing.

- [ ] Pass

### A9 — Dual uplink per edge & `alarm-limit = 0`

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

### N2 — NFC config delivery: RESET

**Goal:** A RESET NFC tag triggers factory reset.
**Observable:** RTT `NFC action: RESET`; ~10 yellow blinks; config back to defaults; reboot.

**Prompt for Claude:**
> Tell me how to present a RESET NFC tag. On tap, confirm the RTT log shows `NFC action: RESET`,
> the yellow LED blinks ~10×, the device factory-resets and reboots, and config returns to
> defaults. (Destructive — confirm OK on this bench.)

- [ ] Pass

### N3 — NFC firmware update

**Goal:** NFC-based FW update path works (if active in this build).
**Observable:** Per `doc/nfc-update-protocol.md` (frame format, status codes, state machine).

**Prompt for Claude:**
> Check whether NFC firmware update is active in this build (consult `doc/nfc-update-protocol.md`
> and the build config). If it is, outline and run the update handshake and confirm the status
> codes/state transitions match the protocol doc. If it is not yet wired up, mark this task as
> TODO/N-A and say what's missing.

- [ ] Pass / TODO

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
