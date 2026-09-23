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
`a05f173` (the A5 audit below at `0dd8cb4`, after #413 merged) — treat the surrounding
identifiers as authoritative and the numbers as a hint.

**Revision 2026-09-22** (code review of this plan against `v1.5.0`): A1 no longer falls back
to EU868 (regulatory risk — it goes radio-silent instead); A5a reframed — responses are
already budget-capped, the real gap is that `Info` cannot be split, which needs a proto
design; A2 now waits for #414 and weighs the `Info` size budget; A6 reordered after A5a
(dwell=1 region is not usable without it); A4 gained the duty-cycle vs 30 s heartbeat
conflict as a blocker. Steps in §4 renumbered accordingly.

**Revision 2 2026-09-22** — A5 audit of **every** uplink type against every DR/region
budget (sizes measured by encoding real messages through `app_config.proto`, not
estimated). Corrects revision 1: alarms **are** budget-capped at encode time (they trim
events, they are not dropped in `tx_send_queued()`). A5a widened from "splittable `Info`"
to a general split rule for fPort 85 and fPort 3, with sub-steps 3a–3g.

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
| A1 | **Build-vs-runtime region guard** | API ready | `app_lrw.c`, ~40 lines |
| A2 | **RSSI/SNR in GetInfo** (like `AT$RFQ`) | API ready, `Info` size budget | proto + `app_cmd`, 3–4 files; after #414 |
| A3 | **Manual datarate parameter** (like `AT$DR`) | API ready | yml + configen + `app_lrw.c` |
| A4 | **Confirmed uplinks for alarms** (like `AT$REPC`) | API ready, blocker: 30 s heartbeat | `app_lrw.c` + config param |
| A5 | **AU915 dwell-time compliance** | (a) general split rule — proto design, (b) validation | proto + `app_cmd` + `app_alarm` + `app_lrw.c` + decoder + playbook |
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
(the same one an all-zero DevEUI uses, #98) and log a loud `LOG_ERR`. Over NFC the device
then reports `lrw_state` DISABLED and the existing `device_status` bit `lrw_disabled`; no
dedicated bit (decided 2026-09-23 — the cause is visible in the log and in `config show`).

Why not "fall back to EU868": a device configured for US915/AU915 would then transmit on
868 MHz in the Americas or Australia — outside the permitted ISM band. A dead-but-diagnosable
radio is the safe failure; a wrong-band radio is a compliance violation. The operator fixes
it by reflashing a full image or changing `lrw-region` over NFC/shell.

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

**Dependency.** #414 (mailbox, `get_basic_info`, `device_status` regroup) changes
`app_config.proto`, `app_cmd.c` and the `Info` build path. Do A2 after #414 lands to avoid
a conflict on the same message. (#413, boot settings-info, touched the same files and has
since merged into `v1.5.0`.)

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

**The real gap** is elsewhere: most uplink types other than telemetry cannot shrink to a
small budget. Audit of every uplink type on `v1.5.0` @ `0dd8cb4`:

**Budget tiers** (loramac-node `MaxPayloadOfDatarate*`, application payload N; pending MAC
answers in FOpts shrink it further):

| Tier | Where |
|---|---|
| **11 B** | US915 DR0; AU915 DR2 and AS923 DR2 (dwell=1 floor — DR0/DR1 are 0 B) |
| **51 B** | EU868 DR0–2 (also KR920/IN865 DR0–2, not compiled) |
| **≥ 53 B** | US915 DR1+, AU915/AS923 DR3+, EU868 DR3+ |

**Per message type** (sizes include the 1-byte `APP_PROTO_VERSION` prefix):

| Message | Mechanism today | 11 B | 51 B | ≥ 53 B |
|---|---|---|---|---|
| Telemetry, fPort 2 | **splits** by sensor group, each frame self-contained | ✅ except a single group larger than the budget (machine-probe reading 29 B) — dropped (M-10) | ✅ | ✅ |
| HistoryFrame replay | **splits** by budget | ❌ worst-case varint overhead 33 B → sample cap 0 → replay stops at frame 0 | ✅ ~18 B samples/frame | ✅ |
| Info (join, clock-sync, GetInfo) | **trims** `active_alarms`, cannot split | ❌ min 13 B (fw + serial), typical 30 B | ✅ 30–46 B, ~2 alarms | ✅ |
| settings-info ConfigDump (#413) | fixed single page, 40 B | ❌ skipped with a log | ✅ | ✅ |
| GetConfig | pages of fixed `DUMP_PAGE_BUDGET = 30` → ~32–44 B frames | ❌ every page | ✅ | ✅ but not DR-adaptive (same ~page count at 242 B) |
| GetParam | no paging | ⚠️ one small field (10 B) | ⚠️ ~2 alarm rules | ⚠️ 64 B cap |
| W1Scan | no paging, 10 B per ROM | ❌ 1 ROM = 15 B | ⚠️ ~3 ROMs | ⚠️ 64 B cap |
| AlarmReport, fPort 3 | **trims** events (`total` keeps the true count), cannot split | ❌ 1 event = 15–26 B → alarm lost | ⚠️ 2 events | ⚠️ ~3 events (64 B cap) |
| Error | `detail` string, 15–30 B | ❌ (code-only would be 7 B) | ✅ | ✅ |
| Ack | 5–9 B | ✅ | ✅ | ✅ |

Cross-cutting defects found by the audit:

1. **Silence at 11 B.** When a response does not fit, `app_cmd_handle()` substitutes
   `Error "response too large"` — 25 B, which does not fit either, so a downlink command gets
   **no answer at all**.
2. **64 B caps independent of DR.** `APP_LRW_RESPONSE_BUF_SIZE` and `ALARM_FRAME_MAX` are
   64 B. The alarm batch holds `ALARM_BATCH_MAX = 8` events but at most ~3 fit one frame even
   at 242 B; the rest are counted in `total` and never sent.
3. **Budget 0 read as "unlimited".** Every encoder uses `if (budget > 0 && budget < cap)`,
   so during a MAC-command flood (budget 0) it encodes at full size and
   `tx_send_queued()` then drops the frame.
4. **Encode-time vs send-time budget.** Responses and alarms are encoded against the budget
   at queue time; if ADR lowers the DR before the send, `tx_send_queued()` drops the bytes.
   Telemetry does not have this problem — it recomposes per frame.
5. **Calibration forces `LORAWAN_DR_5`**, which does not exist on US915 / AU915-dwell1. The
   factory flow is EU868-only, so this is a note, not a work item.

So the 11 B tier (every non-EU region at its lowest DR) can reliably deliver only telemetry
and Acks. That is the AU915 work item and the largest item in this PR — shared with US915
DR0 and with AS923.

**Constraint that shapes the design:** LNS payload formatters (TTN `ttn.js`, ChirpStack
codecs) are **stateless per uplink**. Reassembling a byte stream across uplinks in the
decoder is not possible there. Hence the rule the telemetry composer and the history replay
already follow: **every frame is a complete, independently decodable message**, and a large
logical message becomes N smaller messages, not N fragments of one.

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
  after A5a** — before it, AS923 at DR2 (11 B) loses GetInfo-on-join, settings-info, every
  alarm and every command response except Ack, i.e. the region would be nominally supported
  but not usable.
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

- [x] A1 — build-vs-runtime region guard (radio-silent, no region fallback);
  **HW PASS 2026-09-23** (AT-LRW-19). The dedicated `device_status` bit 13 was dropped
  afterwards (not needed — `lrw_disabled` + the log suffice)
- [x] A3 — manual datarate parameter (`lrw-datarate` auto|dr0-dr7, enum, proto_id 16) —
  **HW PASS 2026-09-23** (AT-LRW-20 a+b; the per-region reject path (c) needs a 915 gateway)
- [ ] A5a — general split rule for fPort 85 / fPort 3 (see Step 3); closes #418
  - [x] 3a — shared budget helper (0 = defer, not unlimited) + compact LoRaWAN `Error` (`05e4a5f`)
  - [x] 3b — alarms: N `AlarmReport` frames instead of trimming; alarm bits in telemetry (`3b0a97d`)
  - [x] 3c — `Info`-lite at the 11 B tier + deferred boot announce; `BUDGET_TOO_SMALL` (`6cbb4db`;
    `InfoLite` = `Response` field 11, since #414 claims 9/10)
  - [ ] 3d — settings-info + GetConfig: DR-adaptive pages (≥ 51 B tier, low priority)
  - [ ] 3e — GetParam / W1Scan: paging (≥ 51 B tier, low priority)
  - [x] 3f — HistoryFrame: real (not worst-case) overhead, defined floor (`790b8f9`)
  - [x] 3g — DR drop between queue and send: recover by frame kind (`c77f5ac`; narrowed — full
    send-time re-encode would need a whole `Response` per queue slot, no RAM for it)
- [x] A6 — AS923 region — measured +2 536 B flash / +0 B RAM (release, on the synced base);
  AS923-1, release only (debug trims it); not HW-testable on the EU868 bench
- [ ] A2 — RSSI/SNR in GetInfo — after #414
- [ ] A4 — confirmed uplinks for alarms — heartbeat blocker resolved first
- [ ] A5b — AU915 HIL validation (needs an AU915-plan gateway)

**Deferred / v2:** A7 (RX2 override).

**Merge note for `feat-p2p`:** 3f renamed `app_lrw_start_history_replay()` (bool) to
`app_lrw_history_replay_start()` (int, 0 = started). `feat-p2p`'s `app_cmd.c` still calls the old
name next to `app_p2p_start_history_replay()` — the rename makes that a compile error at the
v1.5.0 ↔ feat-p2p merge instead of a silent true/false inversion; resolve by mapping
`-EMSGSIZE` to `BUDGET_TOO_SMALL` there too.

**Also in this PR:** #419 — `DevStatusReq` after `LinkADRReq` (loramac-node `west patch`,
`6d6215f`); **HW PASS 2026-09-23**.

**HIL record 2026-09-23** (debug build of this branch at `a75a468` + #419 patch, STICKER DevEUI
`5876070000000413` via J-Link 822005110, EU868 on the ProXimos Hub ChirpStack v4, LoRaWAN 1.0.4,
frames read from the ChirpStack device-frame stream):

| Item | Result |
|---|---|
| Boot regression | join → Info (24 B) → settings-info (34 B) → telemetry, FIFO order; LinkCheck OK |
| #419 | `DeviceTimeAns + LinkADRReq + DevStatusReq` → next uplink `LinkADRAns + DevStatusAns{battery 192, margin 6}`; ChirpStack `device_status` populated for the first time. Also `DeviceTimeAns + DevStatusReq` → answered |
| A3 | ADR off + `dr3` → every uplink after join at DR3/SF9; ADR on + `dr3` → `lrw-datarate DR3 ignored: ADR is on`, NS drives the DR |
| 3b alarm bits | telemetry `alarm_status` 3 = `alarm_any` + `alarm_threshold` while rules were active, 0 after clearing |
| 3b split | a 4-event batch at DR0 (51 B) → two fPort 3 frames (3 + 1 events, 42 B + 24 B), same `base_time` / `total` |
| A1 | `lrw-region us915` on `debug.conf` (US915/AU915 not compiled) → `lrw-region 1 is not compiled into this image: radio-silent`, state `DISABLED`, `device_status` 0x3000 (bit 13 since removed), no JoinRequest after boot |
| 3a compact Error | downlink `set_param interval_report=10` → 10 B `Error{OUT_OF_RANGE, fault 203}`, no detail; downlink `get_info` → full Info, 26 B at DR0 |

Not testable on this EU868 bench (smallest budget 51 B): the 11 B tier paths — `InfoLite`,
`BUDGET_TOO_SMALL`, history-replay floor, 3g recovery after a DR drop. Covered by native tests only;
needs a US915 / AU915 gateway (Step 7).

Found during the run: `west flash` rebuilds a stale build dir before flashing, which bypassed the
configure-time patch check and flashed an unpatched `LoRaMac.c` — fixed by checking on every build
(`0d2b85a`).

## 4. Implementation steps

One step per commit, in order. Standard verification for every step, unless noted: Release
and `debug.conf` builds, `bash tests/run_native.sh`, `clang-format --dry-run --Werror`; when
`app_config.yml` or the proto changes, also
`pytest sticker/scripts/west_commands/tests` and `cd app/decoder && node --test`.

Steps 1–2 have no dependency on other open PRs and can start immediately.

### Step 1 — A1: region guard

Gate each `lorawan_set_region()` case in `app_lrw_init()` on
`IS_ENABLED(CONFIG_LORAMAC_REGION_*)`. On a stored region that is not compiled in, enter
`APP_LRW_STATE_DISABLED` (radio-silent, like #98) with a loud `LOG_ERR` — **never**
substitute another region. `app_lrw.c` only.

**Verify:** standard + decoder tests; bench check on `debug.conf` (which drops US915/AU915) —
set `lrw-region us915`, reboot, expect the error log, `DISABLED` state (`lrw_disabled` over
NFC), and **no RF emission**; setting `lrw-region eu868` restores a working radio.

### Step 2 — A3: manual datarate parameter

- New `lrw-datarate` param (persisted, shell+NFC writable — same access model as the other
  `lrw_*` params) in `app_config.yml` + regenerated artefacts + decoder map. **As built:** an
  enum `auto | dr0..dr7` (wire `AUTO = 0`, `DRn = n + 1`) instead of an int with a sentinel —
  configen maps every int to `uint32`, so a negative sentinel is impossible and an enum lets
  `auto` be the proto3 default.
- Apply in `on_join_success()` after `lorawan_enable_adr()`, before
  `refresh_payload_budget()`; skip (with a log) when ADR is on; log loudly on `-EINVAL`
  (region/dwell-invalid DR). Calibration mode's own `LORAWAN_DR_5` stays authoritative
  while calibrating.

**Verify:** standard + configen pytest + decoder tests; bench check: ADR off + DR pinned,
confirm uplink DR on the LNS; ADR on + param set, confirm the skip log.

### Step 3 — A5a: general split rule (fPort 85 + fPort 3)

Design first, then code — the largest step, landed as sub-commits 3a–3g. **Rule:** every
uplink frame is a complete, independently decodable message sized to the current budget;
a logical message that does not fit becomes N self-contained messages (telemetry / history
precedent), never byte fragments. Record the wire decisions below in this plan before
coding and coordinate them with apps/manager and the LNS decoder owners.

**Design decisions (2026-09-22).** Measured first: at the 11 B tier a self-contained
protobuf frame does not fit even with a single field for most types — a `ConfigDump` page
with one field is 12 B, one alarm rule page 30 B, one history record ≥ 19 B, `Info`
serial + uptime 13 B. Only `Info` firmware version (9–11 B) and a stripped alarm
(source/type/slot, 9 B) fit. At 51 B everything splits fine. Hence:

1. **11 B tier = floor, not full function.** Telemetry, Ack, compact `Error` and
   `Info`-lite are delivered; every other response gets a compact `Error` with a new code
   **`BUDGET_TOO_SMALL`** so the host knows to retry once ADR raises the DR. Full splitting
   targets the ≥ 51 B tiers. No new binary format.
2. **Alarms at the 11 B tier: no fPort 3 detail**; the alarm *state* travels as bits in
   `Telemetry.system_flags` (mirror of the `device_status` alarm bits 0–5 → system_flags
   bits 1–6), which is in every telemetry frame and stays a 1-byte varint. Additive bits —
   an old decoder ignores them.
3. **`Info` at the 11 B tier: `Info`-lite** (firmware version, plus build type when it
   fits); an `Info` without `serial_number` is the lite form. The full `Info` (and the
   #412 settings-info) is sent automatically once the budget allows it.

**3a — Shared budget helper + compact Error.**
- One helper replaces the scattered `if (budget > 0 && budget < cap)` copies in
  `queue_info_uplink()`, `queue_settings_info_uplink()`, the downlink handler and
  `alarm_batch_flush()`. Budget 0 (MAC flood) means **defer** (retry after the MAC flush),
  not "no cap".
- Over LoRaWAN, `Error` omits `detail` (code + `fault_field` only, 7–9 B, fits every tier);
  NFC keeps the string. The "response too large" fallback then always fits, so no command
  goes unanswered.

**3b — Alarms: split instead of trim + alarm bits in telemetry.**
- `alarm_batch_flush()` emits as many `AlarmReport` frames as needed (each with the same
  `base_time` and `total`) instead of `n--` until one frame fits; queue depth and
  `ALARM_FRAME_MAX` sized so all `ALARM_BATCH_MAX` events can leave.
- When not even one event fits (11 B tier), no fPort 3 frame is built (logged); the state
  is carried by the new `Telemetry.system_flags` alarm bits (decision 2).

**3c — `Info`-lite + deferred boot announce + `BUDGET_TOO_SMALL`.**
- `app_cmd_build_info()` falls back to `Info`-lite when the full `Info` does not fit even
  with `active_alarms` trimmed (decision 3).
- If the boot `Info` went out as lite, or settings-info (#412) was skipped, a pending flag
  re-sends them once a DR change raises the budget enough.
- New `Error.Code BUDGET_TOO_SMALL`: the LoRaWAN "response too large" fallback uses it
  (instead of `UNKNOWN`), so the host can tell "retry at a higher DR" from a real failure.
- Closes #418 together with 3b.

**3d — settings-info + GetConfig: DR-adaptive, self-contained pages** (≥ 51 B tier; low
priority — the fixed 30 B pages already fit 51 B).
- settings-info (#413): split into several single-section `ConfigDump` pages when the budget
  is small (application / sensors / `w1_slot_type`), instead of skipping.
- GetConfig: page budget derived from the current payload budget (minus the measured
  wrapper overhead) instead of the fixed `DUMP_PAGE_BUDGET = 30`. **Pitfall:** `page_count`
  then depends on the DR, and the host requests pages one by one across downlinks — if the
  DR changes mid-read, the page layout shifts. Either pin the layout to the budget tier that
  was in effect for page 0 (and report the tier in the response), or keep fixed pages but
  size them for the 11 B tier on LoRaWAN. Decide before coding.

**3e — GetParam / W1Scan: paging** (≥ 51 B tier; low priority). Reuse the ConfigDump paging for GetParam; W1Scan
answers with one self-contained frame per 1–N ROMs (`page_index` / `page_count`).

**3f — HistoryFrame: real overhead and a defined floor.** `history_frame_cap()` uses
worst-case varints (33 B of overhead); compute it with the real `frame_count` / `t0` /
`present` values that are known up front. Then state the floor explicitly: if not even one
record fits (the 11 B tier), answer `req_history` with the compact `BUDGET_TOO_SMALL`
error instead of a silent stop.

**3g — Send-time budget.** Queue the logical message (or a rebuild callback) rather than
pre-encoded bytes, so `tx_send_queued()` can re-encode / re-split at the budget in effect
when the frame actually leaves. A DR drop between queue and send then costs an extra frame,
not the message.

**Out of scope for 3:** splitting a single telemetry group larger than the budget
(machine-probe reading, 29 B, at the 11 B tier) — telemetry already splits per group; a
per-reading split needs its own schema change. Tracked as a follow-up.

**Verify:** standard + proto pytest + decoder tests (each new frame shape decodes
standalone) + ztest cases in `tests/cmd` / `tests/compose` at 11 B, 51 B and 242 B budgets
for every message type in the §2 A5 audit table; bench check on US915 DR0 (same 11 B budget
as AU915 DR2, no AU gateway needed) walking the whole table.

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

Starts after #414 is merged into `v1.5.0` (#413 has already merged).

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
DR floor = DR2, 11 B budget, every row of the §2 A5 audit table after Step 3). Blocked on bench
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
