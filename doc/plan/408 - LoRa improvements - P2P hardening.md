# HARDWARIO STICKER — LoRa Improvements: P2P Hardening

A survey of the HARDWARIO TOWER radio stack and the `twr-sdk` LoRaWAN driver, and what
STICKER should adopt from them. This PR carries the LoRa P2P side (duty cycle, ACK metadata,
downlink, self-healing). The LoRaWAN side moved to
[PR #409](https://github.com/hardwario/sticker-firmware/pull/409) (§2) and the TOWER
transport to [PR #410](https://github.com/hardwario/sticker-firmware/pull/410) (§5).

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

## 2. Track A — LoRaWAN improvements (moved to PR #409)

The LoRaWAN items (A1–A7: region guard, AS923, manual datarate, RSSI/SNR in GetInfo, the
AU915 fragment-not-drop gap, confirmed alarms, RX2 override) moved to their own PR so that
`app_lrw.c` work targets `v1.5.0` directly instead of riding on `feat-p2p`:

**[PR #409](https://github.com/hardwario/sticker-firmware/pull/409)** —
`doc/plan/409 - LoRaWAN improvements - regions, datarate, diagnostics.md`.

Section numbering below is kept stable to preserve cross-references.

---

## 3. Track B — LoRa P2P improvements

Inspired by the TOWER SPIRIT1 network layer. Section 4 compares the two protocols in detail
and justifies each item.

> **Governing principle — all communication goes through the `app_radio` facade.**
> Every function a transport exposes to the rest of the app (uplink, downlink, link-ready
> announces, history) is reached through the `app_radio_*` facade, never by an application
> module calling `app_lrw_*` or `app_p2p_*` directly. The facade is the single seam that lets
> a transport be added, swapped or extended without touching callers. A feature counts as
> "done" only when it is either routed through the facade (and therefore works on every
> transport) **or** explicitly and deliberately transport-specific and documented as such
> (e.g. `lrw_join`). This is what B10 audits and enforces; new work must not add a second
> transport-hardcoded call site.

| # | Proposal | TOWER pattern adopted | Blast radius |
|---|---|---|---|
| B1 | **ACK carries RSSI/SNR** | ACK with receiver RSSI | `app_p2p.c` ~10 lines + central |
| B2 | **Sliding-hour duty ledger** | exact per-window ledger (token bucket withdrawn) | `app_p2p.c`, ~6 sites |
| B3 | **Self-healing rejoin** | — (our own §7 spec, unimplemented) | `app_p2p.c` ~30 lines |
| B4 | **Pending-downlink chaining + COMMAND dispatch** | pending flag → chained RX | `app_p2p.c`, `app_cmd`, central |
| B5 | **Clock sync over P2P** | — | `app_p2p.c` + central |
| B6 | **Multi-channel / P2P region model** | runtime band switch, per-node channel | yml, `app_p2p.c`, doc, central |
| B7 | **CAD / listen-before-talk** | CSMA + AFA | driver-level, not a quick win |
| B8 | **History replay over P2P** (device-driven stream) | — (mirrors LoRaWAN replay) | `app_radio`, `app_p2p.c`, `app_cmd`, central |
| B9 | **Counter/replay hardening audit** | fail-closed reserve-ahead | review of `app_p2p.c` |
| B10 | **Transport parity via facade** | — (our governing principle) | `app_radio`, `app_lrw.c`, `app_p2p.c`, `app_cmd` |
| B11 | **Detach / RejoinRequest** (central-initiated) ✅ #416 | — (extends B3) | `app_p2p.c` + central |
| B12 | **Per-node TX-power via JoinAccept** ✅ #416 | — | `app_p2p.c` + central |
| B13 | **Exact RX1 window from `pending_frame_len`** ✅ #416 | — (extends B1/B4/B5) | `app_p2p.c` + central |

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

### B2 — Sliding-hour duty ledger

The original model was a single "blocked until" timestamp: after each TX the device was
hard-blocked for 99× the airtime (a 240 B SF10 frame blocked for **~227 s**). B2 first replaced
that with a TOWER-style **token bucket** (36 000 ms/hour for EU 1 %, sub-millisecond residue
carry), which held the hourly *average* at 1 % while still permitting a legitimate burst — an
alarm right after a telemetry frame.

The control-radio completion work (#416) then **withdrew the bucket** for an **exact
sliding-hour ledger**. The bucket held only the long-run average, so a node idle for an hour
could then burst all 36 s of air at once — a simulated 24 h continuous run reached **2.00 %** in
the worst sliding hour. #408 had documented that as an accepted trade-off; it was reversed
because amortised compliance is not what the regulation asks for, nor what a certification
review accepts. The ledger records `(end_time, air_time)` per transmission and admits a frame
only when the air already inside the trailing hour plus that frame fits `P2P_DUTY_BUDGET_MS`, so
**every** sliding one-hour window sums to ≤ 1 % exactly — while keeping the bucket's latency
behaviour (a frame goes the moment there is room, not after a fixed post-frame penalty).

Costs, both documented in `doc/p2p.md` §6: **+384 B RAM** (48 × 8 B entries, replacing the
bucket's 16 B) and a **~48 uplinks/hour** bound before the ring — not the air budget — becomes
the limiting factor (a safe direction: a full ring can only delay a frame, never permit one the
budget forbids; matters only for bench cadences below ~75 s). Still RAM-only, so a reboot loop
can exceed 1 % — the same hole the bucket had, kept for the same reason (persisting would cost
an NVS write per frame).

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

**Implemented (#416):** `0x56` is dispatched through `app_cmd_handle(APP_CMD_TRANSPORT_P2P, …)`
with responses on `app_p2p_queue_response()`, and **deferred actions now execute** after the
`0x55` (mirrors `app_lrw`'s post-cmd handler: stash, wait `POST_CMD_DRAIN_WAIT_SEC`, re-defer up
to 6× while the response is undelivered, then run regardless). A bench stack overflow from
running command dispatch on the 2 KB P2P work queue was fixed by sizing it to 4 KB (+2 KB RAM).
The oversized-RX-window gotcha above is separately solved by **B13** (exact RX1 window from the
Ack's `pending_frame_len`, #416).

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

### B8 — History replay over P2P (device-driven stream)

**Goal: feature parity.** Everything the LoRaWAN transport exposes must also work over P2P.
History replay is the one remaining hole: telemetry, alarms and command responses already go
through the `app_radio` facade (`app_p2p_send_telemetry` / `app_p2p_send_alarm` /
`app_p2p_queue_response`), but history replay does not — it is wired straight to LoRaWAN.

Today `app_cmd_handle_req_history()` is compiled under `#if defined(CONFIG_LORAWAN)` and calls
`app_lrw_start_history_replay()` directly (`app_cmd.c`), and the command dispatcher rejects any
transport other than LRW (`app_cmd.c`, `tp != APP_CMD_TRANSPORT_LRW`). There is no
`app_p2p` replay path at all — `app_p2p.c` has zero history code.

**Design — device-driven stream, mirroring LoRaWAN** (user decision 2026-09-11). We reuse the
existing replay engine rather than inventing a pull protocol:

1. **Promote replay into the `app_radio` facade.** Add `app_radio_start_history_replay(from, to,
   seq)` that dispatches to `app_lrw_start_history_replay()` (LoRaWAN) or a new
   `app_p2p_start_history_replay()` (P2P), the same shape as the other three facade calls.
2. **`app_p2p_start_history_replay()`** streams the matching records back as N `HistoryFrame`
   protobuf frames — the identical encoder used by the LoRaWAN path (`history_frame_cap()` +
   `app_history_export_*`), just emitted as P2P uplinks instead of port-85 LoRaWAN uplinks.
   `app_history_set_replay_active(true)` still self-skips capture during the stream (#126).
3. **Duty compliance.** Each frame is charged through the B2 sliding-hour duty ledger and
   sized to `app_p2p_get_max_payload()`; the stream yields when the bucket is empty and resumes
   on refill (no busy-wait), the P2P analogue of the LoRaWAN MAC-busy retry loop.
4. **Command plumbing.** Drop the `#if defined(CONFIG_LORAWAN)` in `app_cmd_handle_req_history()`
   so it routes via the facade, and add `APP_CMD_TRANSPORT_P2P` to the `req_history` allow-list
   in the dispatcher. The first frame is the reply (`which_body` stays 0, no redundant Ack), as
   on LoRaWAN.
5. **Central (S5).** The central must issue `ReqHistory` over the `0x56` COMMAND channel (B4)
   and collect the resulting `HistoryFrame` stream. Added to proximos-v2 MR !30 as S5.

**Non-goal (still v2):** a *generic* pull-based bulk-transfer primitive (TOWER's
`BulkSource`/`BulkSink`, verified to 64 KB in constant RAM) for FUOTA and large-object fetch.
History replay does not need it — the device-driven stream above is enough — so that primitive
stays deferred and is recorded here as the shape to adopt when FUOTA returns.

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

### B10 — Transport parity via facade

Enforce the governing principle above: everything the LoRaWAN transport exposes must be
reachable over P2P through the `app_radio` facade, or be a documented, deliberate exception.
A code audit (2026-09-11) found the data plane already at parity — telemetry, alarms and
command responses all route through `app_radio_send_telemetry` / `app_radio_send_alarm` /
`app_radio_queue_response`, and both stacks implement the full facade surface — but three
behaviours still bypass the facade and are LoRaWAN-only (a fourth, downlink command execution,
was closed by #416 — see B4):

| Gap | Today | Fix |
|---|---|---|
| **History replay** | `app_cmd_handle_req_history()` calls `app_lrw_start_history_replay()` directly under `#if defined(CONFIG_LORAWAN)` | covered by **B8** (adds `app_radio_start_history_replay()`) |
| **GetInfo-on-join** | `queue_info_uplink()` lives in `app_lrw.c` and fires only on a LoRaWAN join | add a facade link-ready **announce hook** so P2P also announces identity/firmware on pairing (`mark_ready()`) |
| **`force_send`** | handler body is `#if defined(CONFIG_LORAWAN)`; dispatcher rejects `tp != LRW` | route through `app_report_trigger()` (already facade-backed) and add `APP_CMD_TRANSPORT_P2P` to the allow-list |

And the command dispatcher (`app_cmd.c`) excludes P2P from several commands LoRaWAN already
accepts. Widen the `tp != …` allow-lists to include `APP_CMD_TRANSPORT_P2P` for the ones that
are transport-neutral: **`force_send`, `sample`, `buzzer_play`, `enter_calibration`**, and
reconcile **`clock_sync`** (either accept the command over P2P too, or document B5's `0x56`
time-tail as the P2P equivalent so the two mechanisms are not silently divergent). P2P reuses
the LoRaWAN over-the-air command gating (positive `tp ==` allow-lists in the generated
`app_config_ingest.c`), so opening these does not widen the write surface beyond what LoRaWAN
already permits.

**Deliberate exceptions (stay transport-specific, documented here):**

- `lrw_join` / `lrw_reset` — inherently LoRaWAN; P2P has its own join/rejoin (`app_p2p_start` /
  `app_p2p_rejoin`) reached through `app_radio_start` / `app_radio_rejoin`.
- `device_reset`, `factory_reset`, `set_secret_key`, `clm_ack`, `clm_rearm`, `vendor_reset`,
  `req_history_page` — local provisioning / NFC / vendor commands, not part of the LoRaWAN
  over-the-air surface, so no P2P parity is owed. LoRaWAN does not accept them either.

The outcome is that parity becomes a checked invariant, not an accident: after B10 every
LoRaWAN-capable function is either facade-routed (works everywhere) or on the exception list.

### Beyond the plan — landed in #416

The control-radio completion work added three protocol features the original proposal did not
call out. They are recorded here so the plan's inventory matches the merged code.

#### B11 — Detach (0xFD) and RejoinRequest (0xFE)

Central-initiated session control. The central had always sent Detach on node-remove, but
`recv_ack()` accepted only `0xFA`/`0x56` and dropped it — so a removed node kept retrying into
a dead session and, after 8 failed cycles, **self-healed into an endless rejoin loop against an
unregistered serial**. Both frames are empty-bodied, authenticated under `session_key` with
direction RX and the acknowledged uplink's counter (15 B on air), so neither is forgeable and
the single-use counter rules out replay.

- **Detach → `pairing_clear()`** (factored out of `app_p2p_unjoin()`, compiled
  unconditionally): drop to UNPAIRED, purge TX + Ack-retry queues, clear `m_started` so
  `app_report` skips uplinks while its timer keeps running. **No auto re-join** — the node was
  removed deliberately, so it stays silent until a reboot or explicit `join`. `dev_nonce` is
  untouched, so a later re-registration still authenticates.
- **RejoinRequest → `start_join_episode(true)`** (the self-heal policy: exempt from the 120 s
  boot window, 60 s → ×2 → 1 h backoff) so a paired node asked to rekey keeps trying.

#### B12 — Per-node TX-power via the JoinAccept reserved bytes

`JoinAccept.reserved(4)` — previously discarded — is now
`channel_idx(1) | sf(1) | tx_power(1) | flags(1)`. `tx_power` (2–22 dBm, bounded by the node's
own `p2p_tx_power`; **0 = no assignment**) is applied to the session, **persisted with the
pairing** (survives reboot without a re-join), reported by `ats radio status` as
`tx power: <n> dBm (assigned|config)`, and preferred over local config in
`build_modem_config()` — the central owns the link budget, the node owns its default.
`channel_idx` must be 0 and `sf` is a documented hook (both logged and ignored — the NorthBridge
has a single receiver, so SF is network-wide); every unsupported/out-of-range field is warned
and ignored rather than refused, since declining to pair over an unhonourable byte would strand
an otherwise-valid authenticated JoinAccept. All-zero (what the central sends until
`node_tx_power_dbm` is configured) means "no assignment", so it is compatible with the central
as it ships. `p2p_parse_join_accept_reserved()` is a pure, tested function.

#### B13 — Exact RX1 window from the Ack's `pending_frame_len`

The announcing Ack now carries the pending `0x56`'s total on-air length, so the node sizes its
next RX1 window for exactly that frame instead of a 255 B worst case (this driver has no
hardware symbol timeout, so a short window aborts a real command mid-reception). At SF10 a 2 B
GetInfo drops receiver-on from **2434 ms to 468 ms**. This closes the oversized-window gotcha
noted under B4. The Ack body becomes
`flags(1) | rssi(i8) | snr(i8) | [pending_frame_len if bit0] | [unix_be32 if bit1]` (valid
lengths 3/4/7/8), extending the B1/B5 format. `p2p_parse_ack_body()` is restructured around the
length (the flags only refine it), which is what lets the byte be adopted **without a wire
version**: a legacy 3/7-byte body with bit 0 set still parses as "pending, length unknown" and
the node keeps the 255 B fallback until the central emits the byte.

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
| Downlink | pending flag → chained RX windows, remote shell over the air | pending flag → chained RX; `0x56` COMMAND dispatched and deferred actions (save/reboot) executed after the `0x55` | **parity** | B4 landed the full bidirectional path — dispatch + deferred execution mirroring `app_lrw` post-cmd |
| Duty / compliance | token bucket with residue carry, LBT/AFA (EU), FHSS (US), runtime band switch | exact sliding-hour ledger (≤1% per window, stricter than the bucket's average), EU-only, no LBT | **TOWER** | TOWER still wins on LBT/FHSS/multi-region; P2P's duty accounting (B2) is now exact per window, not amortised → B6, B7 |
| Bulk transfer | pull-based, constant RAM, verified to 64 KB | history replay is device-driven (LoRaWAN today; P2P via B8), no generic pull primitive | **TOWER** for a generic primitive (FUOTA, v2); history replay reaches P2P parity device-side via B8 |
| Link diagnostics | RSSI/LQI/SQI/AFC per packet, channel RSSI scan | RSSI/SNR logged, shown in `ats radio status` | **TOWER** | richer link telemetry; adopt partially via B1 |
| Gateway / central model | stateful dongle gateway, registry in EEPROM | stateless keyless gateways + one central (FIBER v2), multi-gateway dedup and roaming | **P2P** | scales to many gateways, keeps all state in one place, and gateways hold no keys |
| Testability | pure decision kernels split into host-testable `no_std` crates (`tower-net-core`, `tower-radio-core`) | `app_p2p.c` is HW/HIL-only; no native ztest suite covers it | **TOWER** | all regulatory arithmetic and security accept/reject logic unit-tested on the host. Strong argument for extracting our P2P decision logic into a testable core |

**Summary:** STICKER P2P wins on radio reach, framing, addressing, key hygiene, and network
architecture. TOWER wins on operational maturity — duty-cycle compliance, downlink,
diagnostics, counter hardening, and testability. Track B is the list of TOWER's wins worth
importing.

---

## 5. Next phase — a `radio-mode tower` transport (moved to PR #410)

The TOWER transport (fourth `radio_mode` value, the `app_tower` module, the verified GFSK
PHY path, the bench interop gate) moved to its own PR so the P2P hardening and the TOWER
work are separate, independently reviewable streams:

**[PR #410](https://github.com/hardwario/sticker-firmware/pull/410)** —
`doc/plan/410 - TOWER GFSK transport (radio-mode tower).md`.

One dependency stays shared: #410's Step 1 (radio-driver FSK access) and B7 here (CAD) are
**one driver change** — design them together, open the driver once.

Section numbering below is kept stable to preserve cross-references.

---

## 6. Priorities and tracking

The scope of this PR is the P2P work; the LoRaWAN items (A1–A7) are tracked in
[PR #409](https://github.com/hardwario/sticker-firmware/pull/409) and the TOWER transport in
[PR #410](https://github.com/hardwario/sticker-firmware/pull/410).

**Quick wins**

- [x] B2 — sliding-hour duty ledger ✅ (first landed as a token bucket, replaced by the exact ledger in #416; + Step 1 native test suite)
- [x] B3 — self-healing rejoin ✅
- [x] B1 — ACK carries RSSI/SNR ✅ (central S1 done)

**Medium** — needs central/gateway coordination or a larger change

- [x] B9 — counter/replay hardening audit ✅
- [x] B4 — pending-downlink chaining + `0x56` COMMAND dispatch ✅ (central S2 done; deferred-action execution now runs after the `0x55` — #416, mirrors `app_lrw` post-cmd; dispatch work queue sized 2→4 KB after a bench stack overflow)
- [x] B5 — clock sync over P2P ✅ (central S3 done)
- [ ] B8 — history replay over P2P (device-driven stream) *(reuses the LoRaWAN `HistoryFrame` encoder via a new `app_radio` facade call; needs central S5)*
- [ ] B10 — transport parity via facade *(GetInfo-on-join announce hook + `force_send`/`sample`/`buzzer_play`/`enter_calibration` allow-lists; B4 deferred-action execution already landed in #416; enforces the §3 governing principle)*

**Landed in #416, beyond the original proposal**

- [x] B11 — Detach / RejoinRequest (central-initiated session control) ✅
- [x] B12 — per-node TX-power via the JoinAccept reserved bytes ✅
- [x] B13 — exact RX1 window from the Ack's `pending_frame_len` ✅

**Large — design and compliance first**

- [ ] B6 — multi-channel / P2P region model *(blocked on the regulatory decision, §3 B6; needs central, S4)*
- [ ] B7 — CAD / listen-before-talk *(Zephyr driver patch)*

**Next phase:** the TOWER transport is tracked in
[PR #410](https://github.com/hardwario/sticker-firmware/pull/410) (§5).

**Deferred / v2:** a generic pull-based bulk-transfer primitive (FUOTA, large-object fetch) —
history replay itself is now in scope as B8.

Server-side counterparts for B1, B4, B5, B6, and B8 are defined in
[proximos-v2 MR !30](https://gitlab.hardwario.com/proximos/proximos-v2/-/merge_requests/30)
as S1–S5.

Suggested order: B2 and B3 are self-contained device-side changes with no external
dependency, so they can land first. B1 needs the central to key the ACK size off the session
`proto_version`, so it should land alongside S1. B7 and #410's Step 1 share the same Zephyr
driver work and should be planned together.

Section 7 breaks this into ordered, commit-sized steps.

**Live HIL (2026-08-28):** B1, B5 and the join + data plane are validated end-to-end against
the Proximos central (real STICKER ↔ northbridge ↔ `control-radio`, MR!30); B4's 0x56 downlink
radiated but the 0x55 round-trip is unconfirmed live. Full results in
`doc/p2p-e2e-test-plan.md` §6.

## 7. Implementation steps

One step per commit, in order. Each step states what changes, which files, and how it is
verified. Steps 1–4 have no external dependency; 5–7 need their server-side counterpart in
[MR !30](https://gitlab.hardwario.com/proximos/proximos-v2/-/merge_requests/30) to land first.

Standard verification for every step, unless noted: the three build configs (Release
dual-stack, `debug.conf`, `debug.conf + debug_p2p_bench.conf`), `bash tests/run_native.sh`,
and `clang-format --dry-run --Werror`.

### Step 1 — a native test suite for the P2P logic ✅ DONE

**Why first:** `app_p2p.c` had *no* native test coverage. `tests/p2p/` is a bench firmware
for a second STICKER, not a ztest suite (no `testcase.yaml`, so `run_native.sh` skips it).
Every P2P change below was verifiable only on a bench. This step buys a safety net before
anything moves, and is TOWER's host-testable-decision-kernel lesson (§4) applied to us.

- **Everything stays in `app_p2p.*`** (no new production source file — an initial extraction
  into `app_p2p_wire.{c,h}` was reverted on request). The pure helpers stay `static` in the
  firmware and are given external linkage **only under `CONFIG_ZTEST`** (a `P2P_TESTABLE`
  macro + a guarded declaration block in `app_p2p.h`), the same idiom as `app_cmd.c`'s
  existing CONFIG_ZTEST hook. Frame-geometry + duty constants and `struct p2p_duty` moved into
  `app_p2p.h` as the shared single source of truth.
- Added `tests/p2p_logic/` which **compiles the real `app_p2p.c`** against a no-op fake LoRa
  device (`src/emul_lora.c` + a test-local DTS binding/overlay, like `tests/nfc_hw`) and thin
  stubs (`src/stubs.c`: `g_app_config`, `app_compose_*`); `app_ccm.c` + soft-SE linked for
  real crypto. Picked up automatically by `run_native.sh`. Tests: ToA reference/monotonicity,
  nonce layout + direction, frame codec round-trip + tamper + max body.
- Verified: 12/12 native suites, all three build configs, clang-format 22.1.5 clean.

### Step 2 — B2: sliding-hour duty ledger ✅ DONE

Replaced the single `m_dc_blocked_until` deadline with a per-frame duty governor. It first
landed as a Tower-style token bucket (36 000 ms cap, µs residue carry); the control-radio
completion work (#416) then replaced it with an **exact sliding-hour ledger**, because the
bucket held only the 1 % *average* — a simulated 24 h run reached 2.00 % in the worst sliding
hour, which a certification review will not accept.

- Final state: `struct p2p_duty` holds a ring of `(end_time, air_time)` entries;
  `p2p_duty_wait_ms(wire_len)` / `p2p_duty_charge(air)` admit a frame only when the trailing
  hour plus that frame fits `P2P_DUTY_BUDGET_MS = 36000`. `p2p_duty_refill()` and
  `P2P_DUTY_PERMILLE` are gone; `p2p_duty_init()` needs no timestamp.
- All nine former `m_dc_blocked_until` sites gate/charge/wait per-frame (each knows its frame
  size — the "coarse gate" wrinkle from the original plan proved unnecessary).
- `app_p2p.c` + `doc/p2p.md` §6 updated: every sliding hour is ≤ 1 % exactly; the costs
  (+384 B RAM, ~48 uplinks/h bound) are documented.
- Tests: the six bucket tests were replaced by six ledger tests, including a 24 h property
  check over every window that rejects a reference bucket at 2.00 %. 28/28 in `p2p_logic`.

**Verify (both steps):** 12/12 native suites; Release dual-stack + `debug.conf` +
`debug.conf+debug_p2p_bench.conf` all build; clang-format 22.1.5 clean.

### Step 3 — B3: self-healing rejoin ✅ DONE

The link-loss signal existed but nothing acted on it (the ACK give-up branch only logged).

- Added `m_consec_uplink_fail` (+ `m_self_healing`, `m_rejoin_attempt`): incremented at both
  give-up branches (`note_uplink_cycle_failed()`), reset on any Ack (`note_uplink_acked()`,
  wired into `send_confirmed` and `ack_retry_work_handler`) and on pairing (`mark_ready`).
- At `P2P_REJOIN_FAIL_THRESHOLD = 8` an already-PAIRED node self-heals: a shared
  `start_join_episode(self_healing=true)` drops to JOINING and schedules the join work. The
  trigger fires only while PAIRED, so once re-joining it can't re-trigger itself.
- `app_p2p_rejoin()` (shell) and `app_p2p_start()` (boot) both route through the same
  `start_join_episode(false)`; the self-heal path is compiled unconditionally (Release can
  self-heal — no CONFIG_SHELL dependency). `app_key_is_set()` guard preserved.
- `join_work_handler` now branches on `m_self_healing`: self-heal is exempt from the 120 s
  boot-window cap (§5.2/§7) and uses exponential backoff `p2p_rejoin_backoff_ms(attempt)`
  (60 s → ×2 → 3600 s, pure/testable) with ±25 % jitter; the boot/shell join keeps its
  window cap + tight jitter.

**Verified:** `p2p_logic` covers the backoff curve (double-then-cap, monotonic, saturates).
Real link loss stays a bench test — `ats radio ack_drop <n>` forces the give-up path.

### Step 4 — B9: counter and replay hardening audit ✅ DONE

- **Fail-closed** (fix): `fcnt_reserve()` now advances the in-RAM watermark ONLY after the
  durable `settings_save_one` succeeds, and `fcnt_next()` returns an errno (refuses TX) if a
  needed reservation can't be persisted — was previously bumping the watermark before the
  save and ignoring its failure, so a reboot could reuse a counter. `tx_frame()` propagates
  the refusal.
- **Saturate, don't wrap** (fix): `fcnt_next()` returns `-EOVERFLOW` at `UINT32_MAX` instead
  of wrapping (a wrap repeats every `(key, nonce)`); reserve target clamped so it can't
  overflow near the ceiling.
- **Verify-then-classify** (already correct): `recv_join_accept()` verifies the CMAC tag
  before `pairing_persist()`; `recv_ack()` authenticates before returning and mutates no
  persistent state. Device-side has no replay window to poison (the central holds it).
- **Counter consumed at seal time** (already correct): `fcnt_next()` runs in `tx_frame()`
  before build/send; retries reuse the same counter via `tx_frame_at()`, so a failed send
  burns a counter rather than reusing one.

**Verified:** `p2p_logic` fcnt cases — normal advance, fail-closed on a failed reserve (the
`CONFIG_SETTINGS_NONE` backend makes saves fail), and saturation at `UINT32_MAX` (via
CONFIG_ZTEST hooks `p2p_test_set_fcnt`/`p2p_test_fcnt_next`).

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

### Step 8 — B8: history replay over P2P *(needs S5)*

Bring history to feature parity with LoRaWAN. Add `app_radio_start_history_replay(from, to,
seq)` to the facade and a new `app_p2p_start_history_replay()` that streams the matching records
as N `HistoryFrame` frames — reusing `history_frame_cap()` + `app_history_export_*`, each frame
charged through the B2 duty ledger and sized to `app_p2p_get_max_payload()`, yielding/resuming
on the ledger rather than busy-waiting. Route `app_cmd_handle_req_history()` through the
facade (drop the `#if defined(CONFIG_LORAWAN)`) and add `APP_CMD_TRANSPORT_P2P` to the
`req_history` allow-list in the dispatcher. Test natively in `tests/p2p_logic` against the
emul-LoRa device: a `ReqHistory` produces the expected frame count and yields under a starved
duty bucket. Pairs with Step 6 (both ride the `0x56` COMMAND channel).

### Step 9 — B10: transport parity via facade

Close the remaining facade-bypass gaps found in the 2026-09-11 audit (§3 B10). Land in small,
independent commits:

1. **GetInfo-on-join → facade.** Lift `queue_info_uplink()` out of `app_lrw.c` into a shared
   link-ready announce that the facade invokes for whichever stack just became ready, so P2P
   announces identity/firmware on `mark_ready()` the way LoRaWAN does on join.
2. **`force_send` radio-agnostic.** Drop the `#if defined(CONFIG_LORAWAN)` in the handler (it
   already calls the facade-backed `app_report_trigger()`) and add `APP_CMD_TRANSPORT_P2P` to
   its dispatch allow-list.
3. **Widen allow-lists.** Add `APP_CMD_TRANSPORT_P2P` to `sample`, `buzzer_play` and
   `enter_calibration`; reconcile `clock_sync` (accept over P2P or document B5 as its P2P
   equivalent). No new write surface — P2P reuses the LoRaWAN `tp ==` gating.
4. **Deferred downlink action execution — already done in #416** (`6d855b0`): a `0x56` COMMAND
   that schedules a save/reboot now runs after the `0x55`, mirroring `app_lrw`'s post-cmd
   handler (settings_save, reboot, reset_counters, lrw_reset and lrw_join are ungated for P2P;
   lrw_join is refused in P2P mode, lrw_reset honoured only where the LoRaWAN stack is compiled
   in). No further work in this step.

Each sub-step is verifiable in `tests/p2p_logic` (command accepted over P2P, right action
emitted) plus the three build configs and clang-format. After this step, "does every
LoRaWAN function work over P2P?" is answered by the §3 B10 table, not by inspection.

### Later — design and compliance gated

- **B6 (P2P region model)** is blocked on the regulatory decision in §3: an SF10/BW125 frame
  is ~2.3 s of air, which AU915's 400 ms dwell forbids outright. Needs a certification-lab
  answer before the server-side channel-assignment policy (S4) can be finalised.
- **B7 (CAD/LBT)** and **#410's TOWER GFSK PHY** both need the same in-workspace Zephyr LoRa
  driver extension, so plan them as one piece of work rather than opening that driver twice.

### Not in this PR

Track A is LoRaWAN work in `app_lrw.c` and moved to
[PR #409](https://github.com/hardwario/sticker-firmware/pull/409) against `v1.5.0`; the TOWER
transport moved to [PR #410](https://github.com/hardwario/sticker-firmware/pull/410) against
`feat-p2p`. Each has its own plan and ordered steps.

## 8. Explicit non-goals

- **Class C / multicast** — STICKER is a battery device and Class A only is a deliberate
  choice; LTO strips the Class B/C code paths today.
- **FHSS for P2P in EU** — TOWER's FHSS exists to satisfy FCC §15.247. Under ETSI the duty
  cycle model is sufficient, and FHSS would add complexity for nothing.

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
