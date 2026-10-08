# STICKER P2P radio — TOWER protocol end to end

*Status: current as of 2026-10-06 (PR #470, phase P3). Node FW: `feat-p2p` + #470;
Northbridge 0.3.2; central `control-radio` c69.*

This document describes how the P2P radio behaves **end to end**: between a STICKER Node
and the Northbridge (NB) gateway on air, and between the NB, the Hub central and the
Portal. It is an overview. The normative details live in:

| Topic | Normative source |
|---|---|
| Node firmware (wire format, windows, keys, lifecycle) | [`doc/p2p.md`](p2p.md) |
| Design decisions, phases, test results | [`doc/plan/470 - TOWER protocol as the P2P transport.md`](plan/470%20-%20TOWER%20protocol%20as%20the%20P2P%20transport.md) |
| NB ↔ Hub link, central adapter, outbox | proximos-v2 `plan/control/radio/p2p_tower_gateway.md` (sticker copy: [`doc/plan/470 - Northbridge-Hub link protocol.md`](plan/470%20-%20Northbridge-Hub%20link%20protocol.md)) |

---

## 1. Roles

```
 STICKER Node ──air (TOWER over LoRa)──▶ Northbridge ──UART (HDLC)──▶ Hub central ──MQTT──▶ Portal
   app_radio_p2p                           STM32WL55 gateway            control-radio            
```

| Role | What it does | Holds |
|---|---|---|
| **Node** (STICKER) | measures, sends confirmed uplinks, joins, applies downlinks | `radio_appkey`, the session key, its TX counter |
| **Northbridge** (NB) | TOWER gateway net layer: CCM, replay check, **local ACK in 20 ms**, downlink queue per Node, answers `LinkCheckReq` / `TimeReq` in the ACK | session keys **in RAM only** (fed by the central), its own TX counter block |
| **Hub central** (`control-radio`) | app_key registry, join, session keys, decode, persistence, downlink retries, northbound publishing | app keys, `sessions.db`, the outbox |
| **Portal** | shows readings, sends commands, triggers history backfill | nothing radio-specific |

The NB never talks to the Portal directly. Everything the Portal sees passes through the
central, in the same northbound documents as a LoRaWAN Node (`transport = p2p`).

---

## 2. Node ↔ Northbridge (air)

### 2.1 Radio

Fixed per network. There is no ADR and no power or SF control.

| | Value (default) |
|---|---|
| Modulation | LoRa, BW 125 kHz, CR 4/5, preamble 8, explicit header, CRC on |
| SF | `p2p-spreading-factor` = SF7 |
| Channel | `p2p-frequency` = 869.525 MHz (h1.6, 10 % duty), both directions |
| RX gain | boosted (0x96, ~+2 dB) on the Node and the NB |
| TX power | `p2p-tx-power` = 14 dBm, or the JoinAccept's assignment (the central sends none today) |
| Frame MTU | 100 B (the SX126x 4 s TX timeout caps `lora` there) |

`fsk` (TOWER-native GFSK) is accepted by the config but refuses to start until phase P5.

### 2.2 Frame

```
ver_type(1) | flags(1) | src(4 LE) | dest(4 LE) | counter(4 LE) | ciphertext | tag(8)
```

- `ver_type` = `version << 5 | type` (version 1, type 0 Data, 1 Ack); `flags` b0 =
  CONFIRMED.
- Addresses: the Node = low 32 bits of its DevEUI, the gateway = `net_id`, a JoinRequest
  goes to `dest = 0`. 0 and `0xFFFFFFFF` are reserved.
- AES-128-CCM, 8 B tag, AAD = the 14 B header, nonce = `src ‖ counter ‖ 0*5`.
- `counter` strictly increases per sender and session, 0 is reserved.
- 100 B frame → 78 B plaintext → **76 B body** after the envelope.

### 2.3 Envelopes

```
0x81 | port | LoRaWAN fPort payload        data
0x91 | { id | len | value } x n            control TLVs
```

- `0x81` ports = the LoRaWAN ones: 2 telemetry, 3 alarm, 85 answers, 86 commands (down).
  The body is byte-identical to LoRaWAN, so one decoder serves both radios.
- `0x91` IDs: `0x01` Capabilities ↑, `0x02` Hello ↑, `0x03` Detach ↓, `0x04` RejoinReq ↓,
  `0x07` JoinReq ↑, `0x08` JoinAccept ↓, `0x10` LinkCheckReq/Ans, `0x20` TimeReq/Ans.
  Unknown IDs are skipped by length.

### 2.4 Join

```
Node                                   NB                         central
 │ JoinRequest (dest 0, dev_nonce)  ──▶│ EVT_RX ─────────────────▶│ verify, central_nonce,
 │                                     │                          │ session_key
 │                                     │◀── TX_SCHEDULE(t_rx+1 s) │
 │◀── JoinAccept (net_id, nonce, …) ───│                          │ commit session,
 │                                     │◀── TWR_NODE_ADD ─────────│ hand key to NB
 │ derive session_key, persist         │                          │
 │ Capabilities + Hello (+TimeReq) ──▶ │ ACK ...                  │
```

- Keys: `join_key = CMAC(app_key, "HIO-TWR-JOIN" ‖ 1 ‖ dev_eui)`, `session_key =
  CMAC(app_key, "HIO-TWR-SES" ‖ 1 ‖ dev_nonce ‖ central_nonce ‖ dev_eui)`. Only the
  `app_key` is a root secret; no key goes on air.
- The Node persists the next `dev_nonce` **before** sending; the central accepts only a
  `dev_nonce` above the stored one.
- JoinAccept arrives `rx_delay` = 1 s after the JoinRequest. The central commits the
  session only after the NB accepted the `TX_SCHEDULE`.
- Join policy on the Node: fast retries for 120 s after boot, then the common backoff
  60 s → ×2 → 1 h. A normal reboot resumes the persisted pairing without a join.
- After a join the Node announces: Info → settings → first telemetry, plus the `0x91`
  Capabilities + Hello.

### 2.5 Uplink and ACK

- **Every uplink is confirmed** (telemetry, answers, history, `0x91`; alarms per
  `radio-alarm-ack`). A downlink therefore waits at most one report interval.
- A confirmed send = up to **3 byte-identical transmissions** (same counter), each
  followed by an ACK window from TX-done (SF7 200 ms). Unanswered, app_radio retries up to
  3 × **under the same counter** while no other frame took one since (otherwise a new
  counter); a heard frame whose ACKs were all lost is then a retransmission at the NB,
  not a second report.
- The NB checks the frame (header → CCM → counter) and ACKs **20 ms** after RxDone:

  ```
  ACK payload: acked(4) | rssi(i8) | flags(1) [| TLV tail]
               flags b0 = PENDING (a downlink follows), b1 = CTRL (a TLV tail follows)
  ```

- Replay on both sides: counter above the last seen = fresh; equal = retransmission,
  re-ACKed but not delivered again; lower = dropped.
- The NB forwards a fresh frame to the central **after** the ACK is scheduled.

### 2.6 Answers in the ACK (`LinkCheckAns`, `TimeAns`)

A confirmed `0x91` uplink with `LinkCheckReq` and/or `TimeReq` gets its answers in the
ACK itself, as the CTRL tail (plan §13.5). The Node needs no extra receive window.

| Answer | Body | Built by the NB from |
|---|---|---|
| `0x10` LinkCheckAns | `rssi i8, snr i8, margin i8, gw_count = 1` | the received frame; margin = SNR − SF floor (−7.5 dB at SF7) |
| `0x20` TimeAns | `unix(4) ‖ frac(1/256 s) ‖ req_counter(4)` | the wall time at the end of this frame, from the central's `TWR_TIME_SYNC` anchor |

- Fixed order `0x10`, `0x20`. The tail adds at most 17 B (SF7 +25.6 ms), and only to the
  ACK of a request; a plain ACK is unchanged.
- No anchor on the NB → no TimeAns; the Node asks again later.
- The Node applies TimeAns relative to the TX-done of the request (clock within ±1 s).
- A downlink after PENDING can follow in the same exchange: tail first, then the
  command.
- An older NB (status 172 / 171 B) does not answer in the ACK; the central then queues
  the answers as `0x91` downlinks, delivered after the next PENDING.

### 2.7 Downlink

- The central queues the downlink on the NB. On the Node's next confirmed uplink the NB
  sets **PENDING**, and the downlink goes out `dl_gap_ms` (20 ms) after the ACK.
- The Node keeps its receiver on for **one** Data frame (SF7 221 ms).
- Commands (`0x81` port 86) are sent **confirmed**: the Node ACKs them 20 ms after
  RxDone under its own next counter.
- `0x91` downlinks (answers, Detach, RejoinReq) are unconfirmed.
- A downlink not delivered is retried by the central, not the NB (§3.6).

### 2.8 Link supervision and lifecycle

- A link check is the **link-check report** (every `radio-link-check-interval`-th report
  and the first after link-up) plus its `LinkCheckReq`. Only a link-check report with no
  ACK counts as a failure; a lost plain report, alarm or answer does not.
- 3 failures in a row → WARNING (no rung: nothing to tune). `radio-link-check-fail-rejoin`
  (5) more → self-heal rejoin on the slow policy. The session persists on both sides
  through an RF outage.
- A paired Node with no telemetry for 4 × `interval_report` rejoins (shared with
  LoRaWAN).
- **Detach** clears the pairing; the Node stays silent. **RejoinReq** starts a new join
  (rekey).
- Reset tiers (`factory_reset`, …) clear the pairing; `dev_nonce` and the TX counter
  survive.

### 2.9 Duty cycle and timing

- The Node charges every transmission to a sliding-hour ledger (EU868 sub-band of
  `p2p-frequency`); a held send returns `-EAGAIN`, nothing goes on air.
- The NB counts its own airtime (`airtime_ms_last_hour`); report-only today.

| Frame | SF7 ToA |
|---|---|
| empty Data (22 B) | 57 ms |
| ACK (28 B) / ACK with full tail (45 B) | 67 / ~93 ms |
| JoinReq / JoinAccept | 83 ms |
| MTU (100 B) | 175 ms |

---

## 3. Northbridge ↔ Hub central (UART link)

### 3.1 Framing

- UART 115200 8N1 (bench: the same stream over J-Link RTT).
- HDLC-style `0x7E | escaped payload | 0x7E`, payload `type ‖ seq ‖ body ‖ crc16`
  (CRC16-CCITT-FALSE), all integers little-endian.
- `type`: commands `0x01–0x3F`, responses `0x40 | cmd` with a status byte, events
  `0x80–0xBF`. Response timeout 800 ms.

### 3.2 Commands (central → NB)

| ID | Command | Purpose |
|---|---|---|
| `0x01` | `GET_INFO` | full status: version, `now_ms` clock, state, radio config, 25 counters, ring and queue use, counter block, `evt_head_seq`, ACK-tail counters |
| `0x02` | `SET_RADIO_CONFIG` | frequency, SF, BW, CR, preamble, TX power |
| `0x04` | `RX_STOP` | leave gateway mode, keep table and queue |
| `0x05` / `0x06` | `TX_SCHEDULE` / `TX_CANCEL` | JoinAccept at a given time (sealed by the central) |
| `0x10` | `TWR_START` | enter gateway mode (`net_id`, turnaround, DL gap) |
| `0x11` / `0x12` | `TWR_NODE_ADD` / `REMOVE` | peer table: addr, session key, `last_seen`, `HOME`, `KEEP_NEWER` |
| `0x13` | `TWR_CTR_BLOCK` | the NB's TX counter block (the central persists it first) |
| `0x14` / `0x15` | `TWR_QUEUE_PUSH` / `DROP` | downlink queue, idempotent by `(addr, item)` |
| `0x17` | `TWR_NODE_LIST` | peer table readout (never keys) |
| `0x18` | `TWR_EVT_ACK` | cumulative ack of ring events (no response) |
| `0x19` | `TWR_TIME_SYNC` | clock anchor `{now_ms, unix_ms}` for TimeAns in the ACK |

### 3.3 Events (NB → central)

| ID | Event | Delivery |
|---|---|---|
| `0x80` | `EVT_BOOT` | best-effort → the central restores the NB |
| `0x81` | `EVT_RX` | best-effort: JoinRequests and frames from unknown addresses |
| `0x82` | `EVT_TX_DONE` | best-effort: `TX_SCHEDULE` outcome |
| `0x83` | `EVT_LOG` | best-effort, rate-limited |
| `0x84` | `EVT_TWR_UPLINK` | **ring**: a fresh, verified uplink (`t_ms`, RSSI, SNR, addr, counter, `ack`, plaintext) |
| `0x85` | `EVT_TWR_TX` | **ring**: downlink outcome (`DELIVERED`, `SENT`, `NOT_DELIVERED`, `EXPIRED`, `RADIO_ERR`, `NO_COUNTER`) |
| `0x86` | `EVT_TWR_CTR_LOW` | best-effort: counter block below 25 % |

`EVT_TWR_UPLINK` `ack` = code in bits 0..5 (`0` not ACKed, `1` ACK, `2` ACK + PENDING,
`3` late, `4` no counter, `5` reserved, never sent), bit 7 = the ACK carried a CTRL tail,
bit 6 = the tail held a TimeAns.

### 3.4 Reliable delivery

The NB ACKs the Node locally, so the Node forgets the frame at once. Losing the event on
the link would lose the data, hence:

- **Event ring** (32 events): uplinks and downlink outcomes carry a delivery `seq`; the NB
  resends go-back-N every 300 ms until the central acks with `TWR_EVT_ACK`.
- The central acks **only after persisting** (outbox + `sessions.db`), processes strictly
  in `seq` order from `evt_head_seq`, and dedups by `seq` (window 128) and by
  `(addr, counter)`.
- **Backpressure:** with the ring full the NB drops a fresh frame completely: no ACK, no
  event, no `last_seen` update (`acks_suppressed`). The Node keeps the data and later
  sends it again or via history backfill.
- **`KEEP_NEWER`** on restore: the NB keeps the newer of its own and the central's
  `last_seen`, so a central restart never reopens a replay window.

### 3.5 Restore sequences

- **NB boot:** `EVT_BOOT` → `GET_INFO` → `TWR_TIME_SYNC` → `SET_RADIO_CONFIG` →
  `TWR_CTR_BLOCK` → `TWR_NODE_ADD` × all joined Nodes → `TWR_START` → `TWR_QUEUE_PUSH` of
  pending downlinks. Expected `seq` = 1.
- **Central restart** (NB keeps running): `GET_INFO` → `TWR_TIME_SYNC` → new counter
  block → `TWR_NODE_ADD` + `KEEP_NEWER` → re-push pending downlinks → continue from
  `evt_head_seq`.
- A missed NB reboot (`now_ms` dropped) is detected by the 60 s `GET_INFO` probe.

### 3.6 Downlink handling in the central

- **One command in flight per Node**, pushed `CONFIRMED`, TTL 1 h.
- `DELIVERED` / `SENT` → waiting for the port-85 answer; a Node reboot (`Hello`) or 3
  uplinks without the answer push it again.
- `NOT_DELIVERED` / `RADIO_ERR` → pushed again; **after 5 unacked attempts** dropped,
  with an `undelivered` command document northbound.
- `0x91` answers (fallback NB only): unconfirmed, TTL 10 min, at most one waiting per
  Node.

### 3.7 Time

- The central takes a clock anchor (`now_ms` ↔ host time) at every `GET_INFO` and sends
  `TWR_TIME_SYNC` after it (with the 60 s reconcile probe, in practice every 60 s) and at least every 10 min; earlier (rate-limited to 60 s) when
  an ACK went out without a TimeAns.
- The same anchor stamps `received_at` of every uplink with its **air time** (`t_ms`),
  also for uplinks replayed from the ring.

---

## 4. Hub central ↔ Portal (MQTT)

### 4.1 Uplinks northbound

- `0x81` payloads are decoded by the STICKER decoder by port (2 telemetry, 3 alarm, 85
  answers) into the **same documents as LoRaWAN** (reading, alarm, history, config,
  device info, command), with `transport = p2p` and `dev_addr` = the 32-bit `addr`.
- `0x91` control uplinks (Capabilities, Hello, LinkCheckReq, TimeReq) are consumed by
  the central and publish no reading. "Answered in the ACK" is in the central log and in
  the NB counters only.
- `received_at` = the air time of the frame, not the handling time.

### 4.2 Outbox and delivery guarantee

- A ring event's northbound messages are written to a **persistent outbox**
  (`/data/proximos/radio/p2p/outbox/<dev_eui>-uplink-<fcnt>.json`) before the event is
  acked to the NB.
- A drainer publishes at **QoS 1**, one in flight, deleting an entry only after `PUBACK`;
  it survives central restarts and broker outages.
- Bound 1000 messages; when full, ring events are not acked (backpressure through the NB
  to the Node).
- **At least once:** consumers must **dedup by DevEUI + `fcnt`**.

### 4.3 Commands from the Portal

- The Portal sends commands through the existing enqueue API; the central wraps them as
  `0x81 ‖ 86 ‖ protobuf` (body ≤ 76 B; longer responses use paging).
- The answer comes back as a port-85 uplink and is correlated by `seq`.
- After 5 failed attempts the central publishes `{kind: "undelivered", attempts: 5, …}`.
  The Portal ends the command with this result (H4, proximos-v2 `b357e231`, not merged yet).

### 4.4 Status and diagnostics

- Retained **`proximos/<hub>/radio/northbridge`**: NB state, versions, radio config,
  gateway use (peers, queue, ring, counter block, airtime) and all counters (NB's 25 +
  `ack_ctrl_sent`, `ack_time_no_anchor` + the central's: `evt_seq_gap`, `outbox_pending`,
  `outbox_overflow`, `rx_time_fallback`, `ack_ctrl_req`, `ack_time_missed`,
  `time_sync_sent`, …). The Portal's Hub radio card reads it.
- The Node's `RadioState` (via NFC / Info) shows link state, last ACK RSSI, downlink
  RSSI/SNR, margin and gw_count.

### 4.5 History backfill

- If uplinks are lost beyond the ring (≥ 32 frames), the Portal sees a gap and requests
  history; the Node replays it at **sample** resolution. Report resolution of the gap is
  lost; no rejoin is needed (criterion T3).
- The Portal triggers backfill only on a gap > 2 × `interval_report`, batched, as on LoRaWAN
  (decided 2026-10-07): a single lost report is not backfilled.

---

## 5. Known limits

| # | Limit |
|---|---|
| T8-F1 | A Portal reconnecting with `clean_session` to a broker without persistence loses what the outbox drained meanwhile. Not TOWER-specific; own issue. |
| T3-F2 | Ring 32 events, shared by all Nodes. Beyond it, history backfill at sample resolution. |
| — | An NB reset after the ACK and before the central's ack loses those frames (RAM). |
| — | A JoinAccept lost on air after the commit: the Node keeps its old pairing until the next JoinRequest. |
| — | TimeAns runs late by the `GET_INFO` transfer (~16 ms), uncompensated. |
| — | A single lost report is not backfilled (gap ≤ 2 × interval), as on LoRaWAN. |
| §3.3 | One channel 869.525 MHz for ~30 Nodes per NB (decided 2026-10-08). The Hub config still defaults to 868.1 MHz until the central MR changes it. |

---

## 6. Verification

- **KATs:** `tests/ccm/tower_frame_kat.json` (frames), `tower_join_kat.json` (join, keys),
  `p2p_tower_ack_ctrl_kat.txt` (ACK tail), shared byte-for-byte with the NB and the
  central.
- **Native:** `p2p_logic` against a gateway emulator (`bash tests/run_native.sh`).
- **HIL** (STICKER 5722 ↔ Hub NB + central, SF7, 868.1 MHz), plan §13.3–§13.5:
  - P0: KAT, ACK turnaround 20 ms, 0 retransmits at SF7–SF12, downlink after PENDING;
  - P2: join, confirmed uplinks, `0x91`, Portal commands, ring replay, NB/central
    restarts, backfill beyond the ring (T1–T6, T8 central side): **PASS**;
  - P3: command checks PASS; answers in the ACK on NB 0.3.2 + c69: **PASS** (144/144
    uplinks ACKed, 22 LinkCheckAns + 4 TimeAns all in the ACK tail, no `0x91` DL, RTC
    within ±0.5 s).
