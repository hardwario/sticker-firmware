# HARDWARIO STICKER — LoRaWAN Improvements: Regions, Datarate, Diagnostics

The LoRaWAN half of the LoRa improvements survey: region handling (AS923, the AU915 dwell
gap, a build-vs-runtime guard), a manual datarate parameter, confirmed uplinks for alarms,
and link diagnostics in GetInfo. Split out of
`doc/plan/408 - LoRa improvements - P2P hardening and TOWER transport.md` (PR #408), which
keeps the P2P and TOWER work — this work is `app_lrw.c` territory and targets `v1.5.0`
directly, while #408 stacks on `feat-p2p`.

> Requested 2026-08-27: "review the LoRa implementation on TOWER, propose what we could add
> to STICKER (band switching, US915 and AU regulations), and propose improvements to LoRa
> P2P for STICKER." — this PR carries the "band switching, US915 and AU regulations" half.

**Status:** this document is the plan for this PR, which implements the accepted items. The
survey findings, feasibility verification and the TOWER protocol comparison live in the #408
plan; only what this PR acts on is repeated here. Line citations are against `v1.5.0` at
`a05f173` — treat the surrounding identifiers as authoritative and the numbers as a hint.

---

## 1. Context — what STICKER already has

Verified in-tree, to avoid duplicating work:

- **Runtime region selection already exists**: `lrw-region` enum {EU868, US915, AU915},
  applied in `app_lrw_init()` via `lorawan_set_region()` (`app/src/app_lrw.c`).
- **Sub-band selection already exists**: `lrw-sub-band` 0–8 with a hand-built 6-word channel
  mask, `apply_subband()`, gated to US915/AU915 only.
- **US915 is validated end to end** — issue #303, closed 2026-07-28, no firmware defect
  found. Test plan in `doc/us915-test-plan.md`, playbook scenarios AT-LRW-13..15.
- **Downlink RSSI/SNR is already captured and printed** by `ats radio status`.

So "band switching" and "US915 support" are already shipped. The genuinely open work is
enumerated below. The parameter surface is inspired by `twr-sdk`'s `twr_cmwx1zzabz` driver
(`AT$BAND`, `AT$DR`, `AT$REPC`, `AT$RFQ`) — see the #408 plan, §1.2.

## 2. Items

| # | Proposal | Feasibility | Blast radius |
|---|---|---|---|
| A1 | **Build-vs-runtime region guard** | API ready | `app_lrw.c`, ~15 lines |
| A2 | **RSSI/SNR in GetInfo** (like `AT$RFQ`) | API ready | proto + `app_cmd`, 3–4 files |
| A3 | **Manual datarate parameter** (like `AT$DR`) | API ready | yml + configen + `app_lrw.c` |
| A4 | **Confirmed uplinks for alarms** (like `AT$REPC`) | API ready, 2 gotchas | `app_lrw.c` + config param |
| A5 | **AU915 dwell-time compliance** | (a) needs code, (b) validation | `app_lrw.c` + playbook |
| A6 | **AS923 region** | mechanical plumbing | ~5 files, **+2576 B flash** |
| A7 | **RX2 override** (expert knob) | precedent exists | `app_lrw.c` + param |

### A1 — Build-vs-runtime region guard

`lorawan_set_region()` returns `-ENOTSUP` for a region that was not compiled in
(`zephyr/subsys/lorawan/lorawan.c:395-397`; each case is `#if defined(CONFIG_LORAMAC_REGION_x)`
gated). Today the app hard-fails the whole radio init on that error, which means a stored
`lrw-region us915` on a debug image silently produces a device with no radio at all.

This is **not theoretical**: `debug.conf` on v1.5.0 already drops AU915/US915 to reclaim
~5.9 KB flash / ~0.7 KB RAM.

Proposal: gate each `case` on `IS_ENABLED(CONFIG_LORAMAC_REGION_*)` (the pattern is already
used elsewhere in this codebase) and fall back to EU868 with a loud `LOG_ERR` instead of
failing init.

### A2 — RSSI/SNR in GetInfo

The Zephyr downlink callback already carries them —
`struct lorawan_downlink_cb.cb(port, flags, int16_t rssi, int8_t snr, len, data)`
(`zephyr/include/zephyr/lorawan/lorawan.h:198`) — and the values are already stored and
exposed on the shell. The remaining gap is the over-the-air `Info` message: neither the
`Info` proto message nor `app_cmd_get_info()` carries them, so an operator with only a
phone (NFC) or the LNS cannot see link quality.

Caveat to document: the values come from the **last downlink**, which on a Class-A sensor
may be hours old. Consider pairing them with a timestamp or a staleness flag.

### A3 — Manual datarate parameter

`lorawan_set_datarate()` exists (`lorawan.h:382`, impl `lorawan.c:595`) and returns `-EINVAL`
if ADR is enabled (`lorawan.c:600-602`). Hook it in `on_join_success()` immediately after
`lorawan_enable_adr()` and before `refresh_payload_budget()`, so the budget reflects the
manual DR. It re-applies on every rejoin for free.

Precedent: `app_calibration.c` already calls `lorawan_set_datarate(LORAWAN_DR_5)`.

Gotcha: DR validity is region- and dwell-dependent — AU915 with dwell=1 rejects DR0/DR1
(`RegionAU915.c:412-423`), so a config value of 0–1 fails at join with only an `-EINVAL`.
Validate per region, or log loudly.

### A4 — Confirmed uplinks for alarms

Alarms (fPort 3) are currently sent unconfirmed like everything else — an alarm lost to RF
is lost silently. `lorawan_set_conf_msg_tries()` (`lorawan.h:343`) plus
`LORAWAN_MSG_CONFIRMED` in `tx_send_queued()` would fix that; the existing failure path
already requeues with backoff, so a NOACK (`-ETIMEDOUT`) is handled.

**Two gotchas that must be in the implementation plan:**

1. `MIB_CHANNELS_NB_TRANS` also repeats **unconfirmed** uplinks —
   `CheckRetransUnconfirmedUplink()` uses the same limit
   (`modules/lib/loramac-node/src/mac/LoRaMac.c:3614-3618`). Setting tries globally would
   triple *telemetry* airtime too. Use a set → send → restore sequence around the confirmed
   send (`lorawan_send()` is synchronous). A `LinkADRReq` can also overwrite NbTrans
   (`LoRaMac.c:2262`).
2. A confirmed send blocks `m_work_q` for the whole retry sequence, and the watchdog
   heartbeat runs on the same queue with a 30 s timeout. Verify the worst-case NbTrans
   duration at DR0 before shipping.

### A5 — AU915 dwell time ("AU regulations")

Investigated in detail, and the conclusion corrects a common assumption: **dwell time is
already fully MAC-enforced and needs no app changes for telemetry.**

- `AU915_DEFAULT_UPLINK_DWELL_TIME = 1` (`RegionAU915.h:111`), dwell-limited minimum
  datarate `AU915_DWELL_LIMIT_DATARATE = DR_2` (`RegionAU915.h:81`).
- The Dwell1 payload table (`RegionAU915.h:256`) gives DR0/DR1 = 0 B, **DR2 = 11 B**,
  DR3 = 53, DR4 = 125, DR5/6 = 242.
- That table already flows into the app: `GetPhyParam(PHY_MAX_PAYLOAD)` selects it from
  `UplinkDwellTime` (`RegionAU915.c:153-159`) → `LoRaMacQueryTxPossible` →
  `lorawan_get_payload_sizes()` (`lorawan.c:618-628`) → `refresh_payload_budget()` →
  compose. `PHY_MIN_TX_DR` is dwell-aware too, so the ADR floor and
  `lorawan_set_datarate()` validation both respect DR2.

**The real gap** is elsewhere: `tx_send_queued()` **drops** over-budget response and alarm
frames instead of fragmenting them. At an 11 B DR2 budget, the GetInfo-on-join frame and
most alarm frames are near-guaranteed drops. That is the actual AU915 work item — and it is
shared with AS923, which also defaults to dwell=1.

Second item: HIL validation for AU915 mirroring what #303 did for US915 (playbook scenarios,
a gateway on an AU915 plan).

### A6 — AS923 region

Feasible and cheap. **Measured**, not estimated — two release builds of `sticker-v150/app`
(LTO, same tree, only the Kconfig differs):

| Build | FLASH | RAM |
|---|---|---|
| Baseline (EU868 + US915 + AU915) | 153 844 B (72.23 % of 208 KB) | 52 620 B (80.29 %) |
| `+ CONFIG_LORAMAC_REGION_AS923=y` | 156 420 B (73.44 %) | 52 620 B (80.29 %) |
| **Delta** | **+2 576 B (+1.21 pp)** | **+0 B** |

RAM delta is zero because loramac-node's channel structures are already sized for the
largest enabled region (US915 = 72 channels; AS923 has at most 16). The figure is the pure
compile-time region cost — app plumbing (enum value, switch case, decoder map) would add a
few hundred bytes more.

What is needed: the `lrw-region` enum value, the `app_lrw.c` switch case, `prj.conf`, and the
`_LRW_ENUM.region` map in `app/decoder/ttn.js` (hand-maintained). The Zephyr Kconfig
(`zephyr/subsys/lorawan/Kconfig:47`), CMake sourcing, and the `lorawan_set_region()` case
already exist.

Notes:

- The **channel-mask concern does not apply** — `apply_subband()` is gated to US915/AU915,
  and AS923 uses a size-1 mask it never touches.
- The **channel plan group** (AS923-1/2/3/4) is compile-time only in loramac-node
  (`RegionAS923.c:54-55`), injectable via
  `zephyr_compile_definitions(REGION_AS923_DEFAULT_CHANNEL_PLAN=…)`. One group per build —
  build variants, not a runtime setting.
- AS923 also defaults to dwell=1, so the A5 fragmentation gap applies here too.

### A7 — RX2 override

No Zephyr API exists, but `app_lrw.c` already includes `<LoRaMac.h>` and calls
`LoRaMacMibSetRequestConfirm` directly for the RX delays and public-network flag — exact
precedent. `MIB_RX2_CHANNEL` (and `MIB_RX2_DEFAULT_CHANNEL`, so a MAC reset does not revert
it) would work the same way, re-applied after join because JoinAccept DLSettings and
`RXParamSetupReq` both overwrite RX2.

Low priority, and it should stay a debug/expert knob: an override that does not match the
LNS silently kills all downlinks.

## 3. Tracking

- [ ] A1 — build-vs-runtime region guard
- [ ] A6 — AS923 region (+2 576 B flash, +0 B RAM)
- [ ] A3 — manual datarate parameter
- [ ] A2 — RSSI/SNR in GetInfo
- [ ] A5a — fragment instead of dropping over-budget response/alarm frames
- [ ] A4 — confirmed uplinks for alarms
- [ ] A5b — AU915 HIL validation (needs an AU915-plan gateway)

**Deferred / v2:** A7 (RX2 override).

## 4. Implementation steps

One step per commit, in order. Standard verification for every step, unless noted: Release
and `debug.conf` builds, `bash tests/run_native.sh`, `clang-format --dry-run --Werror`; when
`app_config.yml` or the proto changes, also
`pytest sticker/scripts/west_commands/tests` and `cd app/decoder && node --test`.

### Step 1 — A1: region guard

Gate each `lorawan_set_region()` case in `app_lrw_init()` on
`IS_ENABLED(CONFIG_LORAMAC_REGION_*)`; on a stored region that is not compiled in, fall back
to EU868 with a loud `LOG_ERR` instead of failing radio init. ~15 lines, `app_lrw.c` only.

**Verify:** standard; bench check on `debug.conf` (which drops US915/AU915) — set
`lrw-region us915`, reboot, expect the fallback log and a working EU868 radio, not a dead
one.

### Step 2 — A6: AS923 region

Rides on Step 1 (the guard makes a non-compiled AS923 safe on trimmed images).

- `app_config.yml`: `AS923` enum value; regenerate via local configen (never hand-edit
  `app_config.c` — it is generated).
- `app_lrw.c`: the switch case (`IS_ENABLED`-gated per Step 1); AS923 takes no sub-band.
- `prj.conf`: `CONFIG_LORAMAC_REGION_AS923=y` (release; decide whether debug keeps it —
  debug already trims the 915-family for RAM/flash headroom).
- App CMake: `REGION_AS923_DEFAULT_CHANNEL_PLAN` compile definition, default AS923-1.
- `app/decoder/ttn.js`: extend `_LRW_ENUM.region` + tests.

**Verify:** standard + configen pytest + decoder tests; flash/RAM delta recorded in the
commit message (expected ≈ +2.6 KB / +0 B against the table above).

### Step 3 — A3: manual datarate parameter

- New `lrw-datarate` param (int, sentinel for "let ADR/stack choose", persisted, shell+NFC
  writable — same access model as the other `lrw_*` params) in `app_config.yml` +
  regenerated artefacts + decoder map.
- Apply in `on_join_success()` after `lorawan_enable_adr()`, before
  `refresh_payload_budget()`; skip (with a log) when ADR is on; log loudly on `-EINVAL`
  (region/dwell-invalid DR).

**Verify:** standard + configen pytest + decoder tests; bench check: ADR off + DR pinned,
confirm uplink DR on the LNS; ADR on + param set, confirm the skip log.

### Step 4 — A2: RSSI/SNR in GetInfo

- `Info` proto message: `last_rssi` / `last_snr` (+ staleness: either a
  `last_downlink_age_s` field or documenting "last downlink" semantics — decide during
  implementation).
- `app_cmd_get_info()` fills them from the values `app_lrw` already tracks; `ttn.js` Info
  decode + tests; extend the `tests/cmd` build_info ztest cases.

**Verify:** standard + configen/proto pytest + decoder tests + `tests/cmd` suite.

### Step 5 — A5a: fragment instead of dropping

`tx_send_queued()` currently drops a queued response/alarm frame larger than the current
payload budget. Split it across multiple uplinks instead (the compose/history paths already
have multi-frame precedent). Needs a small design decision on framing for split responses —
reuse the existing multi-frame pattern rather than inventing a new one.

**Verify:** standard + a `tests/compose`-style ztest case at an 11 B budget; bench check on
US915 DR0 (same 11 B budget as AU915 DR2, no AU gateway needed).

### Step 6 — A4: confirmed alarms

- New config param (e.g. `lrw-alarm-confirmed`, bool, default off) — alarms opt into
  `LORAWAN_MSG_CONFIRMED`.
- Set → send → restore `MIB_CHANNELS_NB_TRANS` around the confirmed send so telemetry
  airtime is untouched; measure worst-case duration at DR0 against the 30 s work-queue
  watchdog before enabling by default anywhere.

**Verify:** standard + configen pytest; bench check with a gateway ACKing fPort 3, plus a
forced-NOACK run (gateway down) confirming the requeue path and no watchdog trip.

### Step 7 — A5b: AU915 HIL validation

Playbook scenarios mirroring AT-LRW-13..15 for AU915 (join on a sub-band, dwell-limited
DR floor = DR2, 11 B budget multi-frame split, fragment-not-drop from Step 5). Blocked on
bench hardware: needs a gateway on an AU915 frequency plan.

## 5. Explicit non-goals

- **Class C / multicast** — STICKER is a battery device and Class A only is a deliberate
  choice; LTO strips the Class B/C code paths today.
- **Runtime AS923 channel-plan switching** — loramac-node fixes the plan group at compile
  time; build variants cover the need.
- **P2P and TOWER work** — lives in PR #408 (`feat-p2p`).

## 6. References

- `doc/plan/408 - LoRa improvements - P2P hardening and TOWER transport.md` — the full
  survey (TOWER stack, twr-sdk), the feasibility matrix, and the P2P/TOWER tracks.
- `hardwario/twr-sdk` @ `9ded554` — `twr/src/twr_cmwx1zzabz.c`, `twr/src/twr_at_lora.c`.
- Issue #303 — US915 end-to-end validation (closed, no firmware defect found).
- `doc/us915-test-plan.md`, `doc/automated-test-playbook.md` (AT-LRW-13..15).
