# HARDWARIO STICKER — LoRa Improvements: P2P Hardening and TOWER Transport

A survey of the HARDWARIO TOWER radio stack and the `twr-sdk` LoRaWAN driver, and what
STICKER should adopt from them. Covers the LoRaWAN side (regions, datarate, confirmed
uplinks, diagnostics) and the LoRa P2P side (duty cycle, ACK metadata, downlink, self-healing).

> Requested 2026-08-27: "review the LoRa implementation on TOWER, propose what we could add
> to STICKER (band switching, US915 and AU regulations), and propose improvements to LoRa
> P2P for STICKER."

**Status:** this document is the plan for PR #408, which implements the accepted items. It
opened as a survey and stays here as the rationale and reference for the code that follows;
the tracking checklist is in §6. Every claim is backed by a source citation, and feasibility
was verified against the actual APIs in this workspace before anything was scheduled.

**Base:** this branch stacks on `feat-p2p` (the P2P umbrella, PR #400), not directly on
`v1.5.0` — the P2P transport this work extends lives there, and PR #401/#403/#404 all landed
the same way. Line citations to `app/src/app_p2p.c` are against `feat-p2p` as of the #404
merge (`e3fb42d`); they will drift as the code changes, so treat the surrounding identifiers
as authoritative and the numbers as a hint.

---

## 1. Survey findings

### 1.1 `hardwario/tower-firmware` contains no LoRa

The referenced repository and commit (`tower-firmware` @ `b7f3f4a`, directory `apps/`) has
**zero LoRa or LoRaWAN code** — a case-insensitive search for `lora` over the whole tree
returns nothing. It is a Rust/Embassy (`no_std`) firmware SDK for the TOWER Core Module
(STM32L083CZ) whose radio is a **SPIRIT1 / SPSGRF GFSK transceiver** with a bespoke
AES-128-CCM network layer. Its "EU868 / US915" are proprietary narrowband GFSK channels,
not LoRaWAN regions.

That does not make it irrelevant — quite the opposite. It is a mature sub-GHz end-device
stack solving exactly the problems STICKER's P2P protocol is now facing (duty-cycle
compliance, ACK metadata, downlink to a sleeping node, counter persistence), and it solves
several of them better than we currently do. Section 3 is built on it.

### 1.2 The real HARDWARIO LoRaWAN reference is `twr-sdk`

`hardwario/twr-sdk` drives a Murata CMWX1ZZABZ over UART AT commands:

| File | Role |
|---|---|
| `twr/src/twr_cmwx1zzabz.c` | AT state machine (1616 lines) |
| `twr/inc/twr_cmwx1zzabz.h` | Driver API |
| `twr/src/twr_at_lora.c` | User-facing `AT$…` command handlers |

It is an AT-modem wrapper, not a LoRaWAN MAC — the region tables live in the modem. Its
value to us is the **parameter surface** it chose to expose (Section 2).

### 1.3 What STICKER already has

To avoid duplicating work, the current state was verified in the tree:

- **Runtime region selection already exists**: `lrw-region` enum {EU868, US915, AU915},
  applied in `app_lrw_init()` via `lorawan_set_region()` (`app/src/app_lrw.c:1556-1596`).
- **Sub-band selection already exists**: `lrw-sub-band` 0–8 with a hand-built 6-word channel
  mask, `apply_subband()` (`app/src/app_lrw.c:1455-1484`), gated to US915/AU915 only.
- **US915 is validated end to end** — issue #303, closed 2026-07-28, no firmware defect found
  (OTAA join on FSB2, confirmed/unconfirmed uplink, DR0–DR4, payload budget, region-switch
  hygiene). Test plan in `doc/us915-test-plan.md`, playbook scenarios AT-LRW-13..15.
- **Downlink RSSI/SNR is already captured and printed** by `ats radio status`
  (`app_lrw.c:209-210`, `:1350-1351`, `app_ats.c:573-574`).
- **`radio-mode`** {off, lorawan, p2p} selects the transport at boot (#271/#314); default
  flipped to `off` by #350.
- **P2P** (branch `feat-p2p`): EU868-only, SF10/BW125, AES-CCM data plane, app_key-rooted
  CMAC KDF (HIL-validated, PR #404), `dev_nonce` capped-skip anti-replay, NVS session
  persistence with a 256-frame counter reservation.

So "band switching" and "US915 support" are, for LoRaWAN, **already shipped**. The genuinely
open work is elsewhere, and is enumerated below.

---

## 2. Track A — LoRaWAN improvements

Inspired by the `twr-sdk` parameter surface (`AT$BAND`, `AT$DR`, `AT$REPC`, `AT$RFQ`).

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
gated). Today the app hard-fails the whole radio init on that error
(`app/src/app_lrw.c:1573-1577`), which means a stored `lrw-region us915` on a debug image
silently produces a device with no radio at all.

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
if ADR is enabled (`lorawan.c:600-602`). Hook it in `on_join_success()`
(`app_lrw.c:439`) immediately after `lorawan_enable_adr()` and before
`refresh_payload_budget()`, so the budget reflects the manual DR. It re-applies on every
rejoin for free.

Precedent: `app_calibration.c:298` already calls `lorawan_set_datarate(LORAWAN_DR_5)`.

Gotcha: DR validity is region- and dwell-dependent — AU915 with dwell=1 rejects DR0/DR1
(`RegionAU915.c:412-423`), so a config value of 0–1 fails at join with only an `-EINVAL`.
Validate per region, or log loudly.

### A4 — Confirmed uplinks for alarms

Alarms (fPort 3) are currently sent unconfirmed like everything else
(`app_lrw.c:1098`) — an alarm lost to RF is lost silently. `lorawan_set_conf_msg_tries()`
(`lorawan.h:343`) plus `LORAWAN_MSG_CONFIRMED` in `tx_send_queued()` (`app_lrw.c:1086`)
would fix that; the existing failure path already requeues with backoff, so a NOACK
(`-ETIMEDOUT`) is handled.

**Two gotchas that must be in the implementation plan:**

1. `MIB_CHANNELS_NB_TRANS` also repeats **unconfirmed** uplinks —
   `CheckRetransUnconfirmedUplink()` uses the same limit
   (`modules/lib/loramac-node/src/mac/LoRaMac.c:3614-3618`). Setting tries globally would
   triple *telemetry* airtime too. Use a set → send → restore sequence around the confirmed
   send (`lorawan_send()` is synchronous). A `LinkADRReq` can also overwrite NbTrans
   (`LoRaMac.c:2262`).
2. A confirmed send blocks `m_work_q` for the whole retry sequence, and the watchdog
   heartbeat runs on the same queue with a 30 s timeout (`app_lrw.c:100-101`). Verify the
   worst-case NbTrans duration at DR0 before shipping.

### A5 — AU915 dwell time ("AU regulations")

Investigated in detail, and the conclusion corrects a common assumption: **dwell time is
already fully MAC-enforced and needs no app changes for telemetry.**

- `AU915_DEFAULT_UPLINK_DWELL_TIME = 1` (`RegionAU915.h:111`), dwell-limited minimum
  datarate `AU915_DWELL_LIMIT_DATARATE = DR_2` (`RegionAU915.h:81`).
- The Dwell1 payload table (`RegionAU915.h:256`) gives DR0/DR1 = 0 B, **DR2 = 11 B**,
  DR3 = 53, DR4 = 125, DR5/6 = 242.
- That table already flows into the app: `GetPhyParam(PHY_MAX_PAYLOAD)` selects it from
  `UplinkDwellTime` (`RegionAU915.c:153-159`) → `LoRaMacQueryTxPossible` →
  `lorawan_get_payload_sizes()` (`lorawan.c:618-628`) → `refresh_payload_budget()`
  (`app_lrw.c:308`) → compose. `PHY_MIN_TX_DR` is dwell-aware too, so the ADR floor and
  `lorawan_set_datarate()` validation both respect DR2.

**The real gap** is elsewhere: `tx_send_queued()` **drops** over-budget response and alarm
frames instead of fragmenting them (`app_lrw.c:1090-1095`). At an 11 B DR2 budget, the
GetInfo-on-join frame and most alarm frames are near-guaranteed drops. That is the actual
AU915 work item — and it is shared with AS923, which also defaults to dwell=1.

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

Two notes:

- The **channel-mask concern does not apply** — `apply_subband()` is gated to US915/AU915
  (`app_lrw.c:1585-1587`), and AS923 uses a size-1 mask it never touches.
- The **channel plan group** (AS923-1/2/3/4) is compile-time only in loramac-node
  (`RegionAS923.c:54-55`), injectable via
  `zephyr_compile_definitions(REGION_AS923_DEFAULT_CHANNEL_PLAN=…)`. One group per build —
  build variants, not a runtime setting.
- AS923 also defaults to dwell=1, so the A5 fragmentation gap applies here too.

### A7 — RX2 override

No Zephyr API exists, but `app_lrw.c` already includes `<LoRaMac.h>` and calls
`LoRaMacMibSetRequestConfirm` directly for the RX delays and public-network flag
(`app_lrw.c:888-905`) — exact precedent. `MIB_RX2_CHANNEL` (and `MIB_RX2_DEFAULT_CHANNEL`,
so a MAC reset does not revert it) would work the same way, re-applied after join because
JoinAccept DLSettings and `RXParamSetupReq` both overwrite RX2.

Low priority, and it should stay a debug/expert knob: an override that does not match the
LNS silently kills all downlinks.

---

## 3. Track B — LoRa P2P improvements

Inspired by the TOWER SPIRIT1 network layer. Section 4 compares the two protocols in detail
and justifies each item.

| # | Proposal | TOWER pattern adopted | Blast radius |
|---|---|---|---|
| B1 | **ACK carries RSSI/SNR** | ACK with receiver RSSI | `app_p2p.c` ~10 lines + central |
| B2 | **Token-bucket duty governor** | `DutyGovernor` with residue carry | `app_p2p.c`, ~6 sites |
| B3 | **Self-healing rejoin** | — (our own §7 spec, unimplemented) | `app_p2p.c` ~30 lines |
| B4 | **Pending-downlink chaining + COMMAND dispatch** | pending flag → chained RX | `app_p2p.c`, `app_cmd`, central |
| B5 | **Clock sync over P2P** | — | `app_p2p.c` + central |
| B6 | **Multi-channel / P2P region model** | runtime band switch, per-node channel | yml, `app_p2p.c`, doc, central |
| B7 | **CAD / listen-before-talk** | CSMA + AFA | driver-level, not a quick win |
| B8 | **Pull-based bulk transfer** | `bulk_serve`/`bulk_fetch` | deferred to v2 |
| B9 | **Counter/replay hardening audit** | fail-closed reserve-ahead | review of `app_p2p.c` |

### B1 — ACK carries RSSI/SNR

TOWER's ACK carries the acknowledged counter, the receiver's RSSI, and a pending flag. Ours
carries one byte of flags. Adding RSSI/SNR gives every uplink a free link-quality
measurement — the basis for any future adaptive SF or TX-power policy, and a cheap
deployment/diagnostic tool.

The RX metadata is already plumbed: `lora_recv(dev, buf, size, timeout, &rssi, &snr)`
(`zephyr/include/zephyr/drivers/lora.h:284-286`) is already used in `p2p_rx_window()`
(`app_p2p.c:181-183`) and captured at `:780-785`.

**Compatibility gotcha:** `recv_ack()` does a strict length check (`app_p2p.c:846`, `len != want`
→ reject), so an old node would treat a 3-byte ACK as no ACK at all, retry, and give up. The
central must key the ACK size off the session's `proto_version` (sent in the JoinRequest body
and persisted centrally). Consider a P2P-specific wire version — `APP_PROTO_VERSION` is
currently shared with the fPort-85/NFC command protocol.

### B2 — Token-bucket duty governor

The current model is a single "blocked until" timestamp (`app_p2p.c:274`): after each TX the
device is hard-blocked for 99× the airtime. TOWER instead runs a token bucket (36 000 ms per
hour for EU 1 %, with sub-millisecond residue carry so repeated small charges do not round
away). The practical difference: a token bucket permits a legitimate burst — an alarm
immediately after a telemetry frame — while still holding the hourly average.

This matters more than it sounds: the code notes a 240 B SF10 frame blocks for **~227 s**
under the current model (`app_p2p.c:181-183`).

Implementation note: the duty gate currently runs before the frame length is known
(`tx_frame`, `app_p2p.c:807`). Either gate coarsely (`tokens <= 0`) or move the check into
`tx_frame_at()` where `wire_len` exists.

### B3 — Self-healing rejoin

`doc/p2p.md` §7 already specifies this ("N consecutive fully-failed uplink cycles, default
8"), but it is not implemented — link loss is visible exactly once, in the give-up log after
`P2P_ACK_MAX_RETRIES` (`app_p2p.c:962-965`). A device whose session goes stale stays mute
until someone power-cycles it.

The machinery already exists: `app_p2p_rejoin()` (`app_p2p.c:1579`) does exactly the
right thing but is `CONFIG_SHELL`-gated. Needed: a consecutive-failure counter, promotion of
`app_p2p_rejoin()` out of the shell gate, and exponential backoff between rounds (§7 requires
it; nothing exists today). §5.2 explicitly exempts self-healing rejoin from the 120 s
never-paired boot window.

### B4 — Pending-downlink chaining and COMMAND dispatch

Today the ACK's pending bit is read and logged only (`app_p2p.c:824`, `:874`), and
`APP_P2P_FRAME_COMMAND` (0x56) is reserved but never dispatched (`app_p2p.h:58`). So P2P is
effectively uplink-only: no remote configuration, no remote commands. TOWER chains RX windows
while the pending flag stays set, and gets a remote shell over the air out of it.

The dispatch target already exists and is transport-generic:
`app_cmd_handle(enum app_cmd_transport, …)` (`app_cmd.h:126`), called by the LoRaWAN path
with `APP_CMD_TRANSPORT_LRW`. Adding `APP_CMD_TRANSPORT_P2P` keeps the writability gating
explicit; responses go out through the existing `app_p2p_queue_response()`.

Gotcha: `p2p_rx_window()` sizes its timeout from the expected frame length (the driver has no
hardware symbol timeout). Commands are variable-length, so the window must be sized for the
maximum command frame — meaning a longer radio-on time per pending downlink. Worth measuring
against the 92 µA idle baseline.

### B5 — Clock sync over P2P

In P2P mode the device has no wall clock, so alarm timestamps and history entries lose
absolute time. The sink already exists and is public: `app_clock_set_unix()`
(`app_clock.h:44`), documented as the manual-provisioning path. Only the wire mechanism is
missing — either 4 bytes of Unix time in the extended ACK (pairs naturally with B1) or a
ClockSync command over 0x56 (pairs with B4). `app_clock.c` itself needs no changes.

### B6 — Multi-channel and a P2P region model

Correction to a common assumption: the P2P frequency is **already a runtime parameter**
(`p2p_frequency`, default 868.1 MHz, bounds 863–870 MHz). What is hardcoded is the bandwidth,
coding rate, and the EU 1 % duty model (`app_p2p.c:571-573`, `:120`).

`doc/p2p.md` §3.3 states the constraint plainly: a deployment outside EU868 needs a new
duty-cycle model, not just a different frequency.

A `p2p_region` enum would parametrise: frequency bounds (the YAML min/max is static, so
per-region validation must move into C), the duty model (US915/AU915 have no duty-cycle
limit), and bandwidth.

**Landmine to resolve before committing:** at SF10/BW125 a maximum-size frame is ~2.3 s of
airtime. AU915's 400 ms dwell limit forbids that outright, so AU/US would need BW500 and/or a
frame-size cap. And US915 compliance under FCC §15.247 means either frequency hopping across
≥50 channels (TOWER's FHSS approach) or digital transmission system rules with BW500. The
recommendation is BW500 single-channel plus a compliance review with the certification lab —
this is a regulatory decision, not just a firmware one.

The JoinAccept's `reserved(4)` field (`app_p2p.c:158-160`) is the confirmed hook for per-node
channel assignment (`doc/p2p.md` §5.3) and comfortably fits channel index, SF, TX power, and
flags.

### B7 — CAD / listen-before-talk

TOWER does CSMA with a −90 dBm CCA threshold before every TX, and adaptive frequency agility
(8 channels, per-channel off-time) for EN 300 220. STICKER P2P transmits blind on a single
shared channel — with several STICKERs in range, collisions are unmanaged.

**This one is not a quick win.** The Zephyr LoRa API has no CAD at all; the loramac-node HAL
does (`Radio.StartCad`, `sx126x/radio.c:1145`, delivering `RadioEvents->CadDone`), but the
Zephyr driver owns the static `RadioEvents_t` and registers only TxDone/RxDone/RxError/TxTimeout
(`zephyr/drivers/lora/loramac_node/sx12xx_common.c:387-391`). Adding CAD means patching the
in-workspace Zephyr driver — the same driver work as the GFSK PHY in Section 5, so the two
should be planned together.

### B8 — Pull-based bulk transfer (deferred, v2)

TOWER has a pull-based bulk transfer over `BulkSource`/`BulkSink` traits, verified to 64 KB
in constant RAM. That is precisely the primitive needed for history replay over P2P and,
later, FUOTA. `doc/p2p.md` §13 explicitly excludes both from v1; this is recorded as the
shape to adopt when they come back.

### B9 — Counter and replay hardening audit

TOWER's counter/replay handling has several properties worth auditing ours against:

1. **Verify then classify** — CCM verification happens *before* the replay comparison, so a
   forged high counter cannot poison replay state.
2. **Fail closed** — if the counter watermark cannot be durably written and verified, the
   radio refuses to transmit rather than risking nonce reuse.
3. **Counter consumed at seal time, not TX time** — a cancelled send skips a counter
   (harmless) instead of reusing one (a full CCM break).
4. **Saturate, never wrap** — the counter saturates at 2³²−1.

STICKER already has reserve-ahead persistence (`P2P_FCNT_RESERVE 256`). This is an audit
item, not a feature: confirm the fail-closed branch exists and behaves.

---

## 4. TOWER vs STICKER P2P — protocol comparison

Per-row verdict on which design is better and why. This is the evidence base for Section 3.

| Aspect | TOWER (SPIRIT1) | STICKER P2P | Better | Why |
|---|---|---|---|---|
| PHY | GFSK 19.2 kbps, ±20 kHz dev | LoRa SF10/BW125 | **P2P** | ~20+ dB better link budget (SF10 ≈ −134 dBm vs GFSK ≈ −110 dBm) — multiples of the range, at the cost of airtime. For a battery sensor sending small frames, the right trade |
| MTU / frame | 96 B max (SPIRIT1 FIFO) | 255 B (11 B header + 240 B body) | **P2P** | less fragmentation; 96 B would force splitting on mid-size reports |
| Addressing | 32-bit node address | `net_id`(4) + `dev_addr`(2) | **P2P** | explicit `net_id` supports several independent networks in RF range and cheap early-drop of foreign frames |
| CCM tag | 8 B | 4 B | **TOWER** | 2⁻⁶⁴ vs 2⁻³² forgery probability. Ours traded tag length for airtime — defensible at a low frame rate, but it is a deliberate trade and should be recorded as one |
| CCM nonce | `src ‖ counter ‖ bulk_idx` (13 B) | `counter ‖ dev_addr ‖ type ‖ dir` (13 B) | **P2P** | the direction byte rules out an uplink/downlink nonce collision under the same counter; otherwise equivalent |
| Key establishment | host-minted key delivered under a **public** PAIRING_KEY, bounded window | CMAC KDF from a pre-shared `app_key`; the key is **never on air** (HIL-validated, PR #404) | **P2P** | no key transport over RF at all. TOWER protects the key only with a well-known key plus a time window |
| Join anti-replay | EEPROM-persisted epoch, replay lanes with bounded lazy persistence | monotonic `dev_nonce` + capped-skip (1024), NVS | **P2P** (adopt TOWER's edges) | capped-skip avoids permanently bricking a device's ability to join. From TOWER take verify-then-classify and bounded-exposure persistence (B9) |
| Counter persistence | reserve-ahead 1024, **fail-closed**, saturating, consumed at seal time | reserve-ahead 256; fail-closed unverified | **TOWER** | explicit fail-closed, saturation, and seal-time consumption are strictly stronger guarantees → audit item B9 |
| ACK contents | counter echo + **RSSI** + pending flag | 1 byte of flags (bit 0 = pending) | **TOWER** | link-quality feedback on every uplink for free → adopt as B1 |
| Retransmit / dedup | byte-identical retry, high-water dedup, re-ACK | byte-identical retry, high-water dedup, re-ACK | tie | both correct, and both nonce-reuse-safe |
| Downlink | pending flag → chained RX windows, remote shell over the air | pending flag logged only; COMMAND frames not dispatched | **TOWER** | a complete bidirectional path. Ours has the hook but not the plumbing → B4 |
| Duty / compliance | token bucket with residue carry, LBT/AFA (EU), FHSS (US), runtime band switch | blocked-until timestamp, EU-only, no LBT | **TOWER** | compliance by construction and genuinely multi-region → B2, B6, B7 |
| Bulk transfer | pull-based, constant RAM, verified to 64 KB | none | **TOWER** | a ready-made primitive for history replay and FUOTA → B8 |
| Link diagnostics | RSSI/LQI/SQI/AFC per packet, channel RSSI scan | RSSI/SNR logged, shown in `ats radio status` | **TOWER** | richer link telemetry; adopt partially via B1 |
| Gateway / central model | stateful dongle gateway, registry in EEPROM | stateless keyless gateways + one central (FIBER v2), multi-gateway dedup and roaming | **P2P** | scales to many gateways, keeps all state in one place, and gateways hold no keys |
| Testability | pure decision kernels split into host-testable `no_std` crates (`tower-net-core`, `tower-radio-core`) | `app_p2p.c` is HW/HIL-only; no native ztest suite covers it | **TOWER** | all regulatory arithmetic and security accept/reject logic unit-tested on the host. Strong argument for extracting our P2P decision logic into a testable core |

**Summary:** STICKER P2P wins on radio reach, framing, addressing, key hygiene, and network
architecture. TOWER wins on operational maturity — duty-cycle compliance, downlink,
diagnostics, counter hardening, and testability. Track B is the list of TOWER's wins worth
importing.

---

## 5. Next phase — a `radio-mode tower` transport

Decision (2026-08-27): **no protocol merge.** STICKER P2P stays its own protocol and simply
absorbs the good ideas above. Talking to a TOWER network is a separate, later feature: a
fourth transport mode in which STICKER speaks the TOWER protocol natively.

**Shape of the work:**

- A fourth value in the `radio_mode` enum {off, lorawan, p2p, **tower**}, selected at boot —
  consistent with the current design (one radio, no concurrent modes). The decoder's enum map
  needs the new value.
- **The PHY is reachable — verified.** TOWER is GFSK 19.2 kbps / ±20 kHz deviation / ~216 kHz
  RX bandwidth / 4-byte sync word `0xDB624715` / CRC-16 `0x1021` / PN9 whitening
  (`tower-firmware`, `src/radio/config.rs`). The STM32WLE5's SX126x has a full GFSK modem and
  the in-tree loramac-node HAL already drives it: the `MODEM_FSK` paths in
  `modules/lib/loramac-node/src/radio/sx126x/radio.c`, sync words up to 8 bytes
  (`SX126xSetSyncWord`, `:692`), whitening (`RADIO_DC_FREEWHITENING` + `SX126xSetWhiteningSeed`,
  `:686`, `:693`), and configurable CRC. The nearest RX bandwidth is 234.3 kHz.
- **A new `app_tower` transport module**, structured like `app_p2p` and plugged into the
  existing `app_radio` facade: frame codec (96 B cap), AES-128-CCM with an 8-byte tag (our
  `app_ccm` hardware engine is the same primitive, only the nonce layout differs), replay
  lanes, the 3-way JOIN pairing, and a duty governor.
- **Radio access**: Zephyr's `lora.h` is LoRa-only and has no FSK, so this needs either direct
  `Radio.*` HAL calls or an extension to the in-workspace Zephyr driver — the same driver work
  as B7 (CAD), so plan them together.

**Open risks:** bit-level PHY interop must be validated on the bench against a real TOWER
dongle (PN9 implementation, bit order, length-field placement in the SPIRIT1 Basic packet
format versus the SX126x packet engine, and CRC-versus-whitening ordering are the classic
traps). `tower-firmware` is under active development, so the work must be pinned to a specific
commit. And the 96 B frame limit constrains payloads relative to what P2P allows today.

---

## 6. Priorities and tracking

The scope of this PR is the P2P and TOWER work; the LoRaWAN items are listed for
completeness and may be split into their own PRs if they grow.

**Quick wins**

- [ ] B2 — token-bucket duty governor
- [ ] B3 — self-healing rejoin
- [ ] B1 — ACK carries RSSI/SNR *(needs central, S1)*
- [ ] A1 — build-vs-runtime region guard
- [ ] A2 — RSSI/SNR in GetInfo
- [ ] A3 — manual datarate parameter

**Medium** — needs central/gateway coordination or a larger change

- [ ] B9 — counter/replay hardening audit
- [ ] B4 — pending-downlink chaining + `0x56` COMMAND dispatch *(needs central, S2)*
- [ ] B5 — clock sync over P2P *(needs central, S3)*
- [ ] A5 — AU915 dwell: fragment instead of dropping over-budget frames
- [ ] A6 — AS923 region (+2 576 B flash, +0 B RAM)
- [ ] A4 — confirmed uplinks for alarms

**Large — design and compliance first**

- [ ] B6 — multi-channel / P2P region model *(blocked on the regulatory decision, §3 B6; needs central, S4)*
- [ ] B7 — CAD / listen-before-talk *(Zephyr driver patch)*

**Next phase**

- [ ] `radio-mode tower` — GFSK PHY + `app_tower` transport (Section 5)

**Deferred / v2:** A7 (RX2 override), B8 (bulk transfer).

Server-side counterparts for B1, B4, B5, and B6 are defined in
[proximos-v2 MR !30](https://gitlab.hardwario.com/proximos/proximos-v2/-/merge_requests/30)
as S1–S4.

Suggested order: B2 and B3 are self-contained device-side changes with no external
dependency, so they can land first. B1 needs the central to key the ACK size off the session
`proto_version`, so it should land alongside S1. B7 and Section 5 share the same Zephyr
driver work and should be planned together.

Section 7 breaks this into ordered, commit-sized steps.

## 7. Implementation steps

One step per commit, in order. Each step states what changes, which files, and how it is
verified. Steps 1–4 have no external dependency; 5–7 need their server-side counterpart in
[MR !30](https://gitlab.hardwario.com/proximos/proximos-v2/-/merge_requests/30) to land first.

Standard verification for every step, unless noted: the three build configs (Release
dual-stack, `debug.conf`, `debug.conf + debug_p2p_bench.conf`), `bash tests/run_native.sh`,
and `clang-format --dry-run --Werror`.

### Step 1 — a native test suite for the P2P logic

**Why first:** `app_p2p.c` has *no* native test coverage today. `tests/p2p/` is a bench
firmware for a second STICKER, not a ztest suite (no `testcase.yaml`, so `run_native.sh`
skips it), which is why memory records this file as "HW/HIL only". Every P2P change below is
currently verifiable only on a bench. This step buys a safety net before anything moves, and
is TOWER's host-testable-decision-kernel lesson (§4) applied to us.

- Add `tests/p2p_logic/` (`testcase.yaml`, `prj.conf`, `CMakeLists.txt`, `src/main.c`), picked
  up automatically by `tests/run_native.sh`.
- Characterise the pure helpers **as they behave today**: `frame_toa_ms()`
  (`app_p2p.c:611`), the CCM nonce layout (`:700-713`), the frame counter reserve/resume math
  (`:449-460`), and the current duty-cycle arithmetic (`:793`).
- Extract only what the tests need into a header-visible or `static`-free form; no behaviour
  change in this step.

**Verify:** the new suite passes and the other ten still do — 11 suites green.

### Step 2 — B2: token-bucket duty governor

Replaces the single `m_dc_blocked_until` deadline (`app_p2p.c:274`) with a refilling budget.

- New state: `m_duty_tokens_ms` + `m_duty_last_refill`; helpers `duty_refill()`,
  `duty_available_ms()`, `duty_wait_ms(payload_len)`, `duty_charge(air_ms)`.
- Rewrite the nine touch points: gates at `:715`, `:807`, `:933`, `:1214`; charges at `:793`,
  `:1260`; wait computations at `:888`, `:1059`, `:1372`.
- **Known wrinkle:** the gate in `tx_frame()` (`:807`) runs *before* the frame is built, so
  the payload length is unknown there. Keep that one coarse (`duty_available_ms() <= 0`) and
  do the exact check in `tx_frame_at()`, where `wire_len` exists.
- **Decision needed — the bucket cap.** Today's model blocks for `air × 99` after *every*
  frame, so an alarm queued behind a 240 B SF10 telemetry frame waits ~227 s (`:225`). A
  bucket lets the alarm go immediately as long as budget remains, which is the actual point
  of this change. How much burst to allow is a regulatory question, not a coding one: start
  the cap low (a few seconds of air-time), which keeps us at least as conservative as today
  while still fixing the alarm-latency case, and raise it only with a compliance review.

**Verify:** standard, plus new duty cases in `tests/p2p_logic/` — refill accrual, burst then
starve, and that the hourly total never exceeds 1 %.

### Step 3 — B3: self-healing rejoin

The link-loss signal exists but nothing acts on it: the give-up branch at `app_p2p.c:962-965`
only logs.

- Add `m_consec_uplink_fail`; increment at the give-up branch, reset on any successful
  `recv_ack()`.
- At the threshold (8 consecutive fully-failed uplink cycles, `doc/p2p.md` §7) drop to
  `P2P_LINK_JOINING` and schedule `m_join_work` — the work `app_p2p_rejoin()` (`:1579`)
  already does. Promote it out of its `#if defined(CONFIG_SHELL)` guard so the non-shell
  Release build can self-heal.
- Add exponential backoff between rejoin rounds (nothing exists today). Keep
  `app_key_is_set()` and the per-round boot-window cap (`:1345-1353`) intact; §5.2 exempts
  self-healing from the never-paired 120 s cap, but each round still bounds itself.

**Verify:** standard, plus a `tests/p2p_logic/` case for the counter/threshold/backoff state
machine. Real link loss stays a bench test — `ats radio ack_drop <n>` already forces it.

### Step 4 — B9: counter and replay hardening audit

An audit with targeted fixes, not a feature. Doing it before the server-facing work means the
data plane is sound before we extend it.

- **Fail-closed** on a failed frame-counter reserve write. TOWER refuses to transmit
  (`NonceLocked`) rather than risk a `(key, nonce)` repeat; confirm ours does the same at
  `:449-460` and make it explicit if not.
- **Saturate, don't wrap**, at `UINT32_MAX`.
- **Verify-then-classify**: confirm CCM verification precedes any replay-state update, so a
  forged high counter cannot poison the window.
- **Counter consumed at seal time**, not send time, so a cancelled or failed send burns a
  counter instead of reusing one.

**Verify:** standard, plus a `tests/p2p_logic/` case per fix. Each fix that turns out to be
already correct gets recorded as a test, not a code change.

### Step 5 — B1: ACK carrying RSSI/SNR *(needs S1)*

- Node side: relax the strict `len != want` check (`app_p2p.c:846`) to accept **either** the
  current 1-byte ACK body or the new 3-byte one. Accepting both is what makes this
  deployable — a new node must still work against a not-yet-updated gateway, and an old node
  must not be broken by a new one.
- Store the reported RSSI/SNR, surface via `app_p2p_get_info()` and `ats radio status`.
- Version the wire change off the session's `proto_version`, and consider a P2P-specific wire
  version rather than reusing `APP_PROTO_VERSION`, which is shared with the fPort-85/NFC
  command protocol.

**Verify:** standard, plus round-trip cases against both body lengths. Bench test with the
gw-sim in `tests/p2p/`.

### Step 6 — B4: pending-downlink chaining and `0x56` COMMAND dispatch *(needs S2)*

- Add `APP_CMD_TRANSPORT_P2P` to `app_cmd.h`, so P2P gets its own writability gating rather
  than borrowing the LoRaWAN transport's.
- On ACK flags bit 0 — read and logged only today (`app_p2p.c:824`, `:874`) — open a further
  RX window, decrypt under the session key with `P2P_DIR_RX`, dispatch through the existing
  transport-generic `app_cmd_handle()`, and return the reply via `app_p2p_queue_response()`.
- **Cost to weigh:** `p2p_rx_window()` sizes its timeout from the *expected* frame length
  because the driver has no hardware symbol timeout (`:181-183`). Commands are
  variable-length, so the window must be sized for the largest one — that is real
  receiver-on time, and therefore real battery, per pending downlink.

**Verify:** standard, plus a command round-trip over the bench rig.

### Step 7 — B5: clock sync over P2P *(needs S3)*

The sink already exists and is public: `app_clock_set_unix()` (`app_clock.h:44`). Only the
wire format is new — either 4 bytes of unix time on the extended ACK (pairs with Step 5) or a
ClockSync command over `0x56` (pairs with Step 6). Pick whichever of those two lands first.

### Later — design and compliance gated

- **B6 (P2P region model)** is blocked on the regulatory decision in §3: an SF10/BW125 frame
  is ~2.3 s of air, which AU915's 400 ms dwell forbids outright. Needs a certification-lab
  answer before the server-side channel-assignment policy (S4) can be finalised.
- **B7 (CAD/LBT)** and **the TOWER GFSK PHY (§5)** both need the same in-workspace Zephyr LoRa
  driver extension, so plan them as one piece of work rather than opening that driver twice.
- **`radio-mode tower` / `app_tower`** follows the driver work.

### Not in this PR

Track A is LoRaWAN work in `app_lrw.c`, and this branch is based on `feat-p2p`. A1, A2, A3,
A5 and A6 (AS923) should therefore go to their own PRs against `v1.5.0` rather than ride
along here.

## 8. Explicit non-goals

- **Class C / multicast** — STICKER is a battery device and Class A only is a deliberate
  choice; LTO strips the Class B/C code paths today.
- **FHSS for P2P in EU** — TOWER's FHSS exists to satisfy FCC §15.247. Under ETSI the duty
  cycle model is sufficient, and FHSS would add complexity for nothing.
- **Runtime AS923 channel-plan switching** — loramac-node fixes the plan group at compile
  time; build variants cover the need.

---

## 9. References

**External**

- `hardwario/tower-firmware` @ `b7f3f4a` — `docs/radio.md` (radio stack guide),
  `src/radio/config.rs` (band and RF parameters), `src/radio/net/` (network layer),
  `crates/tower-net-core/src/txctr.rs` (counter persistence),
  `apps/radio_push_button.rs` (sleeping-node uplink and downlink chaining).
- `hardwario/twr-sdk` @ `9ded554` — `twr/src/twr_cmwx1zzabz.c`, `twr/src/twr_at_lora.c`.

**In-tree**

| Topic | Location |
|---|---|
| LoRaWAN application logic | `app/src/app_lrw.c` |
| Region selection, sub-band mask | `app/src/app_lrw.c:1455-1484`, `:1556-1596` |
| Config schema | `app/src/app_config.yml` |
| Zephyr LoRaWAN API | `zephyr/include/zephyr/lorawan/lorawan.h`, `zephyr/subsys/lorawan/lorawan.c` |
| Region tables | `modules/lib/loramac-node/src/mac/region/` |
| P2P protocol spec | `doc/p2p.md` (branch `feat-p2p`) |
| P2P implementation | `app/src/app_p2p.c` (branch `feat-p2p`) |
| US915 validation | issue #303, `doc/us915-test-plan.md`, playbook AT-LRW-13..15 |
