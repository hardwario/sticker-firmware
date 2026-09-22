# HARDWARIO STICKER — LoRaWAN Improvements: Regions, Datarate, Diagnostics

The LoRaWAN half of the LoRa improvements survey: region handling (AS923, the AU915 dwell
gap, a build-vs-runtime guard), a manual datarate parameter, confirmed uplinks for alarms,
and link diagnostics in GetInfo. Split out of PR #408, whose plan now lives at
`doc/plan/408 - LoRa improvements - P2P hardening.md` **on `feat-p2p` only** (not yet on
`v1.5.0`) — this work is `app_lrw.c` territory and targets `v1.5.0` directly. The TOWER
transport moved to PR #410.

> Requested 2026-08-27: "review the LoRa implementation on TOWER, propose what we could add
> to STICKER (band switching, US915 and AU regulations), and propose improvements to LoRa
> P2P for STICKER." — this PR carries the "band switching, US915 and AU regulations" half.

**Status:** this document is the plan for this PR, which implements the accepted items. The
survey findings, feasibility verification and the TOWER protocol comparison live in the #408
plan; only what this PR acts on is repeated here. Line citations are against `v1.5.0` at
`a05f173` — treat the surrounding identifiers as authoritative and the numbers as a hint.

**Revision 2026-09-22** (code review of this plan against `v1.5.0`): A1 no longer falls back
to EU868 (regulatory risk — it goes radio-silent instead); A5a reframed — responses are
already budget-capped, the real gap is that `Info` cannot be split, which needs a proto
design; A2 now waits for #414 and weighs the `Info` size budget; A6 reordered after A5a
(dwell=1 region is not usable without it); A4 gained the duty-cycle vs 30 s heartbeat
conflict as a blocker. Steps in §4 renumbered accordingly.

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
| A1 | **Build-vs-runtime region guard** | API ready | `app_lrw.c` + a `device_status` bit, ~25 lines |
| A2 | **RSSI/SNR in GetInfo** (like `AT$RFQ`) | API ready, `Info` size budget | proto + `app_cmd`, 3–4 files; after #414 |
| A3 | **Manual datarate parameter** (like `AT$DR`) | API ready | yml + configen + `app_lrw.c` |
| A4 | **Confirmed uplinks for alarms** (like `AT$REPC`) | API ready, blocker: 30 s heartbeat | `app_lrw.c` + config param |
| A5 | **AU915 dwell-time compliance** | (a) needs proto design, (b) validation | proto + `app_cmd` + `app_lrw.c` + decoder + playbook |
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
used elsewhere in this codebase). On a stored region that is not compiled in, **do not fall
back to another region** — enter the existing radio-silent `APP_LRW_STATE_DISABLED` path
(the same one an all-zero DevEUI uses, #98), log a loud `LOG_ERR`, and raise a dedicated
`device_status` bit so the fault is visible over NFC and in the Manager-App.

Why not "fall back to EU868": a device configured for US915/AU915 would then transmit on
868 MHz in the Americas or Australia — outside the permitted ISM band. A dead-but-diagnosable
radio is the safe failure; a wrong-band radio is a compliance violation. The operator fixes
it by reflashing a full image or changing `lrw-region` over NFC/shell.

Note: #414 regroups the `device_status` bit layout — allocate the new bit against the
layout that lands first.

### A2 — RSSI/SNR in GetInfo

The Zephyr downlink callback already carries them —
`struct lorawan_downlink_cb.cb(port, flags, int16_t rssi, int8_t snr, len, data)`
(`zephyr/include/zephyr/lorawan/lorawan.h:198`) — and the values are already stored and
exposed on the shell. The remaining gap is the over-the-air `Info` message: neither the
`Info` proto message nor `app_cmd_get_info()` carries them, so an operator with only a
phone (NFC) or the LNS cannot see link quality.

Caveat to document: the values come from the **last downlink**, which on a Class-A sensor
may be hours old. Consider pairing them with a timestamp or a staleness flag.

**Size budget.** `Info` is already at the edge of the EU868 DR0 budget (51 B): `claim_token`
was moved to NFC-only for exactly that reason (see the comment on `Info.claim_token` in
`app_config.proto`), and `app_cmd_build_info()` trims `active_alarms` to fit. RSSI + SNR +
an age field add roughly 6–8 B of protobuf and would eat into the alarm list at DR0. Two
options, decide in Step 5:

- **NFC-only** (like `lrw_state` / `dev_eui`) — zero LoRaWAN cost; the LNS already has
  per-frame gateway RSSI/SNR for the uplink direction anyway.
- **Both transports** — only after A5a gives `Info` a way to split.

**Dependency.** #414 (mailbox, `get_basic_info`, `device_status` regroup) and #413 (boot
settings-info) both change `app_config.proto`, `app_cmd.c` and the `Info` build path. Do A2
after #414 lands to avoid a three-way conflict on the same message.

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
2. **Blocker, not a check:** a confirmed send blocks `m_work_q` for the whole retry
   sequence (`lorawan_send()` is synchronous), and the #182 liveness heartbeat runs on the
   same queue with `LRW_HEARTBEAT_TIMEOUT_MS = 30000`. At EU868 DR0 one frame is ~1.5 s ToA;
   the 1 % duty cycle then holds the sub-band for ~150 s, so any retry beyond the first
   (unless the MAC finds a free sub-band) blows the 30 s limit by a wide margin. Resolve
   before implementation — options: (a) cap NbTrans for alarms to what fits the heartbeat
   at the current DR, (b) suspend/extend the heartbeat around a confirmed send, or (c) send
   confirmed alarms without blocking the queue. A longer blocking window also widens the
   exposure to the known `lorawan_send()` `K_FOREVER` semaphore hang (see CLAUDE.md).

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

**The real gap** is elsewhere — and it is narrower than "`tx_send_queued()` drops", which is
only half the story:

- **Command responses are already budget-capped.** `queue_info_uplink()` and the downlink
  handler both pass `min(budget, buffer)` into the encoder, and `app_cmd_build_info()` trims
  `active_alarms` to fit — so an over-budget response does not reach `tx_send_queued()`, it
  **fails at encode time** and nothing is queued. At an 11 B DR2 budget even the fixed part
  of `Info` (fw version, serial, uptime, unix time, battery, reset cause, device status +
  envelope) does not fit, so GetInfo-on-join and an explicit GetInfo are lost.
- **Alarm frames** are not budget-capped at encode time; they hit the drop in
  `tx_send_queued()`.
- **`Info` has no way to split.** Unlike `HistoryFrame` (`page_index` / `page_count`) it
  carries no paging fields, so "reuse the existing multi-frame pattern" is not free: it needs
  a proto change (paging fields or a split into smaller messages), matching encoder logic in
  `app_cmd`, and reassembly in `ttn.js` / the LNS.

That is the actual AU915 work item and the largest item in this PR — and it is shared with
AS923, which also defaults to dwell=1.

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
- AS923 also defaults to dwell=1, so the A5 fragmentation gap applies here too. **Ship A6
  after A5a** — before it, AS923 at DR2 (11 B) loses GetInfo-on-join and most alarms, i.e.
  the region would be nominally supported but not usable.
- AS923-1 JP channels require LBT; loramac-node handles it per channel plan — confirm on
  the bench if a JP deployment is ever targeted.

### A7 — RX2 override

No Zephyr API exists, but `app_lrw.c` already includes `<LoRaMac.h>` and calls
`LoRaMacMibSetRequestConfirm` directly for the RX delays and public-network flag — exact
precedent. `MIB_RX2_CHANNEL` (and `MIB_RX2_DEFAULT_CHANNEL`, so a MAC reset does not revert
it) would work the same way, re-applied after join because JoinAccept DLSettings and
`RXParamSetupReq` both overwrite RX2.

Low priority, and it should stay a debug/expert knob: an override that does not match the
LNS silently kills all downlinks.

## 3. Tracking

- [ ] A1 — build-vs-runtime region guard (radio-silent, no region fallback)
- [ ] A3 — manual datarate parameter
- [ ] A5a — splittable `Info` / fragment instead of dropping over-budget frames
- [ ] A6 — AS923 region (+2 576 B flash, +0 B RAM) — after A5a
- [ ] A2 — RSSI/SNR in GetInfo — after #414
- [ ] A4 — confirmed uplinks for alarms — heartbeat blocker resolved first
- [ ] A5b — AU915 HIL validation (needs an AU915-plan gateway)

**Deferred / v2:** A7 (RX2 override).

## 4. Implementation steps

One step per commit, in order. Standard verification for every step, unless noted: Release
and `debug.conf` builds, `bash tests/run_native.sh`, `clang-format --dry-run --Werror`; when
`app_config.yml` or the proto changes, also
`pytest sticker/scripts/west_commands/tests` and `cd app/decoder && node --test`.

Steps 1–2 have no dependency on other open PRs and can start immediately.

### Step 1 — A1: region guard

Gate each `lorawan_set_region()` case in `app_lrw_init()` on
`IS_ENABLED(CONFIG_LORAMAC_REGION_*)`. On a stored region that is not compiled in, enter
`APP_LRW_STATE_DISABLED` (radio-silent, like #98) with a loud `LOG_ERR` and a new
`device_status` bit — **never** substitute another region. `app_lrw.c` + `app_cmd.h` bit +
decoder bit name.

**Verify:** standard + decoder tests; bench check on `debug.conf` (which drops US915/AU915) —
set `lrw-region us915`, reboot, expect the error log, `DISABLED` state, the status bit over
NFC, and **no RF emission**; setting `lrw-region eu868` restores a working radio.

### Step 2 — A3: manual datarate parameter

- New `lrw-datarate` param (int, sentinel for "let ADR/stack choose", persisted, shell+NFC
  writable — same access model as the other `lrw_*` params) in `app_config.yml` +
  regenerated artefacts + decoder map.
- Apply in `on_join_success()` after `lorawan_enable_adr()`, before
  `refresh_payload_budget()`; skip (with a log) when ADR is on; log loudly on `-EINVAL`
  (region/dwell-invalid DR). Calibration mode's own `LORAWAN_DR_5` stays authoritative
  while calibrating.

**Verify:** standard + configen pytest + decoder tests; bench check: ADR off + DR pinned,
confirm uplink DR on the LNS; ADR on + param set, confirm the skip log.

### Step 3 — A5a: splittable `Info` + alarm fragmentation

Design first, then code — this is the largest step:

- **Design decision** (record in this plan before coding): how `Info` splits — paging
  fields (`page_index` / `page_count`, `HistoryFrame` precedent) vs. splitting into smaller
  self-contained messages. Constraint: the first frame alone must identify the device and
  firmware at an 11 B budget. Coordinate the wire change with apps/manager and the LNS
  decoder.
- `app_cmd_build_info()` / `queue_info_uplink()` emit N frames when the budget is small
  instead of failing at encode time.
- Alarm frames: budget-aware encode or split instead of the drop in `tx_send_queued()`.
- `ttn.js` reassembly + tests.

**Verify:** standard + proto pytest + decoder tests + a `tests/cmd`/`tests/compose`-style
ztest case at an 11 B budget; bench check on US915 DR0 (same 11 B budget as AU915 DR2, no
AU gateway needed).

### Step 4 — A6: AS923 region

Rides on Step 1 (the guard makes a non-compiled AS923 safe on trimmed images) and Step 3
(dwell=1 budget).

- `app_config.yml`: `AS923` enum value; regenerate via local configen (never hand-edit
  `app_config.c` — it is generated).
- `app_lrw.c`: the switch case (`IS_ENABLED`-gated per Step 1); AS923 takes no sub-band.
- `prj.conf`: `CONFIG_LORAMAC_REGION_AS923=y` (release; decide whether debug keeps it —
  debug already trims the 915-family for RAM/flash headroom).
- App CMake: `REGION_AS923_DEFAULT_CHANNEL_PLAN` compile definition, default AS923-1.
- `app/decoder/ttn.js`: extend `_LRW_ENUM.region` + tests.

**Verify:** standard + configen pytest + decoder tests; flash/RAM delta recorded in the
commit message (expected ≈ +2.6 KB / +0 B against the table above — re-measure, the
baseline has moved since).

### Step 5 — A2: RSSI/SNR in GetInfo

Starts after #414 is merged into `v1.5.0` (and rebased over #413 if that lands first).

- Decide transport: NFC-only (recommended unless Step 3 made `Info` splittable cheaply) vs.
  both.
- `Info` proto message: `last_rssi` / `last_snr` + staleness (`last_downlink_age_s` or
  documented "last downlink" semantics).
- `app_cmd_get_info()` fills them from the values `app_lrw` already tracks; `ttn.js` Info
  decode + tests; extend the `tests/cmd` build_info ztest cases, including one asserting
  the DR0 budget still fits.

**Verify:** standard + configen/proto pytest + decoder tests + `tests/cmd` suite.

### Step 6 — A4: confirmed alarms

**Precondition:** pick and document the heartbeat resolution from §2 A4 gotcha 2.

- New config param (e.g. `lrw-alarm-confirmed`, bool, default off) — alarms opt into
  `LORAWAN_MSG_CONFIRMED`.
- Set → send → restore `MIB_CHANNELS_NB_TRANS` around the confirmed send so telemetry
  airtime is untouched; NbTrans bounded per the chosen heartbeat resolution.

**Verify:** standard + configen pytest; bench check with a gateway ACKing fPort 3, plus a
forced-NOACK run (gateway down) at EU868 DR0 confirming the requeue path and **no heartbeat
trip / no reboot**.

### Step 7 — A5b: AU915 HIL validation

Playbook scenarios mirroring AT-LRW-13..15 for AU915 (join on a sub-band, dwell-limited
DR floor = DR2, 11 B budget, split `Info` and alarms from Step 3). Blocked on bench
hardware: needs a gateway on an AU915 frequency plan.

## 5. Explicit non-goals

- **Class C / multicast** — STICKER is a battery device and Class A only is a deliberate
  choice; LTO strips the Class B/C code paths today.
- **Runtime AS923 channel-plan switching** — loramac-node fixes the plan group at compile
  time; build variants cover the need.
- **P2P and TOWER work** — PR #408 (merged into `feat-p2p`) and PR #410.

## 6. References

- `doc/plan/408 - LoRa improvements - P2P hardening.md` (on `feat-p2p`) — the full survey
  (TOWER stack, twr-sdk), the feasibility matrix, and the P2P track; TOWER in PR #410.
- `hardwario/twr-sdk` @ `9ded554` — `twr/src/twr_cmwx1zzabz.c`, `twr/src/twr_at_lora.c`.
- Issue #303 — US915 end-to-end validation (closed, no firmware defect found).
- `doc/us915-test-plan.md`, `doc/automated-test-playbook.md` (AT-LRW-13..15).
