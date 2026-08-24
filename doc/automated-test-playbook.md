# STICKER — Automated Hardware-in-the-Loop Test Playbook

**Audience: an AI agent** (Claude Code or similar) driving a real STICKER device on a test
bench. This document is an *operating manual*, not a checklist for humans — every scenario
is written so an agent can execute it with the tools described in Part I, and knows exactly
when it must stop and ask the human operator for a physical action.

It complements — does not replace — [`manual-test-plan.md`](manual-test-plan.md) (the
authoritative per-feature manual plan, IDs G/L/S/H/A/C/K/N/F). Every scenario here maps back
to one or more manual-plan IDs in the coverage matrix (§21). Behavioural reference:
[`version 1.4.md`](<version 1.4.md>). Power baseline: [`power-consumption.md`](power-consumption.md).

Values in `{CURLY_BRACES}` are **bench placeholders** — never guess them; resolve every one
of them in Step 0 before doing anything else.

---

## Part I — Bench, toolchain, rules

## 0. Step 0 — Bench intake (MANDATORY first step)

Before *any* tool call that touches hardware, the agent MUST collect the bench profile from
the operator. Ask the questions below (batched, one questionnaire). If a local bench annex
file is available (see below), pre-fill each answer with its value and ask the operator only
to confirm/correct. **A missing answer never blocks the run: mark every scenario that
depends on it as SKIPPED (with the missing placeholder named) — do not guess.**

> Bench annex: operators keep a private, non-committed file (e.g.
> `~/Documents/claude/sticker-test-bench.md`) with a `placeholder → value` table matching
> this questionnaire exactly. If present, load it first and use it as defaults.

| # | Question | Placeholder(s) | Notes |
|---|----------|----------------|-------|
| 1 | Which J-Link probe drives the STICKER (serial number)? Are other probes connected? | `{JLINK_SN}` | ALWAYS pass explicitly (`--serial` / `-USB` / `--dev-id`). Never auto-pick. |
| 2 | Device serial number + 16-byte secret key (hex)? | `{STICKER_SN}`, `{STICKER_KEY}` | Key needed for NFC crypto and Manager-App comms. |
| 3 | Firmware ref and variant under test? | `{FW_REF}`, `{FW_VARIANT}` | e.g. `origin/v1.4.0`, `debug` or `release`. Determines the executable scenario set (§5). |
| 4 | RTT control-block address? | `{RTT_ADDR}` | LTO builds: `0x20000000`; older non-LTO: `0x20000800`. Verify: `arm-zephyr-eabi-nm build/zephyr/zephyr.elf \| grep -w _SEGGER_RTT`. |
| 5 | rttt MCP port to use? | `{MCP_PORT}` | Default 8090. If another agent shares the bench, take a different port (e.g. 8091). |
| 6 | ChirpStack: host, API token, application ID, device credentials (OTAA + ABP)? | `{CS_HOST}`, `{CS_TOKEN}`, `{CS_APP_ID}`, `{CS_OTAA_DEVEUI}`, `{CS_OTAA_APPKEY}`, `{CS_ABP_DEVEUI}`, `{CS_ABP_DEVADDR}`, `{CS_ABP_NWKSKEY}`, `{CS_ABP_APPSKEY}` | Primary LNS. |
| 7 | TTN/TTS: host, application, API key, device credentials? | `{TTS_HOST}`, `{TTS_APP_ID}`, `{TTS_API_KEY}`, `{TTS_OTAA_DEVEUI}`, `{TTS_OTAA_APPKEY}`, `{TTS_ABP_DEV_ID}` | Secondary LNS (decoder parity). |
| 8 | Shared JoinEUI? | `{JOIN_EUI}` | Same for both networks in current bench practice. |
| 9 | Is a LoRa gateway online and in RF range? Which one? | `{GW_ID}` | If no: all join/OTA scenarios → SKIPPED (RF), shell-level LRW tests still run. |
| 10 | Is a phone with the Manager-App **debug build** connected over adb? | `{PHONE_ADB_SERIAL}` | Enables §10 NFC-via-phone scenarios. `adb devices` to verify. |
| 11 | Is a PPK2 powering the device? At what voltage? | `{PPK2_MV}` | Default 3000 mV. Enables §15 power + §12 undervoltage scenarios. |
| 12 | Which sensor capabilities are fitted/enabled, and what external probes are on hand? | `{SENSORS_FITTED}`, `{W1_PROBES}` | e.g. `cap-w1-sensors`, `cap-accelerometer`, PIR; number of DS18B20 probes, magnet available, etc. |
| 13 | How much operator time is available for physical assistance, and when? | `{ASSIST_MINUTES}` | Drives scenario selection + assist batching (§18). |
| 14 | Am I allowed to flash the device in this session (and which images)? | `{FLASH_OK}` | Flashing ALWAYS requires explicit per-image consent (§4). |

After intake, the agent MUST:

1. Print the resolved **bench profile** (placeholder → value table; mask secrets to first/last
   4 hex chars) and get one confirmation.
2. Filter the coverage matrix (§21) by available hardware, FW variant, and
   `{ASSIST_MINUTES}` into a **run plan**: ordered scenario list with expected duration and
   the assist batches (§18) marked. Present the run plan before executing.
3. Record intake answers at the top of the run's evidence log (§19).

## 1. Purpose, scope, and how to read this document

**Goals.** Exercise the firmware on real hardware across four axes:

- **Functionality** — features behave as specified (`version 1.4.md`, manual plan).
- **Quality/robustness** — malformed input, races, resets, power loss, RF loss (§16).
- **Resource cost ("náročnost")** — power draw, flash/RAM budget, payload efficiency, timing (§15, §6).
- **Improvement discovery** — every run should end with a ranked list of observations that
  are *not* failures but are opportunities (§20).

**Scenario format.** Every scenario has an ID (`AT-<CAT>-<n>`) and five fields:

- **Pre** — preconditions (FW variant, config state, HW present).
- **Steps** — exact commands/tool calls.
- **Expect** — pass criteria (observable, not inferred).
- **Evidence** — what to capture into the run log.
- **Cleanup** — how to return the device to the baseline state.

**Automation levels:** `A` = fully automatic; `SA` = semi-automatic (needs a physical
operator action, see §18); `M` = manual observation only (rare — LED checks on release FW).

**FW applicability:** `D` = debug build only, `R` = release build only, `DR` = both.

**Baseline state** (the state every scenario should start from and Cleanup should restore):
device provisioned (serial + secret key + LoRaWAN keys set), `radio-mode lorawan`, activation
per current run plan, `interval-report 900` or the run's chosen value, `history-enable false`,
all 16 alarm slots empty, `alarm-limit 10` (factory default, #346), clock synced if a network is available.

## 2. Bench architecture

```
                +--------------------+          +---------------------+
                |  Linux host (agent)|          |  LoRa gateway       |
                |  west / rttt MCP   |          |  {GW_ID}            |
                |  JLinkExe / pytest |          +----------+----------+
                |  chirpstack-api    |                     | RF (EU868)
                |  mcp__tts__* tools |          +----------+----------+
                |  adb + curl        |          | ChirpStack {CS_HOST}|
                +---+----+----+------+          | TTN {TTS_HOST}      |
                    |    |    |                 +---------------------+
        USB (SWD)   |    |    | USB (adb)
    +---------------+    |    +----------------+
    |                    | USB                 |
+---+-----------+  +-----+------+   +----------+-----------+
| J-Link        |  | PPK2       |   | Android phone        |
| {JLINK_SN}    |  | source     |   | Manager-App (debug)  |
+---+-----------+  | {PPK2_MV}  |   | DebugControlServer   |
    | SWD           +-----+-----+   | 127.0.0.1:8429       |
    v                     | VOUT    +----------+-----------+
+---+---------------------+---+                | NFC (held on tag)
|  STICKER {STICKER_SN}       |<---------------+
|  STM32WLE5CC + ST25DV       |
|  sensors, LEDs, 1-Wire, PIR |
+-----------------------------+
```

Roles:

- **J-Link** — flash + RTT shell/log (debug FW) via rttt; register/fault inspection via JLinkExe.
- **PPK2** — programmable supply: powers the DUT, sweeps voltage (undervoltage tests), and
  measures current (power tests — *with SWD physically detached*, §15).
- **Phone** — real NFC RF path: Manager-App debug build scripted over `adb forward` + curl.
- **ChirpStack** (primary) / **TTN** (secondary) — join, uplink observation, downlink queueing.

**One probe, one owner.** If two agents share the bench, each owns exactly one J-Link SN and
one rttt MCP port; never touch the other agent's probe.

## 3. Toolchain reference

All host commands assume `source /home/hymbajs/.venv/bin/activate` (or the bench's
equivalent Python env with `west`, `rttt`, `chirpstack-api`, `ppk2-api`, `pytest` installed)
and the repo checked out at `{FW_REF}`.

### 3.1 Build

```bash
cd app
west build -p always -b sticker .                                 # release
west build -p always -b sticker . -- -DEXTRA_CONF_FILE=debug.conf # debug (RTT, shell, PM=n)
```

- `-p always` after any Kconfig/overlay change.
- Record the flash/RAM usage lines from the build report — they feed AT-HOST-05.
- Flash map (256 KB): code 208 KB @ 0x0 · history ring 32 KB @ 0x34000 · NVS/settings 16 KB
  @ 0x3C000 (history shrank 64→32 KB / code grew 176→208 KB via #302/PR#306; `CONFIG_FLASH_LOAD_SIZE=0x34000`
  release, `0x3C000` debug). NVS holds identity + config and is **shared between debug and release** —
  reflashing code does not lose provisioning.
- Version stamping (CI parity): add `-DAPP_FW_VERSION=...` defines only if the scenario
  needs a specific version string; otherwise the default build is fine.

### 3.2 Flash (requires `{FLASH_OK}` consent per image — §4)

Preferred, most reliable in-session method — **JLinkExe loadfile** (sector-erase only,
preserves NVS):

```bash
cat > /tmp/flash.jlink <<'EOF'
si SWD
speed 4000
device STM32WLE5CC
connect
halt
loadfile app/build/zephyr/zephyr.hex
r
g
q
EOF
JLinkExe -USB {JLINK_SN} -nogui 1 -CommanderScript /tmp/flash.jlink
```

- **Never use the bare `erase` command** — it wipes the whole flash including NVS
  (serial/keys/DevAddr/claim_token → factory-blank device). If a full wipe is genuinely
  required, that is a separate explicit consent item.
- `west flash --dev-id {JLINK_SN}` works but is known to hang on the post-flash reset on
  this board (the image still lands). The rttt MCP `flash` tool is convenient (stays
  attached, captures fresh boot) but has produced one non-booting image historically —
  if the device misbehaves right after an rttt flash, re-flash via JLinkExe before
  debugging the firmware.
- Stop any rttt session before JLinkExe/west touches the probe (J-Link is exclusive);
  wait ~3–5 s after killing rttt for USB to free.
- **Release (PM) builds sleep in Stop2 → SWD unreachable.** Flash retry loop (1 s interval,
  up to ~3 min) + ask the operator to power-cycle; the flash catches the boot window.

### 3.3 RTT shell & log — rttt MCP (debug FW only)

The repo ships `.rttt.yaml` (device STM32WLE5CC, speed 1000, mcp on). Start headless:

```bash
cd <repo-root>
nohup script -qec "rttt --serial {JLINK_SN} --address {RTT_ADDR} --mcp-listen 127.0.0.1:{MCP_PORT} --trust-shells" /dev/null >/tmp/rttt-mcp.out 2>&1 &
sleep 8 && ss -tln | grep {MCP_PORT}    # verify it is listening
```

- The `script -qec … /dev/null` pseudo-TTY wrapper is mandatory headless (rttt aborts with
  "Input is not a terminal" otherwise). **Never pass `--reset`** (broken; device resets are
  done via JLinkExe `r;g` or the `reboot` command).
- MCP tools (JSON-RPC over `POST http://127.0.0.1:{MCP_PORT}/mcp`, or via the session's MCP
  client): `send_command(command, timeout)`, `read_terminal(lines)`, `read_log(lines,
  after_cursor, pattern)`, `status()`, `flash(file_path)`.
- `send_command` default timeout is 2 s — **slow commands return empty output while still
  executing**. `w1 scan`, `ats sensors sample` (DS18B20 ≈ 750 ms/probe) need
  `timeout: 6–8`; alternatively read the result afterwards with `read_terminal
  {"lines": 300}` and parse from the last echo of your command.
- Shell output (incl. `E:` errors) lands on the **terminal** channel (0); `<inf>` logging on
  the log channel (1). Multi-line hexdumps: header line + hex on the next line.
- A `send_command` timeout is **not** a crash. Before declaring a wedge, inspect the CPU via
  JLinkExe: `halt`, read IPSR/CFSR(0xE000ED28)/HFSR(0xE000ED2C).
- Fallback without rttt: JLinkExe commander script with `exec SetRTTAddr {RTT_ADDR}` +
  RTT telnet on 19021 (send via FIFO; `nc` closes stdin too early otherwise).

### 3.4 ChirpStack — primary LNS

gRPC only (no REST in v4): `pip install chirpstack-api`, then

```python
import grpc
from chirpstack_api import api
ch = grpc.insecure_channel("{CS_HOST}")
md = [("authorization", "Bearer {CS_TOKEN}")]
dev = api.DeviceServiceStub(ch)

# Observe activity: frame counter must rise with each uplink
act = dev.GetActivation(api.GetDeviceActivationRequest(dev_eui="{CS_OTAA_DEVEUI}"), metadata=md)

# Queue a downlink (fPort 85 command, confirmed=False)
dev.Enqueue(api.EnqueueDeviceQueueItemRequest(queue_item=api.DeviceQueueItem(
    dev_eui="{CS_OTAA_DEVEUI}", f_port=85, data=bytes.fromhex("08032200"))), metadata=md)
```

- Downlinks are delivered in the RX window after the device's **next uplink** — to make a
  test fast, trigger an uplink (`send` from the shell, or a physical event) after queueing.
- ChirpStack answers `LinkCheckReq` reliably (needed for the LC state-machine scenarios).
- Uplink payload events: use the application event stream/API of the bench instance, or
  poll `GetActivation` f_cnt_up for liveness plus read decoded events from the CS UI/API.
- ABP device must have `skip_fcnt_check=True` (FW resets FCnt on reboot).
- **OTAA re-join gotcha:** re-pointing the bench to a *previously-used* CS OTAA device fails with
  `ERROR OTAA DevNonce has already been used` (CS remembers past DevNonces; the FW sends monotonic
  persisted DevNonces — spec-correct, not a FW bug). Fix: `dev.FlushDevNonces(api.FlushDevNoncesRequest(
  dev_eui=...))`, then reboot → immediate join. (OTAA analog of `skip_fcnt_check`.)
- On OTAA rejoin the DevAddr changes — re-read activation, never cache it across joins.

### 3.5 TTN / The Things Stack — secondary LNS

Use the session's TTS MCP tools where present: `mcp__tts__get_uplinks` (decoded payloads
incl. `decoded_payload` from `ttn.js`), `mcp__tts__send_downlink` (`f_port`, hex payload),
`mcp__tts__get_device`. Application `{TTS_APP_ID}` on `{TTS_HOST}`.

- Purpose here: **decoder parity** (same uplink must decode identically on both LNS — both
  run `app/decoder/ttn.js`) and a second-opinion join path.
- **Correction (2026-08-11), supersedes the old "TTN doesn't answer LinkCheckReq" note**: after
  the `hm-sticker-otaa-test` device recreate on 2026-07-07, TTN answers `LinkCheckReq`
  correctly and reliably — confirmed decisive on 2026-08-11: `ats lrw check` got an immediate
  `LinkCheckAns` (margin 26 dB, 2 gateways), and the session then held **HEALTHY on the same
  DevAddr for 438 s / 9 uplinks with zero link-check failures** (`healthy->warning: 0/3`
  throughout). **On the same bench, ChirpStack (`hm-sticker-otaa-cs`) showed the opposite**: a
  freshly release-flashed device with stock `lrw-link-check-interval`/`lrw-link-check-fail-rejoin`
  defaults (both `5`, per `app_config.yml`) rejoined OTAA (fresh DevAddr each time) roughly every
  5-16 minutes, with device uptime climbing continuously across rejoins (confirmed via
  `unix_time - uptime_s` staying self-consistent — this is LoRaWAN-layer RECONNECT churn, not
  an MCU reboot loop). **Net effect: TTN is currently the more reliable LNS on this bench for LC
  state-machine work (AT-LRW-07/AT-LRW-08); ChirpStack is the one that needs the caveat now.**
  Not root-caused further (gateway duty-cycle vs genuine ChirpStack LinkCheckAns handling both
  remain open) — flag if picking this up.

### 3.6 Manager-App debug control server (real-NFC path)

The phone app (GitLab `apps/manager`) in a **debug build** starts a loopback HTTP control
server on port 8429 (`lib/debug/control_server.dart`) precisely so an agent can drive real
NFC exchanges without touching the screen:

```bash
adb -s {PHONE_ADB_SERIAL} forward tcp:8429 tcp:8429
curl -s localhost:8429/ping                    # {"ok":true,"debug":true}
# Configure comms once per run (encrypted, key from the app's secret store or explicit):
curl -s -XPOST localhost:8429/comms -d '{"mode":"encrypted","source":"manual","serial":"{STICKER_SN}","key":"{STICKER_KEY}"}'
# Real NFC ops (phone must be physically held on the tag — operator assist, §18):
curl -s localhost:8429/info                    # plaintext hio.stck:inf read
curl -s -XPOST localhost:8429/command -d '{"op":"getinfo"}'
curl -s -XPOST localhost:8429/command -d '{"op":"setparam","params":{"app.interval-report":300}}'
curl -s -XPOST localhost:8429/command -d '{"op":"getparam","names":["app.interval-report"]}'
curl -s -XPOST localhost:8429/command -d '{"op":"clocksync"}'
curl -s -XPOST localhost:8429/command -d '{"op":"w1scan"}'
curl -s -XPOST localhost:8429/command -d '{"op":"reboot"}'
curl -s localhost:8429/config                  # full paged GetConfig walk (up to 32 pages) + timing
curl -s -XPOST localhost:8429/nav -d '{"route":"/sticker/nfc/config"}'   # drive UI screens
```

- Every command op claims the NFC adapter, reads `hio.stck:inf`, seeds the AES-CCM codec
  (serial + nonce high-water from the tag, key from store/manual), writes `hio.stck:cmd`,
  reads `hio.stck:rsp`.
- **The phone must be on the tag for the whole op** — pair each call with an §18 assist
  block, or ask the operator to fixture the phone on the device for a batch.
- `.sfu` firmware update and ATELOS claim are NOT implemented in the app yet — no scenarios
  for them (see also README: no in-field FW update, by design).

**Phone-free alternative (debug FW, no phone needed).** Everything the phone does to build an
`hio.stck:cmd` exchange can be hand-crafted and injected via the debug shell's `nfc write
<offset> <hex>` — useful for AT-NFC-05/06 (nonce/anti-replay) and the AT-NFC-03-style
device_reset/factory_reset/set_secret_key legs of N9, where the point of the test is the
*protocol/crypto* behavior, not the phone's own UI/codec:

1. Build the plaintext `Command` protobuf bytes (see `app_config.proto`'s `Command` oneof for
   field numbers).
2. AES-CCM-encrypt exactly as `app_nfc.c`'s `decrypt()`/`encrypt()` do: 9-byte nonce =
   `serial(4B BE) || nonce_counter(4B BE) || direction(1B)` (`0x00`=request, `0x01`=response),
   AAD = the 8-byte `serial||nonce_counter` header, 16-byte CCM tag, wire =
   `header || ciphertext+tag`.
3. Wrap in an NDEF record (TNF=`EXTERNAL` (`0x04`), type `hio.stck:cmd`, 12 bytes) inside a TLV
   (`0x03`, length, record bytes, `0xFE` terminator), and `nfc write 4 <hex>` it onto the tag
   (offset 4, right after the 4-byte Capability Container).
4. `nfc check` to trigger processing; read the response back with `nfc dump` and decrypt it the
   same way with direction `0x01`.

```python
# AESCCM(key, tag_length=16).encrypt(nonce, plaintext, aad) — cryptography package
header = struct.pack(">II", serial_number, nonce_counter)
nonce = header + bytes([0x00])          # 0x01 for decrypting a response
wire = header + AESCCM(key, 16).encrypt(nonce, plaintext, aad=header)
```

Gotchas specific to this method: `nfc write` silently **truncates** hex args over ~128
characters (command-line buffer limit) with no error — split into ≤40-byte chunks at
sequential offsets and verify each `wrote N byte(s)` count. Protobuf tag bytes for `Command`
oneof fields **≥ 16** (e.g. `factory_reset`=23, `set_secret_key`=24) exceed 127 and need
2-byte varint encoding, not a single raw byte — encoding the tag as one byte silently produces
a garbled frame the firmware correctly rejects as `Response.Error{BAD_REQUEST,
"end-of-stream"}`, which looks exactly like a firmware bug but is a bug in the hand-rolled
encoder (see §22).

This method **cannot** distinguish the true `hio.stck:ack`-driven release path from the
field-loss backstop (`m_seen_ack` vs `m_info_restore_pending` in `app_nfc.c`) — there is no
real RF field to hold open between writes, so the backstop always wins the race. Both paths
correctly release a pending deferred action (reboot/save), so this is fine for confirming *that*
device_reset/factory_reset/set_secret_key execute correctly and are gated behind the response
write — but confirming the specific *ack* code path (not just the backstop) still needs a real
phone tap (§3.6, N9).

### 3.7 PPK2 — supply control & current measurement

Proven procedure (source-meter mode):

- **Hold the handle open.** DUT power stays on only while a process holds the PPK2 serial
  handle. Run a persistent holder daemon (`setsid nohup python3 ppk_hold.py {PPK2_MV}
  </dev/null >/tmp/ppk_hold.out 2>&1 &`) that loops on a command file: `echo "V 2300" >
  /tmp/ppk_cmd` retunes voltage live without dropping VTref.
- Auto-detect the port: scan `/dev/ttyACM*` for the Nordic `1915:c00a` device where
  `PPK2_API(p).get_modifiers()` returns truthy — the index changes across re-enumerations.
- Voltage-only control never calls `start_measuring()` (avoids the stream-desync bug). Call
  `start_measuring()` only in dedicated measurement scenarios and expect to raw-reset
  (write `0x0d` then `0x20` to the port, wait ~4 s, relaunch holder) afterwards.
- **Current measurements require the J-Link physically detached** (probe adds 60–100 µA and
  DBGMCU keeps the debug domain awake). After any SWD session, power-cycle the DUT before
  measuring.
- Sanity cross-check: PPK2 set value vs J-Link VTref vs `ats sensors sample` voltage line
  (internal divider reads ~+30–60 mV high).

## 4. Safety rules (hard constraints — never override)

1. **Flashing requires explicit operator consent, per image.** Building, reading RTT,
   sending shell commands, LNS downlinks: fine. `loadfile`/`west flash`: ask first.
2. **Always pin the J-Link serial** (`-USB {JLINK_SN}` / `--serial` / `--dev-id`). Multiple
   probes are routinely connected; an unpinned call can erase the wrong target.
3. **Never `pkill -f` with a pattern that appears in your own command line** (jlink, rttt,
   script…) — it kills your own shell. Kill by PID, `pkill -9 -x JLinkExe`, or the
   `pkill -f 'rtt[t]'` bracket trick. Never `kill -9` a J-Link process holding USB if
   avoidable (corrupts USB state).
4. **Preserve NVS.** No `erase` in J-Link scripts, no `west flash --erase`, no
   `settings erase` outside the scenarios that explicitly test it (and those re-provision
   afterwards from the bench profile).
5. `settings save` **auto-reboots** the device. `config` changes are staged until saved;
   capability-gated peripherals (1-Wire bridge, accelerometer) initialise **only at boot**.
6. The NFC `nonce_counter` is write-forward (anti-replay). Never try to rewind it; after
   crypto tests, read the tag's `inf` record to learn the current high-water mark.
7. One J-Link owner at a time: stop rttt before JLinkExe/west, restart after.
8. Findings are reported in the run report (§19). **Never create GitHub issues** or push
   code/PRs from a test run without being asked.

## 5. Firmware variant matrix — what is testable where

| Capability | Debug FW | Release FW |
|---|---|---|
| RTT shell (`ats`, `config`, `history`, `alarm`, `w1`, `clock`, `join`, `send`, `power`) | ✅ | ❌ (no shell, no RTT) |
| RTT log | ✅ | ❌ |
| LNS observation (join, uplinks, fPort-85 responses) | ✅ | ✅ |
| LoRaWAN downlink commands (fPort 85) | ✅ | ✅ — **the primary release-FW control channel** |
| NFC via Manager-App control server | ✅ | ✅ — **the primary release-FW query channel** |
| LED observation | ✅ (redundant with log) | ✅ (operator/camera — often the only local signal) |
| PPK2 current signature (boot, join burst, TX period) | ✅ (but PM=n distorts) | ✅ — **release-FW liveness proof without RTT** |
| Power management / sleep behaviour | ⚠ PM disabled in debug — not representative | ✅ |
| SWD attach | ✅ anytime | ⚠ only in boot/awake window (Stop2 blocks it) |
| **History persistence across reboot/power-loss** | ❌ **`CONFIG_APP_HISTORY_FLASH=n`** in debug.conf — history lives in RAM only and is always lost on any reboot (this is intentional: debug trades the 32 KB history-flash budget for extra code space, `boards/sticker/sticker.dts` code_partition comment). `history-enable`/accumulation/read/stats all work fine *within* a boot, but AT-HIS-03/H8 (flash persistence) is **not testable on debug** — confirmed by direct test (records → 0 after a clean `ats device reboot`, raw flash at 0x34000 shows plain firmware rodata, not a history ring) | ✅ `CONFIG_APP_HISTORY_FLASH=y` — dedicated 32 KB page-ring partition @0x34000, survives reboot/power-loss by design (#265, shrunk #302) |

**Why release testing is non-negotiable:** two historical release-only bugs — the TX-stop
wedge (masked by `CONFIG_LOG` timing and PM=n) and the boot ADC IWDG reset loop — were
invisible on debug builds. Rules:

- Every scenario marked `DR` runs on both variants when time allows; at minimum, the
  release smoke set (§21, "release-smoke" column) runs on every release candidate:
  sustained-TX (AT-LRW-10), boot-loop check (AT-BOOT-04), downlink control (AT-LRW-06),
  NFC getinfo (AT-NFC-02), TX-period power signature (AT-PWR-04).
- On release FW, replace "read RTT" observations with: LNS uplink stream, NFC `inf`/`rsp`
  records, LED (operator), and PPK2 current signature.

---

## Part II — Test categories & scenarios

## 6. Static & host tests (AT-HOST) — no hardware needed

Run these first in every session; they gate everything else. All `A`/host-only.

### AT-HOST-01 — CI superset (direct commands)
- **Pre:** venv active, repo at `{FW_REF}`.
- **Steps:** run each CI step directly — `cd app/decoder && node --test`;
  `pytest scripts/west_commands/tests` (includes the configen-vs-committed sync check,
  `test_generated_c_matches_committed`); `git ls-files 'app/src/*.c' 'app/src/*.h' | xargs
  clang-format --dry-run --Werror`; plus `west build` (release and debug) if not already
  covered by the `build` job for this ref.
- **Expect:** every step exits 0 — decoder tests, configen pytest (incl. sync check),
  clang-format, and both firmware builds all green.
- **Evidence:** last 30 lines of output per step; on failure the full failing step output.
- **Cleanup:** `git status` must stay clean.

### AT-HOST-02 — native_sim ztest suites
- **Steps:** `bash tests/run_native.sh` (iterates tests/alarm_eval, alarm_rules, buzzer, ccm,
  cmd, compose, history, history_flash, ndef, nfc_crypto, nfc_hw on `native_sim/native/64`).
  `alarm_eval` (#348) drives the real `app_alarm.c` dwell/confirm/hold state machine directly
  (`app_alarm_event()`/`app_alarm_poll()`) with hall/sensor GPIO stubbed, plus (#397) the
  alarm-driven buzzer plumbing (`app_buzzer_play_repeating()` stubbed there) — `alarm_rules`
  only covers the static rule-validation layer. `buzzer` (#397) drives the real
  `app_buzzer.c` melody engine against a `gpio_emul`-backed fake GPIO: melody sequencing,
  abort ordering, queue-replace policy, and `buzzer_play` id bounds.
- **Expect:** every suite prints `PROJECT EXECUTION SUCCESSFUL`.
- **Evidence:** per-suite pass/fail table.

### AT-HOST-03 — decoder tests + schema/decoder agreement
- **Steps:** `cd app/decoder && node --test`; then
  `pytest scripts/west_commands/tests -k proto_and_decoder_agree`.
- **Expect:** all decoder cases pass; the firmware proto schema and `ttn.js` agree on the wire.
- **Known gap (2026-08-11, not yet fixed):** `encodeDownlinkCommand` silently builds an
  **empty body** for two `Command` oneof members that actually carry fields in the firmware
  proto: `clock_sync` (drops `unix_time`) and `req_history_page` (drops
  `from_unix`/`to_unix`/`start_ord`). Both are reachable today only by hand-building the
  Command bytes (§3.6 phone-free recipe, or a raw hex downlink) — round-trip via the LNS
  downlink builder silently produces a no-op command instead of erroring. Add an assertion to
  this suite that every `Command` field with a non-trivial message body either has an
  `_enc*`/`encodeDownlinkCommand` branch or is explicitly documented as encoder-excluded (like
  `factory_reset`/`set_secret_key`/`vendor_reset` already are, per the comment in the source) —
  this would have caught `req_history_page` immediately.

### AT-HOST-04 — configen round-trip
- **Steps:** `pytest scripts/west_commands/tests` (full suite: idempotence, append-only
  proto_id allocator, transport validation, dispatch completeness, migration preserves
  factory fields).
- **Expect:** 100% pass.

### AT-HOST-05 — flash/RAM budget regression
- **Pre:** AT-HOST-01 built both variants.
- **Steps:** parse `Memory region … Used Size` from both build logs; compare against the
  recorded baseline (bench annex keeps last-known-good percentages).
- **Expect:** release FLASH ≤ baseline + 0.5 pp; debug FLASH < 100% with ≥ 1 KB headroom;
  RAM within +0.5 pp. Any growth ≥ 1 pp → IMPROVEMENT finding with the diff of
  `rom_report` if obtainable.
- **Evidence:** table variant × region × used/size/%.

### AT-HOST-06 — decoder fuzz (host-side adversarial warm-up)
- **Steps:** run `decodeUplink`/`decodeDownlink` from `app/decoder/ttn.js` (node one-liners)
  over: empty payload, 1-byte payloads 0x00–0xFF on fPorts 1/2/3/85, truncations of each
  known-good vector from `manual-test-plan.md` §"Ready-to-use hex downlinks", and 100
  random byte strings per fPort.
- **Expect:** never throws; returns `errors`/partial decode gracefully.
- **Evidence:** any thrown exception with its input hex (each = HIGH finding: LNS-side
  decoder crash).

## 7. Boot & identity (AT-BOOT)

### AT-BOOT-01 — boot banner & version (D; maps G1)
- **Steps:** JLinkExe `r;g`, then rttt `read_terminal`; or `ats device info`.
- **Expect:** `Firmware version: M.m.p (debug|release)` matches the built ref; serial =
  `{STICKER_SN}`; uptime reset; reset cause decoded (e.g. `PIN`, `SOFTWARE`).
- **Evidence:** the info block verbatim.

### AT-BOOT-02 — boot LED carousel (DR; SA on release; maps G2)
- **Steps:** reboot; debug: confirm carousel timing vs log; release: §18 assist "watch the
  LEDs after I reset the device".
- **Expect:** R→Y→G carousel ~5 s after boot, then idle. See `version 1.4.md` §16 for the
  full LED reference.

### AT-BOOT-03 — identity preserved across factory reset & reflash (D; maps G6, G6c)
- **Pre:** `{FLASH_OK}`; provisioning values recorded first (`config show`).
- **Steps:** (a) shell `settings reset` (or a FactoryReset command encoded with
  `encodeDownlink` and injected via `ats cmd lrw`); (b) reflash the same image (§3.2);
  after each, `config show`.
- **Expect:** serial, secret key, claim token, LoRaWAN identity keys survive both;
  application params reset to defaults after factory reset (interval-report back to 900).
- **Result (2026-08-11, real debug→release reflash, not just factory-reset)**: built a genuine
  `prj.conf`-only release image (`.config` confirmed `CONFIG_SHELL`/`CONFIG_USE_SEGGER_RTT` both
  unset — release truly has zero RTT/shell surface, not just no shell commands), flashed it over
  a provisioned debug device via `JLinkExe loadfile` (sector-erase, no `--erase`). After a real
  power cycle (see AT-BOOT-04's PPK2 correction below), the autonomous post-join GetInfo uplink
  decoded to `debug: false`, `serial_number: 2162199999` (unchanged) and a successful OTAA join
  on the preserved `lrw_deveui`/`lrw_appkey` — full identity survived a real firmware-variant
  swap, not just a same-image reflash. Since release has no shell/RTT, post-release verification
  had to go through LRW (autonomous GetInfo) — there is currently no way to inspect NFC state on
  a release device without a phone or external reader.
- **Cleanup:** restore run-plan config values, `settings save`.

### AT-BOOT-04 — release boot sanity / no reset loop (R; maps regression a42dde6)
- **Pre:** release FW just flashed; PPK2 powering the DUT.
- **Steps:** power-cycle via PPK2 — **use `QUIT` then relaunch the holder, or a live `OFF` then
  `ON` command (both call `toggle_DUT_power` directly); do NOT rely on setting the source
  voltage to 0 and back** (confirmed 2026-08-11: `set_source_voltage(0)` alone does not power
  off the DUT — LED kept blinking and the device kept transmitting on LoRaWAN for 90+ s at
  commanded 0 mV; only an explicit `toggle_DUT_power("OFF")` is a real cut). Watch the PPK2
  current trace ~60 s (or, without measurement mode, watch the LNS for the boot uplink and ask
  the operator to confirm a single LED carousel).
- **Expect:** exactly one boot signature (single carousel, single join/uplink burst) — no
  periodic ~8 s IWDG reset pattern (the historical release-only boot-ADC hang).
- **Evidence:** current trace segment or LNS first-uplink timestamp.

### AT-BOOT-05 — watchdog recovery (D; maps G8)
- **Steps:** if a debug hook to stall the report loop exists in the build, use it;
  otherwise verify IWDG is armed: `ats device info` reset-cause after AT-BOOT-04
  power-cycle history, plus `version 1.4.md` IWDG notes.
- **Expect:** a stalled main loop reboots within the watchdog window and reset cause shows
  watchdog.

### AT-BOOT-06 — config schema migration (D)
- **Pre:** `{FLASH_OK}`; an older-config-version image available on the bench.
- **Steps:** flash older image → set a distinctive app param → flash current image → `config show`.
- **Expect:** boot log reports migration; factory/identity fields carried over; app params
  at new defaults; no boot loop.
- **Cleanup:** restore run-plan config.

### AT-BOOT-07 — i2c1 survives real Stop2 (R; maps #329/#330)
- **Pre:** `{FLASH_OK}`; release image, `cap-w1-sensors true` with at least one 1-Wire
  device on the bus; debug-in-stop **off** (the shipped default — do not add
  `STM32_ENABLE_DEBUG_SLEEP_STOP`, it masks this regression); PPK2 powering the DUT so the
  device actually reaches Stop2 (a debug build with `CONFIG_PM=n` never sleeps and cannot
  exercise this path).
- **Steps:** power-cycle; wait past at least one real Stop2 idle period (tens of seconds,
  clear of boot/join activity); then in one pass: `ats sensors sample` (on-board SHT4x),
  `w1 scan` (DS2484/1-Wire), and a real NFC tap (§18 assist or manager-app debug control)
  reading `/info`.
- **Expect:** all three succeed on the first attempt — no `-EIO`/`-5` on the SHT4x read or
  `w1_reset_bus`, tag read succeeds first try. A regression that reinstates a permanent
  i2c1 `pm_device_runtime_get()` (e.g. hoisting the #330 hold back to whole-uptime scope in
  `app_sensor_init`) wedges all three simultaneously after the first Stop2.
- **Evidence:** sample output + w1 scan output + NFC read result from the same pass.

## 8. Config & remote control (AT-CFG)

The config tree has ~60 parameters in 5 groups (`app/src/app_config.yml` is the single
source of truth — read it before testing so parameter names/ranges are current).

### AT-CFG-01 — shell round-trip on a representative sample (D, A; maps C8)
- **Steps:** for each of: `interval-report` (int, 60–86400), `interval-sample` (5–3600 or 0),
  `battery-level` (1000–3600), `history-enable` (bool), `accel-motion-sensitivity` (enum),
  `lrw-adr` (bool), `lrw-link-check-interval` (0–255): set a non-default valid value →
  read back → `settings save` (reboots) → read back again.
- **Expect:** staged value visible before save; persisted after reboot.
- **Cleanup:** restore defaults, save.

### AT-CFG-02 — range clamping & rejection (D, A; maps C5)
- **Steps:** for each sampled int param, try min−1, max+1, absurd (0xFFFFFFFF), and for
  enums an undefined value — via shell AND via `ats cmd lrw <set_param hex>` (build the
  protobuf with the encodeDownlink helper in `app/decoder/ttn.js` or hand-crafted vectors).
- **Expect:** shell prints a range error; SetParam over transport returns an Error/NACK
  response, value unchanged. Record whether behaviour is clamp vs reject — inconsistency
  between transports = MED finding.

### AT-CFG-03 — transport access model (D, A; maps C1, C10, H-3)
- **Steps:** attempt over `ats cmd lrw`: (a) SetParam on a `lorawan`-group field (e.g.
  radio_mode, lrw_appkey); (b) GetParam of `lrw_appkey`. Then the same over `ats cmd nfc`.
- **Expect:** LRW transport: both refused (lorawan group is shell+nfc writable only; keys
  NFC-readable only). NFC transport: allowed. Any key readable over LRW = **CRIT** finding.

### AT-CFG-04 — staged config + commit semantics (D, A; maps C2)
- **Steps:** SetParam without SettingsSave → reboot (`ats device reboot`) → read back.
  Then SetParam + SettingsSave (`08043200`).
- **Expect:** unstaged change lost on reboot; saved change persists; save triggers exactly
  one reboot.

### AT-CFG-05 — GetConfig pagination (DR, A; maps C4)
- **Steps:** debug: `ats cmd lrw <get_config hex>` and walk pages; release/NFC: control
  server `GET /config` (paged walk with timing).
- **Expect:** every configured non-default value present exactly once; page count sane;
  NFC walk completes < 32 pages.
- **Evidence:** page count + total walk duration (feeds §20 efficiency observations).

### AT-CFG-06 — claim token write-once (D, A; maps #170)
- **Steps:** read `config claim-token`; if zero, write one, save, try to overwrite with a
  different value.
- **Expect:** second write refused; token survives factory reset.
- **Cleanup:** none (write-once by design — use a bench-designated token).

### AT-CFG-07 — counter persistence across power loss (D+PPK2, A; maps C9, #49)
- **Steps:** generate hall/input counts (assist §18 if no debug injector) → confirm counts
  via `ats sensors sample` → cut power via PPK2 (no clean shutdown) → repower → read counts.
- **Expect:** counters restored (persisted per #49/#157), no double-count, no reset to 0.

## 9. LoRaWAN (AT-LRW) — ChirpStack primary

Use the ready-made fPort-85 hex vectors from `manual-test-plan.md` ("Ready-to-use hex
downlinks") — `get_info 08032200`, `settings_save 08043200`, `clock_sync 08056200`,
`force_send 08064a00`, `sample 0805aa0100`, `reboot 08083a00`,
`reset_counters 0807520408011801`, `lrw_reset 0801820100`, `lrw_join 08018a0100`, and the
documented `set_param` example. The leading byte is `seq`, echoed in the response.

### AT-LRW-01 — OTAA join on ChirpStack (DR, A; maps L2)
- **Pre:** provision OTAA creds (§ annex; shell: `config lrw-activation otaa`,
  `config lrw-deveui/joineui/appkey …`, `settings save`). MAC 1.0.3: appkey serves as NwkKey.
- **Steps:** reboot; poll `GetActivation` until DevAddr appears (≤ 2 min).
- **Expect:** join accept; first uplink = autonomous GetInfo on fPort 85 (AT-LRW-04).
- **Evidence:** DevAddr, join timestamp, RSSI/SNR of first uplink.

### AT-LRW-02 — ABP session (DR, A; maps L3)
- **Pre:** ABP creds provisioned; CS device has `skip_fcnt_check=True`.
- **Steps:** reboot; observe f_cnt_up rising from 0.
- **Expect:** telemetry flows without join; counters restart per reboot semantics.

### AT-LRW-03 — TTN join + decoder parity (DR, A; maps L1, F1)
- **Steps:** reprovision to TTN creds, save; after join capture one fPort-2 uplink decoded
  by TTN (`mcp__tts__get_uplinks`), then reprovision back to ChirpStack and capture the
  matching uplink type there.
- **Expect:** identical decoded JSON structure/values (modulo timestamps/metadata) — both
  run `ttn.js`. Any divergence = HIGH finding (decoder or LNS integration).

### AT-LRW-04 — GetInfo-on-join + device_status (DR, A; maps L4, G4)
- **Steps:** force a rejoin (`lrw_join 08018a0100` via downlink or `ats cmd lrw`); capture
  the fPort-85 info frame; decode.
- **Expect:** serial, fw version, config version, battery mV, lrw_state, device_status
  bitmask present and plausible (e.g. low-battery bit clear at {PPK2_MV}=3000).

### AT-LRW-05 — periodic + multi-frame telemetry (DR, A; maps L5, L6)
- **Steps:** set `interval-report 60`, save; enable enough sensors that the fPort-2 payload
  exceeds one frame at the current DR (or use `ats lrw compose <budget>` on debug to
  verify the split logic directly with a tiny budget).
- **Expect:** uplinks every ~60 s (+TX jitter 0..min(interval/10,10 s) — jitter delays TX
  only, never the sampling timestamps); multi-frame sequences reassemble in the decoder.
- **Evidence:** 5 consecutive uplink timestamps + decoded payloads.
- **Cleanup:** restore interval.

### AT-LRW-06 — downlink command matrix (DR, A; maps L10, C1–C7, G4b, G5)
- **Steps:** over ChirpStack enqueue, one at a time, each command: get_info, sample,
  set_param(+readback), get_param, get_config, settings_save, reboot, reset_counters,
  force_send, clock_sync, lrw_reset, lrw_join, enter_calibration, w1_scan, **device_reset**
  (unlike `factory_reset`/`set_secret_key`, `device_reset`'s dispatch in `app_cmd.c` has no
  transport gate — it IS reachable over a LoRaWAN downlink, and hasn't been tried that way);
  after each, trigger an uplink and capture the fPort-85 response.
- Also deliberately try **`factory_reset`**, **`set_secret_key`**, and **`req_history_page`**
  over this same LRW path — `app_cmd.c` gates all three to `[nfc, shell]`/`[nfc, shell,
  vendor]`/`[nfc]` respectively, so the expected result is
  `Response.Error{NOT_READY,"transport not allowed"}`, not silence or a crash. Confirms the
  transport allowlist from the read-through, not just from source.
- **Expect:** every command acks/responds with the echoed seq; exactly **one command
  consumed per RX window** (queue two → second arrives one uplink later); refused
  transports (per AT-CFG-03) return errors, not silence. `device_reset` over LRW is expected
  to succeed and reboot — since LRW has no `hio.stck:ack` concept, watch whether the deferred
  reboot fires immediately after the response uplink is handed to the MAC layer or waits for
  something else; this differs from the NFC ack-gate path (§3.6 phone-free recipe).
- **Result (2026-08-11, real ChirpStack downlinks, sticker 2162199999):** all four confirmed.
  `factory_reset`/`set_secret_key`/`req_history_page` each got back
  `Response.Error{NOT_READY,"transport not allowed"}` with the seq echoed correctly, no state
  change. `device_reset` succeeded: the `Response{seq,ack:{}}` uplink landed, then the device
  rebooted and rejoined OTAA cleanly, with its autonomous post-join GetInfo uplink arriving
  ~35s after the ack — i.e. LRW also does response-before-reboot ordering, just via ordinary
  uplink-then-deferred-action sequencing rather than NFC's ack/backstop gate. Config diff
  matched `app_config_device_reset()`'s preserve-list exactly (`cap-w1-sensors`/
  `interval-report` reset, `lrw-appkey`/secret_key/serial/nonce_counter preserved).
- **Evidence:** command → response-hex → decoded table. This is the core release-FW
  functional suite.

### AT-LRW-07 — link-check state machine (D, A; maps L7, L8, L13)
- **Pre:** ChirpStack (answers LinkCheckReq); `lrw-link-check-interval 5`,
  `lrw-link-check-fail-rejoin 5` or run-plan values.
- **Steps:** `ats lrw check` (real LC); then drive the FSM synthetically: `ats lrw lc fail`
  × N → status via `ats lrw status` after each; then `ats lrw lc ok`.
- **Expect:** HEALTHY → WARNING (with 🟡2× LED per §16) → RECONNECT (rejoin with backoff)
  transitions at the configured thresholds; `ok` recovers to HEALTHY.

### AT-LRW-08 — late LC in RECONNECT does not wedge (D, A; maps L14, HIGH-1 regression)
- **Steps:** per manual L14: force RECONNECT, then inject a late `ats lrw lc ok`; continue
  sending.
- **Expect:** TX continues; no stuck semaphore (the historical overloaded-timer wedge).

### AT-LRW-09 — radio-mode off/lorawan/p2p (D, A; maps #271, supersedes L15)
- **Steps:** `config radio-mode off` + save → confirm radio-silent (no uplinks ≥ 3× report
  interval; sensors/history still run via `ats sensors sample`, `history count`). Then
  `radio-mode p2p` → boot into P2P transport (if bench has a P2P receiver; else just confirm
  no LoRaWAN traffic + no crash). Back to `lorawan`.
- **Expect:** mode changes only via shell/NFC (LRW SetParam refused — AT-CFG-03); each mode
  boots clean.
- **Cleanup:** `radio-mode lorawan`, save, confirm rejoin.

### AT-LRW-10 — release sustained TX (R, A; maps L16 — decisive TX-stop regression)
- **Pre:** release FW, `interval-report 60` (set over NFC or before flashing release).
- **Steps:** observe the LNS uplink stream for ≥ 60 min after a cold power-cycle.
- **Expect:** no gap > 2× interval + jitter; f_cnt strictly increasing; ≥ 55 uplinks/h.
- **Evidence:** uplink timestamp list + max-gap stat.

### AT-LRW-11 — DR/payload budget behaviour (D, A; maps L6)
- **Steps:** `ats lrw compose 51` / `ats lrw compose 242` (DR0 vs DR5-class budgets) with
  many sensors enabled.
- **Expect:** frames never exceed the budget; split points are clean protobuf boundaries;
  the 2-byte-varint capacity case (>127 B frames) composes correctly.

### AT-LRW-12 — alarm uplink priority & fPort-3 (DR, A; maps A10, L-series)
- **Steps:** arm one alarm rule (AT-ALM-01), trigger it while a fPort-85 response is also
  pending (queue a get_info first).
- **Expect:** drain order = fPort-85 response first, then fPort-3 alarm; alarm batch decodes
  (type, source, value, time_synced handling).

### US915 region tests (AT-LRW-13..15; maps #303)

US915 is compiled in (`CONFIG_LORAMAC_REGION_US915=y`) and selectable
(`config lrw-region us915`, `lrw-sub-band` 1–8 / 0 = all, default 2), but the bench RF is EU868
(§2). These scenarios need a **US915-capable gateway + LNS** (ChirpStack region `us915_0`, or TTN
US915). Per Step 0 (§0, gateway question): if no US915 gateway is in RF range, all three are
**SKIPPED (RF)** — record the skip, do not silently pass. The region-switch order is always
**set region → `lrw_reset` → `lrw_join`** (the reset clears the stale EU868 session / DevNonce /
frame counters via `clear_stale_lorawan_nvm`, else the join uses the wrong channel plan).

### AT-LRW-13 — US915 OTAA join on sub-band 2 (DR, A; maps #303, L2)
- **Pre:** US915 LNS + gateway online (`{GW_ID}` is US915); OTAA creds provisioned for the US915
  application. `config lrw-region us915`, `config lrw-sub-band 2`.
- **Steps:** `lrw_reset` → `lrw_join` (shell/`ats cmd lrw` or downlink); poll `GetActivation`
  until DevAddr appears (≤ 2 min).
- **Expect:** join accept using **only sub-band 2** (channels 8–15 + 500 kHz channel 65); first
  uplink = autonomous GetInfo on fPort 85. A join that never completes with a US915 gateway present
  = HIGH finding.
- **Evidence:** DevAddr, join channels seen LNS-side, RSSI/SNR of first uplink.

### AT-LRW-14 — US915 sub-band selection & channel mask (D, A; maps #303)
- **Steps:** set `lrw-sub-band` to a non-default value matching the gateway (e.g. 1), save,
  `lrw_reset` + `lrw_join`; then repeat with `0` (all 64+8 channels) if the gateway supports it.
- **Expect:** the device joins/uplinks only on the selected sub-band's 8 channels (LNS-side channel
  mask matches); `0` uses the full plan. Mismatched sub-band vs gateway = no join (expected — proves
  the mask is applied, not ignored).
- **Cleanup:** restore `lrw-sub-band 2`.

### AT-LRW-15 — US915 up/downlink, DR budget & RX2 (DR, A; maps #303, L5, L6, L10)
- **Steps:** after AT-LRW-13, capture periodic fPort-2 telemetry and run the AT-LRW-06 downlink
  matrix (get_info, set_param+readback, reboot, lrw_reset, …) over the US915 LNS; note the negotiated
  uplink DR and confirm a downlink is received in RX2.
- **Expect:** uplinks flow and decode identically to EU868 (`ttn.js` is region-agnostic — any
  divergence = HIGH); telemetry fits the smallest US915 uplink DR max payload (DR0 ≈ 11 B → payload
  splits across frames, never silently dropped — cross-check with `ats lrw compose 11` on debug);
  RX2 downlink lands (US915 RX2 fixed at **DR8**, 500 kHz); ADR behaves.
- **Evidence:** DR per uplink, one decoded downlink response hex, multi-frame split at DR0.

## 10. NFC + Manager-App (AT-NFC)

All scenarios need the phone fixtured on the tag (one §18 assist batch covers the whole
section — ~10 min). `{PHONE_ADB_SERIAL}` + control server per §3.6. Cross-check both sides:
phone JSON response AND (debug FW) RTT log of the NFC transaction.

### AT-NFC-01 — plaintext info record (DR, SA; maps N-series/inf)
- **Steps:** `curl localhost:8429/info`.
- **Expect:** serial == `{STICKER_SN}`; firmware/build match `{FW_REF}`/`{FW_VARIANT}`;
  nonceCounter ≥ last known; configVersion present.

### AT-NFC-02 — encrypted GetInfo round-trip (DR, SA; maps N4)
- **Steps:** `POST /comms` (encrypted, key from store or `{STICKER_KEY}`) → `POST /command
  {"op":"getinfo"}`.
- **Expect:** decoded info equals AT-NFC-01 + NFC-only fields (lrw_state, dev_eui,
  device_status); nonce advanced by the transaction.

### AT-NFC-03 — setparam → save → reboot → verify (DR, SA; maps N1, K6)
- **Steps:** `setparam {"app.interval-report":300}` → `getparam` readback → shell/NFC
  settings_save → after reboot `getparam` again.
- **Expect:** value persisted; device rebooted exactly once; nonce continuity across reboot.
- **Cleanup:** restore interval.

### AT-NFC-04 — full config walk timing (DR, SA; maps C4/N)
- **Steps:** `GET /config`; record per-page and total timing from the response.
- **Expect:** completes ≤ 32 pages, no page retries; total time recorded → §20 (UX budget:
  a config walk that takes tens of seconds on a phone tap is an improvement item).

### AT-NFC-05 — anti-replay (DR, SA; maps N8, #179/#184)
- **Steps:** perform a getinfo; then `POST /comms` forcing `counter` back to the *previous*
  value (manual source) and repeat the same command.
- **Expect:** device refuses the replayed nonce (error response or silence + unchanged
  state); the response cache does not leak the old plaintext. Then re-seed from the tag
  (`keyFromMemory`/fresh `/info`) and confirm normal operation resumes.
- **Evidence:** both transactions' results + RTT log lines (debug).

### AT-NFC-06 — nonce window upper bound (D, SA; maps #266)
- **Steps:** craft a command with nonce = current + 1024 (accepted, window edge) and
  current + 1025 (rejected) — via `/comms` counter override.
- **Expect:** in-window accepted and becomes the new high-water; beyond-window rejected
  (anti-brick bound holds).

### AT-NFC-07 — NFC-only key readback (D, SA; maps C10, #162)
- **Steps:** encrypted GetParam of `lorawan.lrw-appkey` over NFC.
- **Expect:** key returned over NFC; the identical request over LRW (AT-CFG-03) refused.

### AT-NFC-08 — paged history readout (DR, SA; maps #260)
- **Pre:** history enabled with ≥ 100 records (AT-HIS-02 first).
- **Steps:** debug shortcut: `ats cmd history [<from> [<to>]]` drives the paged read
  end-to-end; phone path: issue ReqHistoryPage commands (proto_id 13, NFC-only) if/when
  the app exposes it — otherwise mark phone-path SKIPPED (app support pending).
- **Expect:** ~28 records/page; `next_ord`/`has_more` chain terminates; record count and
  timestamps match `history stats`.

### AT-NFC-09 — RF loss mid-transaction & info restore (DR, SA; maps N-series, #164)
- **Steps:** start a `GET /config` walk and ask the operator to lift the phone mid-walk;
  wait ~15 s; re-read `/info`.
- **Expect:** device recovers (info record restored on the tag ~10 s after RF loss); no
  wedge; a fresh command round-trip succeeds; no partial-write corruption in the response
  record.

## 11. Sensors (AT-SEN) — the user-assist heavy section

Each scenario is one §18 assist block; batch them into a single operator session ordered as
below (magnet → inputs → touch → PIR → accel → 1-Wire), ~15 min total. On debug FW use
`ats sensors check <sensor> [timeout]` to watch live and `ats sensors sample` before/after;
on release FW verify via the next fPort-2 telemetry (slower — prefer debug for this section).

### AT-SEN-01 — SHT4x temperature/humidity (D, SA; maps S1)
- **Steps:** baseline `ats sensors sample`; assist: "hold a finger on the SHT sensor
  (~30 s)"; `ats sensors check temperature 60`.
- **Expect:** temperature rises ≥ 2 °C, humidity rises; values return toward baseline after
  release. Plausible absolute range (15–35 °C indoors).

### AT-SEN-02 — hall left/right (D, SA; maps S8)
- **Steps:** assist: "approach the LEFT sensor with the magnet, hold 2 s, remove; repeat 3×;
  then the RIGHT one." Watch `ats sensors check hall_left 60`, then sample.
- **Expect:** state edges detected with correct polarity (#352: `hall_left`/`hall_right` = `1`
  while the magnet is present, `0` at rest — a device reading `1` at rest, or `0` while a
  magnet is held, is the double-`GPIO_ACTIVE_LOW`-negation regression this issue fixed); per-side
  counters +3 each; no cross-talk (left magnet must not count on right).

### AT-SEN-03 — inputs A/B (D, SA; maps S9)
- **Steps:** assist: "short input A to GND 3×, then input B". Watch check/sample.
- **Expect:** counts +3 per input; state reflects level while held (#352: `input_a`/`input_b` =
  `1` while shorted to GND, `0` idle — same polarity regression check as AT-SEN-02).

### AT-SEN-04 — PIR (D, SA; maps S5)
- **Pre:** `cap-pir-detector` on.
- **Steps:** keep the bench still 60 s (baseline: no events) → assist: "wave a hand 30 cm
  above the device 3×".
- **Expect:** motion events only during the waves; none during the still window (false
  positives = finding).

### AT-SEN-05 — accelerometer orientation/motion/free-fall (D, SA; maps S2–S4)
- **Pre:** `cap-accelerometer` on (needs save+reboot if it was off — deferred init).
- **Steps:** assist 3 parts: tilt the device to each face (orientation), shake gently
  (any-motion count at each `accel-motion-sensitivity` level low/medium/high), and a
  short guarded drop onto foam (free-fall → alarm path, only if an alarm rule is armed).
- **Expect:** orientation tracks faces; motion count scales with sensitivity; free-fall
  fires the alarm (fPort-3) once.

### AT-SEN-06 — 1-Wire enroll/plug/unplug (D, SA; maps S6)
- **Pre:** `cap-w1-sensors true` + save (bridge is deferred-init: only initialised at boot).
- **Steps:** assist: "plug DS18B20 probe #1 into EXT1" → `w1 scan` → `w1 enroll 1` →
  `ats sensors sample` (expect a fresh conversion, use timeout 8) → assist: "unplug it" →
  sample again → assist: "replug" → sample.
- **Expect:** scan finds the ROM; enrolled slot reports plausible temperature; unplugged
  slot reports null/sentinel (0x7FFF → decoder null), not a stale or garbage value;
  replug recovers without reboot.

### AT-SEN-07 — no-data watchdog → alarm (D, SA; maps PR#205 no-data)
- **Pre:** AT-SEN-06 done; a rule on the enrolled slot armed (AT-ALM-02).
- **Steps:** assist: "unplug the probe and leave it out ≥ 10 s".
- **Expect:** sensor-silent watchdog raises the no-data alarm on fPort 3; fPort-2 shows
  null for that slot; replug clears on next sample.

### AT-SEN-08 — barometer & light (D, SA; maps S10)
- **Steps:** assist: "cover the light sensor with a finger, then shine a phone torch on it";
  baro: just sample (ambient plausibility 950–1050 hPa).
- **Expect:** illuminance spans dark→bright monotonically; pressure plausible and stable.

### AT-SEN-09 — battery/voltage read (D+PPK2, A; maps S11)
- **Steps:** `ats sensors sample` voltage line at {PPK2_MV}=3000, then `echo "V 2800" >
  /tmp/ppk_cmd`, resample.
- **Expect:** reading tracks supply within +30–60 mV divider offset; battery mV also
  reflected in GetInfo (AT-LRW-04).
- **Cleanup:** back to 3000 mV.

### AT-SEN-10 — sensor serials & utilities (D, A; maps S13)
- **Steps:** `ats sensors serial`, `ats sensors reset`, resample.
- **Expect:** serials printed for fitted sensors; counters zeroed by reset.

## 12. Alarm engine (AT-ALM)

Alarm rules live in 16 packed byte-slots (`alarm-0`…`alarm-15`); layout
`[0]flags [1]source [2]quantity [3]from [4]to [5..8]lo [9..12]hi [13..16]dwell` (floats LE).
On debug, manage via `alarm new/set/list/clear/poll`; over transports via SetParam on the
slot bytes (the manual plan's set_param vector includes a worked alarm_0 example).

**#348: `dwell` is a per-rule dwell/hold duration in seconds, not a hysteresis band.** `alarm-notif-time`
and the illuminance-only `alarm-light-confirm-delay` config keys are removed — every kind now
reads its own timing from the rule's `dwell` field (see `doc/version 1.4.md` §7 for the full
per-kind table). The scenarios below test each kind's specific `dwell` role, plus the "early
revert resets the window" behavior that a naive check-once-at-expiry timer would get wrong
(AT-ALM-01b/03b). `alarm poll` forces one evaluation pass immediately — use it to pin down
timing without waiting for the 3 s main-loop cadence, but note the dwell/hold deadlines
themselves are still wall-clock (`k_uptime_get()`), so waiting real time past the deadline is
still required before the next `alarm poll` will observe it as elapsed.

### AT-ALM-01 — onboard temperature threshold + dwell (D, SA; maps A1)
- **Steps:** arm slot 0: onboard temp, hi = ambient + 3 °C, dwell = 5 (`alarm set 0 onboard
  temperature <lo> <hi> 5`); `alarm poll` for baseline (no alarm); assist: finger on sensor
  until it crosses; `alarm poll` immediately (before 5 s), then again after 5 s; then cool
  down and `alarm poll` once more.
- **Expect:** the immediate poll (before the dwell elapses) shows no alarm yet; the poll
  after ~5 s shows exactly one alarm (fPort 3 decoded: type/threshold/value); the poll after
  cooling shows an immediate "cleared" transition, no matching dwell on the way down.
- **Cleanup:** `alarm clear 0`.

### AT-ALM-01b — dwell window resets on an early revert (D, SA; maps A1)
- **Steps:** same slot 0 as AT-ALM-01 (dwell = 5); assist: cross the bound, hold ~2 s, release
  back inside the bound, `alarm poll` (expect no alarm — dwell interrupted); wait a further
  4 s and `alarm poll` again (still expect no alarm — the window was reset, not just paused,
  so the ~2 s spent outside the bound the first time does NOT carry over).
- **Expect:** no alarm fires from the combined ~6 s of intermittent exposure, since no single
  continuous stretch reached 5 s. This directly exercises the #348 "continuous, not
  check-once-at-expiry" dwell requirement — a regression here would silently readmit the old
  illuminance-dwell weakness (#319) across every threshold quantity.
- **Cleanup:** `alarm clear 0`.

### AT-ALM-02 — 1-Wire slot threshold (D, SA; maps A2)
- Same as AT-ALM-01 with source = enrolled slot 1 (warm the probe in hand).

### AT-ALM-03 — state alarms hall/input, edge & level with confirm+hold (D, SA; maps A3, A4)
- **Steps:** arm an edge rule with a confirm+hold window, `alarm set 0 hall-left state 0 1 5`;
  assist: apply the magnet, `alarm poll` immediately (expect no alarm — confirm not yet
  elapsed), `alarm poll` again after 5 s (expect exactly one alarm), then re-apply the magnet
  within the next 5 s and `alarm poll` (expect no second alarm — hold/re-arm-blocked). Then
  arm a level rule `alarm set 0 hall-left state 1 1 5` and confirm it only activates once the
  magnet has been held continuously past 5 s, clearing immediately on removal.
- **Expect:** edge fires only after the 5 s confirm, then blocks re-arming for the same 5 s;
  level dwells the same way on activation but clears immediately; with `alarm-limit 0`
  (explicitly set, not the baseline since #346) ⇒ dual uplink per edge (A13 behaviour).
- **Reverse direction** (`alarm set 0 hall-left state 1 0 5`, edge on removal): HW-confirmed
  symmetric to the `0->1` case. A falling edge gives no counter tell (hall counter only counts
  rises) — use `ats sensors sample`'s `active` field, not the counter, to confirm the physical
  transition has genuinely settled before timing the 5 s dwell.

### AT-ALM-03b — edge confirm resets on an early revert (D, SA; maps A1b analogue for STATE)
- **Steps:** same edge rule as AT-ALM-03 (dwell = 5); assist: apply the magnet, hold ~2 s,
  remove it before 5 s, `alarm poll` (expect no alarm); re-apply and hold the full 5 s,
  `alarm poll` (expect exactly one alarm, timed from this second application, not credited
  for the earlier partial hold).
- **Expect:** the interrupted first attempt contributes nothing to the second — same
  continuous-dwell requirement as AT-ALM-01b, now verified for the STATE edge path.

### AT-ALM-04 — momentary PIR/accel one-shot, dwell as hold/re-arm only (D, SA; maps A5)
- **Pre (accel):** `cap-accelerometer true` alone is not enough — `accel-motion-sensitivity`
  defaults to `off` and must also be set (`low`/`medium`/`high`) for the LIS2DH12 any-motion
  interrupt to fire; both need `settings save` + reboot (deferred init).
- **Steps:** arm PIR state one-shot with a hold window, `alarm set 0 pir state 0 1 8`; wave
  once and `alarm poll` immediately.
- **Expect:** the alarm fires on the very same poll as the pulse (no confirm delay — momentary
  sources fire immediately, unlike AT-ALM-01/03); a real shake/wave produces many rapid pulse
  events (`accel-motion`/`motion-count` can jump by dozens for one shake) — only the first
  activates, the rest are correctly hold-suppressed, not each attempting its own fire; a second
  wave within 8 s produces no second alarm; a wave after 8 s does. Red/orange event LED behaviour
  is covered separately in A12 (commissioning-only orange diagnostic, fixed timing) vs A14 (red
  alarm-active blink, tracks `active` — i.e. tracks this same 8 s hold, not a separate LED timer).

### AT-ALM-05 — rate (count) alarm, dwell as hold/re-arm (D, SA; maps A6)
- **Gotcha:** `interval_report` has a hard shell-enforced 60 s minimum — cannot be shrunk for a
  faster cycle, each window costs a real 60+ s wait. Re-arming via `alarm clear all` immediately
  followed by `alarm new <same source+quantity>` does **not** reset the runtime window state
  (`rt_sync()` only resets on a source/quantity mismatch) — insert `alarm poll` between `clear
  all` and `new` to force a true reset, otherwise a "fresh" rule silently inherits a stale
  `count_window_start`/baseline from the previous run and appears to not fire.
- **Steps:** rule: hall count rate hi=2 per report interval, dwell = 10 (`alarm clear all`,
  `alarm poll`, `alarm new hall-left count 2 10`); assist: 3 magnet passes within one interval,
  then another 3-pass over-rate window within 10 s of the first firing, then one more after 10 s.
- **Expect:** fires on the first over-rate window (delta > 2, not on 2 or fewer); the second
  over-rate window within the 10 s hold produces no second alarm; the one after 10 s does.

### AT-ALM-06 — rate limiting (D, SA; maps A11)
- **Steps:** `config alarm-limit 60`, save; trigger the same alarm 3× within 60 s.
- **Expect:** first uplink immediate, subsequent suppressed until the window elapses;
  suppressed events still counted/batched per spec. (Unaffected by #348 — `alarm-limit` is a
  separate, still-global uplink throttle, not a per-rule timing.)
- **Cleanup:** restore baseline `alarm-limit 10` (not `0` — that was the pre-#346 default).

### AT-ALM-07 — undervoltage latch (DR+PPK2, A; maps #210, S11)
- **Pre:** `battery-level 2400` (default); PPK2 holder running.
- **Steps:** `echo "V 2300" > /tmp/ppk_cmd` → wait one sample period → expect fPort-3
  low-battery + red LED (assist confirm on release) + device_status low-batt bit in
  GetInfo; then `V 3000` → verify hysteresis-based recovery.
- **Expect:** alarm exactly once (latched, not repeating every sample); status bit tracks.
  (Unaffected by #348 — the battery watchdog is independent of the 16 rule slots and keeps
  its own fixed 0.3 V hysteresis, not `dwell`.)
- **Evidence:** decoded alarm + before/after GetInfo.

### AT-ALM-08 — invalid rule rejected/sanitised (D, A; maps H-10, #203)
- **Steps:** (a) via SetParam, attempt slot 15 with an inverted/collapsed THRESHOLD band
  (`hi <= lo`, e.g. `lo=20, hi=20`) — expect rejection, not silent acceptance (#348 replaces
  the old `hi-lo <= 2*dwell` band-collapse guard with a direct `hi > lo` check, since `dwell` no
  longer offsets the band); (b) attempt a rule with `dwell` outside `[0, 3600]` (e.g. `-1` or
  `4000`) on any kind — expect rejection; (c) write a malformed 17-byte blob into `alarm-15`
  directly via SetParam (bad source/quantity, flags=present) and read `alarm list`.
- **Expect:** (a) and (b) rejected with an error, slot NOT stored; (c) invalid slot
  sanitised/ignored with a report (reload returns invalid count); the other 15 slots
  unaffected; no crash on the next poll.
- **Cleanup:** clear slot 15.

### AT-ALM-09 — alarm-driven buzzer melody (D, SA; maps #397)
- **Pre:** `cap-buzzer true` (buzzer HW variant fitted), `alarm-buzzer-mode normal`,
  save/reboot (#154, config save reboots). One armed rule, e.g. AT-ALM-01's
  onboard-temperature slot 0.
- **Steps:** cross the threshold to activate the rule (same assist as AT-ALM-01); assist:
  "listen for a repeating fast 5x-beep melody starting within a couple of seconds of the red
  alarm LED"; wait > 30 s and assist-confirm it repeats; release the threshold to clear the
  alarm; assist: "confirm the buzzer goes silent immediately, not fading out or finishing its
  current repeat cycle". Mode sweep (spot-check): repeat the activation with
  `alarm-buzzer-mode once` (single melody, no repeat) and `fast` (repeats every ~10 s).
  New-alarm replay: with `normal` and the first alarm still active, activate a SECOND rule on
  a different source — assist-confirm the melody replays right away (within the 3 s poll
  cadence) instead of waiting out the 30 s repeat.
- **Expect:** melody starts on each newly activated alarm, replays per the mode's interval
  (`once` = never, `slow`/`normal`/`fast` = 120/30/10 s, `continuous` ≈ every 2.4 s) while
  any alarm stays active, stops immediately once the last one clears. Negative case:
  `alarm-buzzer-mode off` (or `cap-buzzer false`) → same rule activation produces the red
  LED as normal but stays silent.
- **Evidence:** operator confirmation of beep timing per mode + the negative (silent) case.
- **Cleanup:** clear the rules; `alarm-buzzer-mode off` (factory default).

## 13. History (AT-HIS)

Backend: raw-flash page ring, 32 KB @ 0x34000 (16 × 2 KB pages, shrunk from 64 KB via #302/PR#306),
32 B header + 1764 B data/page, double-word self-persisting records (marker 0xA5), count re-anchored
from page headers on boot. Record ≈ 44 B all-caps-on → ~40 records/page → ~600 captures to fully wrap.

### AT-HIS-01 — enable / sensor selection (D, A; maps H1, H2)
- **Steps:** `config history-enable true` (+ `history-sensors <bitmask>` variant), save;
  `interval-sample 5` for fast accumulation; wait 60 s; `history count`, `history read 5`.
- **Expect:** records accumulate at the sample interval, only selected sensors present;
  timestamps fixed-interval (TX jitter must NOT contaminate history timing).

### AT-HIS-02 — record/read/stats/clear (D, A; maps H3, H4, H5)
- **Steps:** accumulate ≥ 50 records (note: on debug this is the RAM ring,
  `CONFIG_APP_HISTORY_RAM_COUNT`-sized, not the 64 KB flash ring — see the variant caveat
  below); `history stats`; `history read 10`; `history clear`; `history count`.
- **Expect:** stats coherent (count, base time, interval); `history read <N>` returns the
  last N records in **ascending chronological order** (oldest of the window first — verified
  empirically, not newest-first); clear zeroes immediately.
- **Debug/release caveat:** on debug, `clear` zeroing does NOT need a reboot to prove itself
  and a reboot is a separate, expected wipe (see AT-HIS-03) — don't conflate the two.

### AT-HIS-03 — persistence across reboot & power-cut (**R**, A; maps H8)
- **Pre:** **release FW required.** Debug builds have `CONFIG_APP_HISTORY_FLASH=n`
  (`app/debug.conf`) — history lives in RAM only and is *always* lost on any reset, by
  design (the 32 KB history-flash budget is reallocated to code space on debug, see
  `boards/sticker/sticker.dts` code_partition comment). This was empirically confirmed
  2026-07-07: 44 RAM-accumulated records on a debug build → 0 after a clean
  `ats device reboot`; a raw flash dump at the history_partition offset (0x34000) showed
  plain firmware rodata/log strings, not a history ring — i.e. on debug that address range
  is inside the code image, not a separate partition. **Do not run this scenario on debug
  and report the result as a history bug** — it is expected behavior, not a finding.
- **Steps (release only):** accumulate N records → note count → (a) `ats device reboot`;
  (b) PPK2 hard power-cut mid-run → repower; `history count` after each.
- **Expect:** count and base time re-anchored exactly (a) and to within the last completed
  double-word (b) — a torn tail is dropped, never garbage-decoded.

### AT-HIS-04 — wrap & eviction (**R**, A; maps #265 HW-valid set)
- **Pre:** release FW (the 64 KB flash ring only exists with `CONFIG_APP_HISTORY_FLASH=y`;
  the debug RAM ring is a different, much smaller capacity — wrap behavior there is a
  distinct, lower-value scenario and not a substitute for this one).
- **Steps:** `interval-sample 5`, all sensors on, run until count plateaus (capacity of the
  64 KB ring) + one more page worth.
- **Expect:** oldest page evicted, count stable at capacity, newest records intact,
  `history stats` shows the moved base time. (Long: ~hours — schedule inside a soak run.)

### AT-HIS-05 — torn-tail injection (**R**, A; adversarial-adjacent)
- **Pre:** release FW (boot-time re-anchor from flash page headers is meaningless on the
  debug RAM ring, which has no persistence to re-anchor from).
- **Steps:** while records are streaming (1 s effective cadence via minimum
  interval-sample), JLinkExe `halt` mid-write, then `r;g`.
- **Expect:** boot re-anchor drops at most the in-flight record; no CRC/parse errors in the
  log; subsequent records append cleanly.

### AT-HIS-06 — LRW replay ReqHistory (DR, A; maps H6, H7)
- **Steps:** downlink `req_history` with a from/to window covering known records; also an
  empty window.
- **Expect:** history frames stream on fPort 85 and reassemble in the decoder to exactly
  the expected records; empty window yields the documented empty response, not silence.

### AT-HIS-07 — NFC paged readout — see AT-NFC-08 (shared scenario).

## 14. Clock / RTC (AT-CLK)

### AT-CLK-01 — unsynced behaviour (D, A; maps K1, L-1/L-3/L-4)
- **Steps:** after `lrw_reset`+reboot with no network time: `clock get`; capture one
  telemetry + one history record.
- **Expect:** device reports unsynced (time_synced flag clear on the wire); decoder renders
  ~1970 timestamps as null — never as a bogus date.

### AT-CLK-02 — clock set/sync paths (DR, A; maps K2, K3, K6)
- **Steps:** (a) shell `clock set <unix>`; (b) `clock sync` → DeviceTimeReq over ChirpStack;
  (c) NFC `clocksync` via control server (deferred info response carries unix_time).
- **Expect:** each path sets RTC within ±2 s of host time; GetInfo `unix_time` agrees;
  time_synced flag set afterwards.

### AT-CLK-03 — history timestamps track RTC (D, A; maps K5)
- **Steps:** sync clock, accumulate 10 records, compare record times vs host wall-clock.
- **Expect:** monotonic, fixed-interval, absolute error < a few seconds.

### AT-CLK-04 — RTC across reset types (D, A; maps K4)
- **Steps:** sync → soft reboot → `clock get`; then hard power-cut → `clock get`.
- **Expect:** documented behaviour per reset type (soft reboot keeps time if designed to;
  power-cut loses it → device returns to unsynced semantics, AT-CLK-01, and re-syncs on
  next DeviceTimeReq).

## 15. Power (AT-PWR) — PPK2 measurement section

**Setup for all measurement scenarios:** J-Link **physically detached**, DUT power-cycled
after the last SWD session (DBGMCU latch), PPK2 in source-meter mode with `start_measuring`
(accept the post-run PPK2 raw-reset, §3.7). Debug builds have PM=n — only release FW gives
representative numbers; debug numbers are recorded as "diagnostic only".

Baselines from `doc/power-consumption.md` (3.3 V bench): deep floor ≈ 3.7 µA (Stop2),
operating mid-band ≈ 55–58 µA, active CPU ≈ 4.5 mA @ 3 V, accelerometer enabled ≈ +30 µA.
Regression tolerance: ±20 % on µA-class averages, flag anything beyond.

### AT-PWR-01 — idle current floor (R, A)
- **Steps:** baseline config (radio joined, interval-report 900, no history), measure 5 min
  after 2 min settling; compute average excluding TX bursts.
- **Expect:** within tolerance of the 55–58 µA mid-band (or the annex's device-specific
  baseline). > +20 % = HIGH power regression finding.

### AT-PWR-02 — Stop2 / suspend floor (R, A/SA; maps #113)
- **Steps:** trigger suspend (2 h idle path is impractical — use the debug autosuspend
  Kconfig if built, or `power suspend` set up before J-Link detach); measure 2 min.
- **Expect:** ≈ 3.7 µA floor; wake only via the designed sources (NFC GPO / NRST).
- **Note:** with any probe attached the device instantly wakes from Shutdown — this
  scenario is meaningless unless SWD is detached.

### AT-PWR-03 — boot & join energy profile (R, A)
- **Steps:** start measuring, power-cycle via PPK2, capture 120 s: boot spike, LED
  carousel, join TX bursts, settle.
- **Expect:** single boot signature (cross-check AT-BOOT-04); join burst count sane
  (1 join + GetInfo + first telemetry); settle to idle band.
- **Evidence:** trace CSV + annotated segment averages.

### AT-PWR-04 — TX-period signature as release liveness (R, A)
- **Steps:** interval-report 60; measure 10 min; detect TX bursts by threshold.
- **Expect:** ~10 evenly spaced bursts (+jitter ≤ min(interval/10,10 s)) — this doubles as
  the no-RTT liveness check for release FW and feeds AT-LRW-10 evidence.

### AT-PWR-05 — per-feature deltas (R, A)
- **Steps:** measure idle band in 4 configs (toggle + save between runs, via NFC to avoid
  SWD): accel on/off, history on/off (interval-sample 60), PIR on/off, 1-Wire cap on/off
  (probe attached).
- **Expect:** accel ≈ +30 µA; each other delta recorded to the annex as the new baseline;
  unexpectedly large deltas → §20.

### AT-PWR-06 — undervoltage sweep to brown-out (R+PPK2, A; extends AT-ALM-07)
- **Steps:** step V down 3000→2400→2300→…→1300 mV in 100 mV steps, 60 s each, watching the
  LNS for the low-battery alarm and for the last successful uplink.
- **Expect:** low-batt alarm below 2400 mV; device keeps operating (degrade-not-die) down
  to the documented ~1.3 V region; no flash-corrupting misbehaviour after recovery to 3 V
  (config intact, history intact — verify via AT-HIS-03-style check).
- **Cleanup:** 3000 mV; full functional smoke (get_info via NFC or LNS).

### AT-PWR-07 — debug-vs-release power sanity (D, A, diagnostic)
- **Steps:** one 2-min average on the debug build (PM=n).
- **Expect:** mA-class (CPU never sleeps) — recorded only to catch gross anomalies (e.g. a
  busy-loop regression doubling debug draw).

### Feature/variant power sweep (AT-PWR-08..10)

**Release FW + J-Link physically detached, no exception** (§15 preamble). Debug PM=n never sleeps
→ mA-class garbage; any attached probe adds 60–100 µA and wakes the device from Stop2 → the numbers
are meaningless. Power-cycle after the last SWD session (DBGMCU latch). Toggle config between runs
**over NFC** (or set before the final release flash) so no SWD re-attach is needed mid-sweep. Every
delta is appended to the power annex / `doc/power-consumption.md` as the new baseline.

### AT-PWR-08 — LoRaWAN radio power delta, on vs off (R, A; maps #271)
- **Steps:** measure the 5-min idle average in two modes (toggle via NFC + save): `radio-mode
  lorawan` (joined, `interval-report 900`) vs `radio-mode off` (radio silent — no join, no periodic
  TX). In the `lorawan` run also capture ≥ 1 full report interval so the per-uplink TX burst energy
  is included and integrated.
- **Expect:** `off` idle ≤ `lorawan` idle (no radio housekeeping); the idle delta **and** the
  per-TX burst energy (mJ/uplink) recorded. This quantifies the radio's idle overhead and the cost
  of each uplink — the dominant energy term at short report intervals.

### AT-PWR-09 — 1-Wire sensor variants: none / 1× Dallas / 2× Dallas / machine probe (R, SA; maps #295)
- **Setup:** operator swaps the 1-Wire fixture between runs (semi-assisted).
- **Steps:** for each variant — (a) **no 1-Wire sensor**, (b) **1× Dallas DS18B20**, (c) **2×
  Dallas**, (d) **machine probe** — provision the matching `cap-w1-sensors` + taught ROMs (config
  save reboots; 1-Wire inits at boot only), then measure both the **idle band** (5 min) and the
  **per-sample burst energy** at `interval-sample 60` (the DS2484 bridge + bus power up per sample).
- **Expect:** idle floor ≈ equal across variants (the bus is powered only during a sample, not at
  idle); per-sample burst energy grows with sensor count / probe type — this is the #295
  characterization. A variant that raises the **idle** floor (bus/bridge left powered) = HIGH finding.
- **Evidence:** variant × {idle µA, mJ/sample} table into the annex.
- **Cleanup:** restore the bench-default sensor config.

### AT-PWR-10 — configuration power matrix, bare floor → full (R, A/SA)
- **Steps:** build the annex power matrix from 5-min idle averages, adding one feature at a time
  (toggle via NFC + save): **bare floor** (no sensors, `radio-mode off`, accel off, history off) →
  + LRW on → + accel on → + history on (`interval-sample 60`) → + SHT4x cap → + 1-Wire (per
  AT-PWR-09). Also record the fully-loaded config (everything on).
- **Expect:** the **bare floor** ≈ the idle/Stop2 baseline in `doc/power-consumption.md` (nothing
  but the housekeeping tick); each single-feature delta is additive and matches
  AT-PWR-05/08/09; the full config ≈ sum of deltas within ±20 %. Non-additive jumps or a bare floor
  above baseline → §20 improvement item.
- **Evidence:** variant × idle-µA matrix appended to the annex (the headline "how much does each
  option cost" deliverable).

## 16. Adversarial & "unrealistic" scenarios (AT-ADV)

Purpose: not to pass — to **find soft spots**. Any crash, wedge, reboot loop, or silent
state corruption here is a real finding even though no user would ever do this. Everything
in this section must end with a full functional smoke (get_info round-trip + one telemetry
cycle) to prove the device recovered. Run these LAST in a session.

### AT-ADV-01 — malformed fPort-85 downlinks (DR, A)
- **Steps:** send: empty payload, 1×0x00, 255×0xFF, each known-good vector truncated at
  every byte boundary, valid header + garbage body, valid protobuf with unknown proto_id
  (e.g. 19/20/22 — the reserved/removed ids), and nested-length-overflow protobuf. Debug:
  `ats cmd lrw <hex>` (fast); release: via ChirpStack enqueue (slower — sample 10 worst cases).
- **Expect:** every frame either yields a decoded Error response or is dropped with a log —
  never a reboot, never a wedge, never an ack.

### AT-ADV-02 — command fuzz campaign (D, A)
- **Steps:** generate 500 mutated vectors (bit flips, length twiddles, field-id swaps on
  the valid vector set) host-side; feed via `ats cmd lrw` and `ats cmd nfc`; after every 50,
  `ats device info` (liveness) + check uptime (no silent reboot).
- **Expect:** zero reboots (uptime monotonic), zero wedges, error responses well-formed.
- **Evidence:** the exact failing vector hex if anything trips (reproducer first-class).

### AT-ADV-03 — downlink flood (DR, A)
- **Steps:** enqueue 10 command downlinks at once on ChirpStack; run 10 uplink cycles.
- **Expect:** exactly one consumed per RX window, in order, none lost/duplicated, queue
  drains fully.
- **HW-verified (2026-08-17, SN 2162199999)**: enqueued 10 raw fPort-85 `SetParam` downlinks
  (unencrypted, `interval_report` 70..79) via ChirpStack `Enqueue`, drained with repeated
  `ats lrw check` + polling `config show`. Observed sequence was monotonically increasing
  (70,71,72,74,76,77,77,79,79,79,79 — some intermediate values missed by 5s poll granularity, not
  by the device), final value stable at **79** (the last item queued) and ChirpStack's
  `GetQueue` afterward returned empty — full drain, in order, no duplicates in final state.

### AT-ADV-04 — oversized/boundary config values (D, A)
- **Steps:** SetParam every int param at exact min/max (valid) and every bytes param at
  exact length, length−1, length+1; enum at max+1.
- **Expect:** exact boundaries accepted; off-by-one rejected consistently (cross-check
  AT-CFG-02 clamp-vs-reject consistency).

### AT-ADV-05 — NFC frame abuse (D, SA)
- **Steps:** via control server manual mode: wrong key (all-zero, bit-flipped
  `{STICKER_KEY}`), stale nonce (AT-NFC-05), nonce far-future (AT-NFC-06), truncated
  ciphertext; plus a raw NDEF abuse pass if a low-level writer is available: TLV length
  overrun, missing terminator, foreign NDEF records before `hio.stck:cmd`, Type-5 CC
  variants (#199 regression).
- **Expect:** all rejected cleanly; tag's `inf` record intact afterwards; no plaintext
  leakage in any response.

### AT-ADV-06 — power-cut during settings save (R+PPK2, A)
- **Steps:** issue SettingsSave via NFC and cut power ~50 ms later (scripted PPK2, **real
  `toggle_DUT_power("OFF")` — see the §22 correction, NOT `set_source_voltage(0)`, which does
  not actually remove power and would make this whole scenario silently test nothing**);
  repower (`toggle_DUT_power("ON")`); read full config. Repeat 5× with varied delays (0–500 ms).
- **Expect:** config is either fully-old or fully-new (NVS atomicity) — never a mix, never
  identity loss, never a boot loop.
- **HW-verified (2026-08-18, SN 2162199999, debug @ hynek/v140-final-review-fixes)**: shell
  `settings save` variant, cuts driven by `ppk2_orchestrator.py` (FIFO `cutin <delay> <off_ms>`,
  timestamped log). 6 cycles total: delays 0/100/250/400/550 ms all landed **pre-commit** →
  the OLD value survived, `config show` fully consistent (47 lines) every time; one extra
  cycle at **1500 ms** landed **post-commit** → the NEW value survived cleanly — both
  branches of the atomicity contract exercised, never a mix, identity intact every cycle.
  Reset causes cleanly discriminated: `0x00000005` (pin, brownout) for every cut vs
  `0x00000003` (pin, software) for the deliberate no-cut restore save. Note for repeats:
  with the ~0.2–0.5 s MCP/shell dispatch latency, delays ≤550 ms never reached the commit —
  use ~1.5 s to hit the post-commit window.

### AT-ADV-07 — power-cut storms (R+PPK2, A)
- **Steps:** 20 random power cycles (real `toggle_DUT_power` off/on, not source-voltage-0 — see
  AT-ADV-06's note; on-time uniform 1–30 s); then full smoke + AT-HIS-03
  + AT-CFG-07 checks.
- **Expect:** no identity/config/history corruption; no reset-cause anomalies; joins recover.
- **HW-verified (2026-08-18, SN 2162199999, 12 cycles — reduced from 20)**: on-times drawn
  uniform 2–25 s, 700 ms cuts, 3 cuts landed <5 s after power-up (mid-boot). Full smoke after
  the storm: identity/keys intact verbatim, `config show` 47 consistent lines, alarms clean,
  history clean, LRW RECONNECT→HEALTHY within ~30 s, RTC resynced via network time after
  rejoin, reset cause `0x00000005` (pin, brownout) as expected. No corruption, no boot loop.

### AT-ADV-08 — RF denial during join backoff (DR, semi-A)
- **Steps:** disable the device on the LNS (or take the gateway offline) → force rejoin →
  observe backoff for 15 min → re-enable.
- **Expect:** exponential backoff per L9 (no hammering); join succeeds promptly after
  re-enable; no watchdog trips while backing off.
- **HW-verified (2026-08-17, SN 2162199999)**: disabled the device on ChirpStack
  (`Device.is_disabled=true`), forced `ats lrw reset` — `Join failed (not activated)` each
  attempt, backoff grew attempt 1 (~60-72s incl. jitter) → attempt 2 (105s), confirming genuine
  exponential growth, not a fixed retry. Re-enabled the device, forced one more `ats lrw reset` —
  rejoined promptly (`state: HEALTHY`, fresh devaddr) within ~20s. `Reset cause` stayed
  `0x00000003` (pin, software) throughout — no watchdog trip during the whole backoff window.

### AT-ADV-09 — time abuse (D, A)
- **Steps:** `clock set` to: 0, 1, year-2038 boundary (0x7FFFFFFF), far future (0xFFFFFFF0);
  after each: one telemetry + one history record + decoder pass.
- **Expect:** firmware clamps/handles; decoder yields null or a correct date — never a
  crash or a negative interval; history base-time stays coherent.
- **HW-verified (2026-08-17, SN 2162199999)**: `clock set 0`/`1` correctly rejected (`-22`); the
  2038/`UINT32_MAX` boundaries in the steps above set/read back fine. **But the untested
  year-2100 boundary (RTC's 2-digit BCD year field only covers 2000-2099) is a real bug**: the
  first `clock set 4102444800` (2100-01-01) caused an **intermittent watchdog reset**
  (`Reset cause: 0x00000011`), 1 hit in ~7 attempts at/near that boundary — filed as **#386**.
  Always check `Reset cause` after a boundary clock test, not just `clock get`'s displayed
  value — a clean-looking readback doesn't rule out a hang on a repeat attempt.
- **Fixed + re-verified (2026-08-17)**: `app_clock_set_unix()` now rejects `unix_s >=
  APP_CLOCK_UNIX_MAX` before touching the RTC. `clock set 4102444800` re-tested post-fix —
  cleanly rejected (`-22`), no reset. Severity corrected on the issue: the real `clock_sync`
  Command path (LRW/NFC) already bounded `unix_time` via `APP_CMD_CLOCK_UNIX_MAX` in
  `app_cmd.c`; only the debug `clock set` shell path (dev/commissioning-trusted) was actually
  wide open — see the issue comment for detail.

### AT-ADV-10 — concurrency pile-up (D, SA)
- **Steps:** simultaneously: phone holds an NFC config walk, ChirpStack has a queued
  downlink, and the operator triggers an alarm (magnet) — repeat 3×.
- **Expect:** all three complete; no deadlock between NFC mailbox, LRW work queue and alarm
  path (LoRaMac is not thread-safe — this is the scenario that would expose a locking bug);
  uptime monotonic.

### AT-ADV-11 — shell abuse (D, A)
- **Steps:** oversized args, missing args, rapid-fire 100 commands, `ats led cycle 99`
  interrupted by reboot, `w1 enroll` on empty bus, `history read 99999`.
- **Expect:** usage errors, no crash; large reads paginate or cap (note: very large shell
  responses over RTT can overflow shell_rtt — a known pre-existing weakness; record
  severity, don't chase).

### AT-ADV-12 — storage exhaustion behaviours (D, A)
- **Steps:** history at capacity (AT-HIS-04 state) + continuous alarms + config churn
  (20 SetParam+save cycles).
- **Expect:** ring keeps evicting; NVS wear path shows no errors; no slow degradation of
  report cadence (compare uplink jitter before/after).

## 17. Long-run / soak (AT-SOAK)

### AT-SOAK-01 — 24 h release soak (R, A)
- **Pre:** release FW, interval-report 300, history on (interval-sample 60), one alarm rule
  armed on a quiet source.
- **Steps:** run ≥ 24 h; poll LNS hourly (agent may schedule itself); at end: full smoke +
  history readout + GetInfo.
- **Expect:** uplink continuity (no gap > 2 intervals), f_cnt strictly monotonic (no
  unexplained resets — reset_cause via GetInfo/status must show none), history record count
  == elapsed/interval ± 1, nonce/frame counters monotonic.
- **Evidence:** hourly liveness table + final counters.

### AT-SOAK-02 — join churn (DR, A)
- **Steps:** 20 cycles of `lrw_join` rejoin (spaced ≥ 2 min, mind duty cycle); track
  DevNonce/DevAddr on ChirpStack.
- **Expect:** every join succeeds; DevNonce strictly increases (no reuse → no join
  failures); no memory-leak symptoms (join latency stable from first to last).

### AT-SOAK-03 — debug memory watermarks (D, A)
- **Pre:** the standard debug build has `CONFIG_INIT_STACKS=y`/`CONFIG_THREAD_STACK_INFO=y`
  but NOT the `kernel stacks` shell command (`CONFIG_KERNEL_SHELL`/`CONFIG_THREAD_MONITOR`
  are off to save flash). This scenario needs a dedicated build: add
  `CONFIG_KERNEL_SHELL=y` + `CONFIG_THREAD_MONITOR=y` via an extra conf fragment (offset
  the ~5 KB with `CONFIG_SHELL_HELP=n` if debug flash overflows) — flash consent required.
- **Steps:** after AT-SOAK-01-style activity (shorter, 2 h), run `kernel stacks` hourly.
- **Expect:** every stack ≥ 15 % headroom; compare against the recorded baseline; shrinkage
  over the run = leak-suspect finding.

---

## 18. v1.4.0 critical-analysis scenarios (AT-REG / AT-LRW-18 / AT-DEF)

Added from the 2026-08-13 static+dynamic+HIL analysis campaign (baseline `origin/v1.4.0` @ `a6c684b`; full
findings report in the analysis note). These are regression guards for freshly-merged code, an LNS-portability
round-trip, and targeted repros for confirmed defects. Legend as elsewhere: D=debug, R=release, A/SA=assist.

### AT-REG-355 — hall/input polarity boot-transient guard (D; maps S8, #355) — **HW PASS 2026-08-13**
- **Why:** #355 fixed a double-inverted `GPIO_ACTIVE_LOW`; the pre-fix bug registered a spurious counter edge on
  the first poll after every boot (would corrupt every deployed door-counter on upgrade).
- **Steps:** `ats sensors sample` (note hall-left/right counts, no magnet) → `ats device reboot` (magnet-free) →
  reconnect → `ats sensors sample` again.
- **Pass/Fail:** counts must be **unchanged** across the magnet-free reboot. FAIL on any increment (pre-#355 signature).
- **Result 2026-08-13:** PASS on 2162199999 (hall-left 30→30, hall-right 6→6). Apply-on-magnet truth table still needs an operator magnet (assist §18).

### AT-LRW-18 — LNS migration round-trip: ChirpStack OTAA → TTN OTAA → TTN ABP (D, A) — **HW PASS 2026-08-13**
- **Why:** verify identity/key re-provisioning and LNS portability across activation modes; exercises the shell
  `config lrw-*` + `settings save` provisioning path end-to-end.
- **Steps:** for each leg, set the LRW keys over the shell (see [[reference_lrw_test_devices]]) + `settings save`,
  then confirm uplinks on the target LNS (CS: `GetActivation` dev_addr change + f_cnt_up rising; TTN: `get_uplinks`).
  1. **CS OTAA** (DevEUI …0010): if join fails with `Rx 2 timeout`, check CS `StreamDeviceFrames` — JoinRequests
     arriving + a `log` event `OTAA DevNonce has already been used` ⇒ `FlushDevNonces` on CS (§3.4 gotcha), reboot.
  2. **TTN OTAA** (DevEUI …0001): join (may need a 2nd attempt at SF12), confirm fPort-85 GetInfo-on-join + fPort-2.
  3. **TTN ABP** (DevEUI …C3AB, DevAddr 98DE812E): no join — HEALTHY immediately, confirm fPort-2 (decoder decodes
     hall/humidity), LinkCheck margin/gateways > 0.
- **Pass/Fail:** each leg joins (OTAA) / transmits (ABP) and delivers a decodable uplink on the target LNS; the
  re-provisioning must not disturb serial/secret_key. Restore to baseline TTN OTAA at the end.
- **Result 2026-08-13:** all three legs PASS on 2162199999. Root-cause found+fixed for the CS-OTAA DevNonce replay.

### AT-DEF-* — targeted repros for confirmed analysis defects (native_sim executable, HW = confirmation only)
These already have **executable native_sim repros** (in the analysis worktree `tests/`), so a HW pass is confirmation, not discovery:
- **AT-DEF-cfggate** (CRITICAL): SetParam over `APP_CMD_TRANSPORT_VENDOR`/`SHELL_DEBUG` on a field whose `writable:`
  list excludes that transport is wrongly **accepted** — the generated write-gate is a denylist (only LRW/NFC arms;
  `configen.py` `normalize_access` derives no `no_write_vendor`/`no_write_shell`). Repro: `tests/cmd` (3 failing ZTESTs).
- **AT-DEF-histoob** (CRITICAL, release): a `flash_area_write()` failure in `backend_append()` leaves `m_stage_len=7`
  → OOB write into `m_stage[]` on the next capture. HW: pair with AT-ADV-06/PPK2 power-cut during a history flash write.
- **AT-DEF-dechang** (HIGH): the payload decoder (`ttn.js`) infinite-loops on a malformed length-delimited uplink
  (`while(pos<end)` with `end` > buffer). Repro: `node -e '…decodeUplink({fPort:85,bytes:[1,0x1a,0x7f]})'` hangs.
- **AT-DEF-349** (HIGH): #349 `rt_sync` keeps a stale latch on an in-place STATE-rule edit; `eval_count` re-fires
  without `!rt->active` (ignores dwell/hold). Repro: `tests/alarm_eval` (2 ZTESTs).
- **AT-DEF-buzz** (HIGH): a `buzzer_play` downlink built via `encodeDownlink` drops `kind`/`repeat_s` → empty
  BuzzerPlay = STOP, acked success. HW: queue a `buzzer_play` downlink over LRW, confirm no beep + ack.

---

## Part III — Process

## 18. User-assist protocol

The agent runs unattended except for **assist blocks**. Rules:

1. **Batch.** Collect all assist-needing scenarios in the run plan into the fewest possible
   operator sessions (target ≤ 3 per run), ordered to minimise tool-swapping (all magnet
   actions together, all probe plug/unplug together, the phone-on-tag batch as one).
2. **Announce the batch up front:** list every action, total estimated time, and what the
   operator needs in hand (magnet, DS18B20 probe, jumper wire, phone).
3. **One action at a time**, using this exact block:

   > **ASSIST [n/total] — <action>** (~<seconds>s)
   > Do: <precise physical instruction, incl. which side/port and how long>
   > I will know it worked when: <the signal the agent watches — sensor delta, count, NFC response>
   > Say "done" when finished (or "skip" / "problem: …").

4. **Verify before moving on.** The agent watches its stated signal (e.g. `ats sensors
   check` output). If the signal doesn't appear within a timeout of 2× the estimate: retry
   the instruction once with more detail; on second failure mark the scenario
   INCONCLUSIVE-ASSIST (not FAIL) and continue the batch.
5. **Never let an assist block gate an automatic scenario** — reorder so automation runs
   while waiting for the operator where possible.

Assist action catalogue (estimate per action):

| Action | Used by | Est. |
|---|---|---|
| Hold finger on SHT sensor 30 s | AT-SEN-01, AT-ALM-01 | 60 s |
| Magnet approach/remove ×3, left then right | AT-SEN-02, AT-ALM-03/05, AT-ADV-10 | 60 s |
| Short input A/B to GND ×3 | AT-SEN-03, AT-ALM-03 | 60 s |
| Wave hand over PIR ×3 (+60 s still baseline) | AT-SEN-04, AT-ALM-04 | 120 s |
| Tilt device to each face / shake / guarded drop on foam | AT-SEN-05 | 120 s |
| Plug/unplug/replug DS18B20 in EXT1/EXT2 | AT-SEN-06/07, AT-ALM-02 | 90 s |
| Cover light sensor, then torch on it | AT-SEN-08 | 30 s |
| Fixture phone on the tag for the NFC batch | all AT-NFC | 10 min (one session) |
| Lift the phone mid-transaction on cue | AT-NFC-09 | 30 s |
| Watch LEDs after reset / during alarm and describe | AT-BOOT-02, AT-ALM-07 (release) | 30 s |
| Power-cycle the DUT on cue (if no PPK2) | flash recovery, AT-BOOT-04 | 15 s |
| Detach/reattach J-Link (power section) | all AT-PWR | 60 s |

## 19. Evidence & reporting

**Run directory** (host, one per run): `run-<date>-<fwref>-<variant>/` containing
`bench-profile.md` (Step 0 result), `run-plan.md`, per-scenario `AT-XXX-nn.md` evidence
files, raw captures (`rtt-*.log`, `uplinks-*.json`, `ppk2-*.csv`, `curl-*.json`), and
`report.md`.

**Per-scenario evidence** = the fields named in each scenario's **Evidence** line, plus:
start/end timestamps, FW variant, and the exact commands issued. Store raw before parsed.

**Verdicts:** `PASS` / `FAIL` (expectation not met — include a minimal reproducer) /
`INCONCLUSIVE` (environment/assist problem) / `SKIPPED` (missing placeholder or HW, name it)
/ `BLOCKED` (prerequisite scenario failed).

**Finding severity:**

- **CRIT** — data/identity loss, security bypass (key over LRW, replay accepted), brick.
- **HIGH** — wedge/reboot-loop/crash reachable in the field; decoder crash on LNS.
- **MED** — wrong value/behaviour with workaround; inconsistency between transports.
- **LOW** — cosmetic, log noise, doc mismatch.
- **IMPROVEMENT** — not a defect: efficiency, UX, robustness-hardening idea (§20).

**Report template** (`report.md`): header (bench profile summary, FW ref+hash, variant,
date, duration) → verdict table (scenario × verdict × 1-line note) → findings ranked by
severity, each with: what/where, reproducer, evidence link, suspected area
(file/subsystem), proposed next step → coverage delta vs §21 → improvement list (§20) →
"state of the device at end of run" (config restored? provisioning intact?).

Findings are **proposals to the operator**. Never open GitHub issues, never push branches
from a test run unless explicitly told to.

## 20. Improvement discovery — what to watch beyond pass/fail

While executing ANY scenario, record observations against these heuristics; they become the
IMPROVEMENT section of the report:

- **Latency & cadence** — command→response round-trip (NFC per-page timing in AT-NFC-04;
  downlink-to-effect over LRW); join time; boot-to-first-uplink. Flag anything a human
  would perceive as slow (NFC op > 3 s/page, config walk > 30 s).
- **Payload efficiency** — bytes/uplink vs information carried; fields sent that never
  change (candidates for delta encoding); frames that just missed a DR budget boundary.
- **Retry/robustness economics** — how often the FW retried anything silently (RTT log
  grep for retry/timeout patterns); backoff shapes.
- **Error-path quality** — every error observed in §16: is the message actionable? does the
  decoder surface it? would a field tech understand it?
- **LED/UX semantics** — can the operator actually distinguish the signals (§16 of
  `version 1.4.md`)? Note real-world confusions the assist operator reports.
- **Determinism** — same input, same output? Any scenario needing a retry to pass is a
  flakiness finding even if it eventually passed.
- **Resource headroom trend** — AT-HOST-05 flash/RAM, AT-SOAK-03 stacks, NVS wear
  observations: extrapolate, don't just snapshot.
- **Doc gaps** — every time this playbook or the manual plan disagreed with observed
  behaviour, record which side is wrong.
- **Test-surface gaps** — anything you wanted to verify but had no hook for (e.g. missing
  `ats` injector) is an IMPROVEMENT item proposing the hook.

## 21. Coverage matrix

Automation level A/SA/M, FW D/R/DR as defined in §1. `release-smoke` marks the minimal set
for every release candidate. Manual-plan IDs marked *(partial)* are consciously not fully
automated; *(excluded)* items are listed with reasons below the table.

| Manual ID | Automated scenario(s) | Lvl | FW | Assist | Release-smoke |
|---|---|---|---|---|---|
| G1 | AT-BOOT-01 | A | D | – | |
| G2 | AT-BOOT-02 | SA | DR | LED watch | |
| G3 | session setup (§3.3 rttt up + `ats device info`) | A | D | – | |
| G4, G4b, G5 | AT-LRW-04, AT-LRW-06 | A | DR | – | ✅ (AT-LRW-06) |
| G6, G6c | AT-BOOT-03 | A | D | – | |
| G6b | *(excluded)* `settings erase` — destructive, on explicit request only (§4) | – | – | – | |
| G7 | AT-SOAK-03 (heartbeat visible in log) | A | D | – | |
| G8 | AT-BOOT-05 | A | D | – | |
| L1 | AT-LRW-03 | A | DR | – | |
| L2 | AT-LRW-01 | A | DR | – | ✅ |
| L3 | AT-LRW-02 | A | DR | – | |
| L4 | AT-LRW-04 | A | DR | – | |
| L5, L6 | AT-LRW-05, AT-LRW-11; assert **no fPort-1 frames** during AT-LRW-05 (covers L12) | A | DR | – | |
| L7, L8, L13 | AT-LRW-07 | A | D | – | |
| L9 | AT-ADV-08 | A | DR | – | |
| L10 | AT-LRW-06 | A | DR | – | ✅ |
| L11 | AT-LRW-07/11 + AT-BOOT-01 (`ats lrw` surface) | A | D | – | |
| L12 | asserted inside AT-LRW-05 | A | DR | – | |
| L14 | AT-LRW-08 | A | D | – | |
| L15 | AT-LRW-09 (radio-mode supersedes DevEUI-zero guard) | A | D | – | |
| L16 | AT-LRW-10 | A | R | – | ✅ |
| #303 (US915) | AT-LRW-13, AT-LRW-14, AT-LRW-15 | A | DR | – | SKIP (RF) unless US915 gateway present |
| S1 | AT-SEN-01 | SA | D | finger | |
| S2, S3, S4 | AT-SEN-05 | SA | D | tilt/shake/drop | |
| S5 | AT-SEN-04 | SA | D | wave | |
| S6 | AT-SEN-06 | SA | D | probe | |
| S7 | *(on-request)* machine-probe variant of AT-SEN-06 — needs MP hardware on bench | SA | D | probe | |
| S8 | AT-SEN-02 | SA | D | magnet | |
| S9 | AT-SEN-03 | SA | D | jumper | |
| S10 | AT-SEN-08 | SA | D | cover/torch | |
| S11 | AT-SEN-09, AT-ALM-07 | A | DR | – | |
| S12, S12b | *(partial)* enter_calibration ack in AT-LRW-06; full fPort-10 calibration flow excluded — needs the calibration bench/fixed ABP keys | A | D | – | |
| S13 | AT-SEN-10 | A | D | – | |
| H1, H2 | AT-HIS-01 | A | D | – | |
| H3–H5 | AT-HIS-02 | A | D | – | |
| H6, H7 | AT-HIS-06 | A | DR | – | |
| H8 | AT-HIS-03 | A | R | – | |
| A1 | AT-ALM-01, AT-ALM-01b | SA | D | finger | |
| A2 | AT-ALM-02 | SA | D | probe | |
| A3, A4, A13 | AT-ALM-03, AT-ALM-03b | SA | D | magnet/jumper | |
| A5, A12 | AT-ALM-04 | SA | D | wave + LED | |
| A6 | AT-ALM-05 | SA | D | magnet | |
| A7 | arming path of AT-ALM-01..03 via SetParam + AT-NFC-03 | A/SA | D | phone | |
| A8 | rule change/clear exercised in AT-ALM cleanups + AT-ALM-08 | A | D | – | |
| A9 | *(partial)* run AT-ALM-01 with a second slot on the same source when time allows | SA | D | finger | |
| A10 | AT-LRW-12 | SA | DR | magnet | |
| A11 | AT-ALM-06 | SA | D | magnet | |
| A14 | AT-ALM-04 (same wave, watch `active=`/LED correlation) | SA | D | wave + LED | |
| C1 | AT-CFG-03, AT-LRW-06 | A | DR | – | |
| C2 | AT-CFG-04 | A | D | – | |
| C3, C6, C7 | AT-LRW-06 | A | DR | – | ✅ |
| C4 | AT-CFG-05, AT-NFC-04 | A/SA | DR | phone | |
| C5 | AT-CFG-02, AT-ADV-04 | A | D | – | |
| C8 | AT-CFG-01 | A | D | – | |
| C9 | AT-CFG-07 | A | D | – | |
| C10 | AT-CFG-03, AT-NFC-07 | A/SA | D | phone | |
| K1 | AT-CLK-01 | A | D | – | |
| K2, K3, K6 | AT-CLK-02 | A/SA | DR | phone (K6), or phone-free §3.6 | K6 ✅ (2026-08-11, phone-free: RTC set from injected unix_time, confirmed ticking forward afterward) |
| K4 | AT-CLK-04 | A | D | – | |
| K5 | AT-CLK-03 | A | D | – | |
| N1 | AT-NFC-03 | SA | DR | phone | ✅ (AT-NFC-02) |
| N2 | *(retired)* superseded by **N9** (#299) — the plaintext dedicated RESET NDEF type it described no longer exists; reset commands now go through the encrypted `hio.stck:cmd` channel (device_reset/factory_reset/set_secret_key), covered by AT-NFC-03's pattern with those payloads | SA | DR | phone | |
| N3, N6 | n/a — features removed by design | – | – | – | |
| N4 | AT-NFC-02 | SA | DR | phone | ✅ |
| N5 | *(partial)* lrw_reset/lrw_join over NFC via `ats cmd nfc` on debug; phone control server has no op for it yet (improvement item) | A | D | – | |
| N7 | *(on-request)* power-off boot-staged provisioning — needs a scripted power-off staging session | SA | DR | phone+power | |
| N8 | AT-NFC-05, AT-NFC-06, AT-ADV-05 | SA | DR | phone (or phone-free frame-crafting, §3.6 — same protocol coverage, not the phone's own codec) | ✅ (2026-08-11, phone-free: retransmission cache, stale-nonce rejection, and anti-brick window all decisive) |
| N9 | *(new, #299)* device_reset/factory_reset/set_secret_key over `hio.stck:cmd` with the ack-before-reboot handshake — all three reboot (set_secret_key since #322, which is what makes the rotated key live) — same AT-NFC-03 injection pattern as N1, plus confirming factory_reset is rejected over a LoRaWAN downlink and an all-zero set_secret_key is rejected | SA | DR | phone for the true ack path; §3.6 phone-free recipe covers decrypt/execute/config-diff decisively but always exercises the field-loss backstop, not `hio.stck:ack`, per its own limitation note | ✅ (2026-08-11, phone-free: all three commands decisive over `hio.stck:cmd`; factory_reset/set_secret_key rejection + device_reset success independently re-confirmed over a real LRW downlink, see AT-LRW-06; all-zero set_secret_key rejected with `BAD_REQUEST,"zero key"` per #322) |
| F1 | AT-LRW-03 | A | DR | – | |
| F2, F3 | AT-HOST-03, AT-HOST-06 | A | host | – | |
| — (new, no manual ID) | AT-BOOT-04/06, AT-CFG-06, AT-HIS-04/05, AT-PWR-01..07, AT-ADV-01..12, AT-SOAK-01..03, AT-NFC-08 | | | | AT-BOOT-04 ✅, AT-PWR-04 ✅ |

**Release smoke set** (run on every release candidate, ~90 min + 60 min unattended):
AT-HOST-01 → flash (consent) → AT-BOOT-04 → AT-LRW-01 → AT-LRW-06 (subset: get_info,
set_param+readback, reboot) → AT-NFC-02 → AT-LRW-10 (60 min, unattended) → AT-PWR-04 (if
PPK2 + J-Link detached).

## 22. Known gotchas appendix (hard-won — read before debugging "failures")

- **rttt needs a pseudo-TTY** headless (`script -qec … /dev/null`); `--reset` is broken —
  never pass it.
- **RTT address moves with LTO** (`0x20000000` LTO vs `0x20000800` legacy). Wrong address =
  garbage/no output, not an error. Verify via `nm`.
- **`send_command` 2 s default timeout** — slow commands (1-Wire!) look empty but ran; use
  `timeout: 6–8` and/or `read_terminal {"lines":300}`.
- **Large shell responses can overflow shell_rtt** (pre-existing) — paginate reads
  (`history read` in chunks) instead of one huge dump.
- **`west flash` post-reset hang** on this board (image lands anyway); **rttt MCP `flash`**
  once produced a non-booting image — the reliable path is JLinkExe `loadfile` (§3.2).
- **Bare JLinkExe `erase` wipes NVS** (identity!) — sector-erase via `loadfile` only.
- **J-Link is exclusive**; stale GUI servers (`JLinkGUIServerE`/AppRun zombies) silently
  hold the probe — "Cannot connect to J-Link" with the probe enumerated ⇒ kill the holder
  PIDs, don't reset USB.
- **Release/PM builds are SWD-unreachable in Stop2** — flash retry loop + power-cycle
  catches the boot window; NRST is not wired.
- **With SWD attached the device can't stay in Shutdown/Stop2** (DBGMCU) — power numbers
  and suspend tests need the probe physically off + a power-cycle.
- **PPK2 output drops the instant its serial handle closes** — persistent holder process;
  never open-set-exit.
- **A killed rttt can leave the CPU halted** → next JLinkExe connect hangs; recover by
  re-flashing via a resetting flasher rather than fighting `connect`.
- **A one-off `JLinkExe -CommanderScript` (flash/readback) while a persistent `rttt` daemon
  holds the same J-Link SN breaks `rttt`'s connection** — next `send_command` returns
  `"J-Link: Unspecified error."` even though a fresh raw JLinkExe connect works fine (target/
  probe are OK, only rttt's session is stale). Fix: kill rttt's two PIDs (the `script` wrapper
  + the python process) **by PID**, not `pkill -f` (see next gotcha), and restart it.
- **`pkill -f` with jlink/rttt/script patterns kills your own shell** (exit 144/1 with no
  output). Kill by PID or `-x`.
- **ChirpStack ABP needs `skip_fcnt_check`**; OTAA DevAddr changes on every rejoin — never
  cache. TTN's "doesn't answer LinkCheckReq" reputation is **stale** — see §3.5's 2026-08-11
  correction; if anything, ChirpStack is now the LNS that churns through OTAA rejoins on this
  bench with stock link-check defaults.
- **PPK2 "cut power" via `set_source_voltage(0)` alone does NOT reliably power off the DUT**
  (source-meter regulation floor / DUT decoupling keeps it alive) — confirmed directly: LED kept
  blinking and the device kept transmitting on LoRaWAN for 90+ s at commanded 0 mV. Only
  `toggle_DUT_power("OFF")` is a real power-cut; the scratchpad `ppk_hold.py` holder previously
  only wired that to its `QUIT` command (which also kills the daemon) — it now also exposes
  live `OFF`/`ON` commands that call `toggle_DUT_power` directly without restarting the holder.
- **Capability-gated peripherals (DS2484 1-Wire bridge, LIS2DH) initialise only at boot** —
  `config cap-… true` + `settings save` (reboot) before they exist.
- **Debug FLASH is ~99 % full** — adding test overlays may need
  `CONFIG_KERNEL_SHELL=n` + `CONFIG_SHELL_HELP=n` trims.
- **First boot log lines drop in the RTT resync window** after a flash-reset — read state
  via `ats device info` instead of hunting the banner.
- **No OTA/DFU exists by design** (flat image, SWD-only updates — see README "Firmware
  update & security model"). Do not "discover" its absence as a finding; improvement ideas
  there must reference the documented trade-off.
- **ABP DevAddr must use a Type-0 NetID prefix (MSB `0`) on this bench's ChirpStack/gateway
  setup** — a Type-1-prefixed address (MSB `10`, e.g. `80D550B0`) is silently never seen by the
  network (device fCnt climbs locally, ChirpStack's `f_cnt_up`/`last_seen_at` stay frozen; no
  error anywhere) while a Type-0 address in the same range as OTAA-assigned addresses (e.g.
  `0081E51D`) joins instantly. Looks exactly like an ABP config bug; isn't — always pick the
  bench's ABP DevAddr from the `0x00xxxxxx`–`0x01xxxxxx` range (found 2026-08-11).
- **Hand-crafted `Command` protobuf frames need varint-encoded tag bytes for oneof fields
  ≥ 16** (tag = `(field<<3)|2` exceeds 127 for field ≥ 16, e.g. `factory_reset`=23 →
  tag=186 needs 2 bytes: `[0xBA, 0x01]`, not `[0xBA]`) — see §3.6 phone-free recipe. A
  single-byte tag for these fields decodes as a truncated stream and the firmware correctly
  rejects it with `Response.Error{BAD_REQUEST,"end-of-stream"}`; this reads exactly like a
  firmware bug until you check the tag encoding of your own test frame (found 2026-08-11).

