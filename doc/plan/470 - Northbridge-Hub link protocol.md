# Northbridge ↔ Hub link protocol (TOWER gateway)

*Type: design · Status: draft · Date: 2026-09-28 · Parent: #470 plan §9.4, §13.2 H2, D5*

The TOWER protocol (#470) defines only the **air** side. The link between the Northbridge
(STM32WL55 on the Hub, the TOWER gateway) and the Hub central (`control-radio` on the CM4)
is **our own protocol**, internal to the Hub. It does not follow the tower-protocol console
and has no upstream dependency (D5, Hynek 2026-09-28).

This document is the whole link in one place:
- the transport and framing, taken over from today's link (proximos-v2
  `plan/system/lora/design.md` §3);
- the commands of today's link that stay;
- the gateway messages frozen in H2 (proximos-v2 `plan/control/radio/p2p_tower_gateway.md`
  §2, Hub controller);
- the state machine and the recovery rules;
- two additions that §4 of this document proposes (event delivery and `last_seen` merge).

Once agreed, the normative copy lives in proximos-v2 next to the Northbridge design, and
this file points there.

---

## 1. Roles

| | Northbridge (gateway) | Hub central |
|---|---|---|
| Holds | radio config, peer table `addr → {session_key, last_seen, HOME}` and DL queue, **RAM only**; the current TX counter block | AppKeys, registry, `net_id`, session DB, persisted `last_seen` and counter blocks, DL queues with delivery state |
| Does on air | CCM open/verify, replay rule, **ACK within the turnaround**, `PENDING`, DL TX, JoinAccept at a given time | nothing directly |
| Does on the link | reports fresh uplinks, raw joins, TX outcomes, its own state | configures, installs sessions, pushes downlinks, restores the NB after a boot |
| Never | holds an AppKey or `join_key`, reads keys back, persists anything | answers in the 20 ms window (too slow via UART + Linux) |

The split follows from T3/D15: the time-critical part (ACK, `PENDING`, DL timing) is on
the MCU, and everything durable is in the central.

## 2. Transport and framing (taken over from today's link)

- UART `ttyAMA3`, 115200 8N1. Bench: the same byte stream over J-Link RTT
  (`BENCH_RTT_BRIDGE`).
- HDLC-style frame: `0x7E | escaped payload | 0x7E`. The escape byte is `0x7D`, followed by
  the byte XOR `0x20`.
- Payload: `type(1) ‖ seq(1) ‖ body(0..280) ‖ crc16(2)`.
- CRC16-CCITT-FALSE (poly `0x1021`, init `0xFFFF`, not reflected) over
  `type ‖ seq ‖ body`. Reference: `0x29B1` for `"123456789"`.
- All link integers are **little-endian**. The TOWER frame is little-endian too (#470 §4).
- `type`: commands `0x01–0x3F`, responses `0x40 | cmd`, events `0x80–0xBF`.
- `seq`:
  - For commands: rolling, set by the host and echoed in the `RSP`.
  - For events: rolling, set by the NB; §4.1 turns it into a delivery sequence.
- `RSP` body = `status(1)` ‖ command-specific data. Status codes:

  | Code | Status |
  |---|---|
  | 0 | `OK` |
  | 1 | `BAD_PARAM` |
  | 2 | `BUSY` |
  | 3 | `INVALID_STATE` |
  | 4 | `LATE` |
  | 5 | `INTERNAL` |
  | 6 | `NO_SPACE` |
  | 7 | `NOT_FOUND` |
  | 8 | `UNSUPPORTED` (unknown command) |

- `BAD_PARAM` is strict: undefined flag bits set, a body of the wrong length, a value out of
  range (e.g. `tx_power_dbm` outside the board PA range, no clamping), `TWR_QUEUE_PUSH` with
  `item`, `ttl_s` or `len` = 0, `TX_CANCEL` after the transmission has started.

- Command timeout on the host: 800 ms (covers the bench RTT bridge).
- Retransmission of commands: only for idempotent commands (§5, *Idempotent* column).
- Bad CRC: drop the frame, resync on the next `0x7E`, count `crc_err`.

**Throughput check.** At 115200 bd one byte takes ~87 µs.
- The largest message is `EVT_TWR_UPLINK` with 78 B plaintext, ~100 B framed. That is
  ~9 ms on the wire.
- A LoRa SF7 frame is on air for ≥ 40 ms, so the link is never the bottleneck. This holds
  even for a full peer-table restore (256 × `TWR_NODE_ADD` ≈ 256 × 34 B ≈ 0.8 s).
- The ACK is sent from the MCU and does not wait for the UART. Events are sent only after
  the ACK is scheduled.

## 3. Flag day — no versioning (Hynek 2026-09-28)

The NB firmware and the Hub software are flashed together. There is no version
negotiation, no capability bits and no backward compatibility:
- no legacy (keyless P2P modem) path in the central;
- no rules for older or newer images;
- no rollback to the old P2P: it is abandoned (Hynek 2026-09-28); the old
  `sessions.db` and the legacy code are removed, not preserved.

`fw_ver` in `EVT_BOOT` / `GET_INFO` is for diagnostics only. The central logs a mismatch
against the image it was built with, but never switches behaviour by it.

### 3.1 `GET_INFO` — the NB status

`GET_INFO` returns everything about the NB in one response (it also replaces a separate
stats command):

| Group | Fields (LE, in byte order) |
|---|---|
| Identity | `fw_ver u8[3]`, `git_sha u8[4]` (first 4 commit bytes, display order), `reset_cause u8`, `build_flags u8` (b0 `BENCH_RTT_BRIDGE`, b1 debug) |
| Clock | `now_ms u64` (NB `k_uptime_get()`: the `TimeAns` anchor, and a drop means a reboot, §6.3) |
| State | `state u8` (0 `BOOT`, 1 `CONFIGURED`, 2 `GATEWAY`), `net_id u32`, `turnaround_ms u8`, `dl_gap_ms u8` |
| Radio | `modulation u8` (0 lora) ‖ the `SET_RADIO_CONFIG` body (`freq_hz u32, sf u8, bw u8, cr u8, preamble u16, tx_power_dbm i8, flags u8`) |
| Errors | `err_flags u32`, `last_err_code u8`, `last_err_age_s u32` |
| Counters | 25 × `u32` since boot: `rx_total, rx_fresh, rx_dup, rx_replay, rx_mic_fail, rx_unknown_src, rx_join_fwd, rx_not_for_us, rx_bad_hdr, radio_crc_err, radio_hdr_err, rx_overrun, rx_rearm_fail, tx_total, tx_radio_err, acks_sent, acks_late, acks_suppressed, dl_sent, dl_delivered, dl_not_delivered, dl_expired, link_crc_err, link_err, evt_resent` |
| Timing | `ack_turnaround_last_us u16`, `ack_turnaround_max_us u16` |
| Duty | `airtime_ms_last_hour u32` |
| Tables | `peers u16, peers_max u16, queue_used u16, queue_max u16, evt_ring_used u8, evt_ring_max u8, ctr_next u32, ctr_last u32` |

171 B after the status byte. `EVT_BOOT` = `fw_ver[3] ‖ git_sha[4] ‖ reset_cause` (8 B).
Byte layout frozen by the Hub controller 2026-09-28; the Hub's NB status document
(`northbridge_diagnostics.md`) shows these fields.

The layout is fixed for the image (flag day). The field order above is the byte order on
the link.

## 4. Additions to the frozen H2 set

Both of these close gaps that come from the NB **ACKing locally**.

### 4.1 Reliable delivery of uplink events (`EVT_ACK`)

**Gap.** The NB ACKs the STICKER within 20 ms, and the STICKER then drops the frame from
its queue. If the following `EVT_TWR_UPLINK` is lost on the link (CRC error, the host is
busy or restarting), the data is lost for good. Today's keyless modem does not have this
problem, because the ACK comes from the central.

**Rule.**
- **Which events:** `EVT_TWR_UPLINK` and `EVT_TWR_TX` go into an NB event ring of
  `evt_ring` entries (≥ 32, ~3.3 KB). `EVT_RX` (joins) stays out: the NB never ACKs a join,
  so the air retry already covers a lost one.
- **Numbering:** these events use the frame `seq` as a delivery sequence number.
- **Host ack:** the host confirms cumulatively with `0x18 TWR_EVT_ACK {seq u8}`, meaning
  "everything up to and including `seq` was processed". It sends the ack after persisting,
  at the latest every 100 ms or every 8 events.
- **Resend:** go-back-N from the oldest unacked event, at most one pass per 300 ms.
- **Host dedup (MUST):** by delivery `seq` (window 128 behind the last acked) for every ring
  event — `EVT_TWR_TX` has no counter — on top of the `(addr, counter)` dedup.
- **Backpressure to the air (MUST):** when the ring is full, the NB **drops the fresh frame
  completely** — no ACK **and no `last_seen` update** (`acks_suppressed` counter). Updating
  `last_seen` would make the node's net-layer reps hit the `==` rule and get a re-ACK for data
  the host never got.
  - The STICKER then sees no ACK, keeps the data (retry ladder, history) and sends it
    again later.
  - An outage of the host service therefore costs no data, only a delay, the same as an
    NB outage (#470 §6.6).
- Residual window (accepted, same class as D15): an NB reset after the ACK but before the
  host ack loses those frames (RAM).
- `EVT_BOOT`, `EVT_RX`, `EVT_TWR_CTR_LOW`, `EVT_TX_DONE` and `EVT_LOG` stay best-effort; they carry
  the current delivery `seq` without advancing it, so the cumulative ack stays unambiguous.

### 4.2 `last_seen` merge on restore (`KEEP_NEWER`)

**Gap.**
- When the host service restarts while the NB keeps running, the host re-sends
  `TWR_NODE_ADD` with its persisted `last_seen`.
- If that value is older than the NB's (the last events had not been processed yet), the
  NB would move the replay lane **back**.
- That opens a replay window for frames the NB has already accepted.

**Rule.** `TWR_NODE_ADD` flag bit 1 `KEEP_NEWER`: when the peer exists with the same
`session_key`, the NB keeps `max(own, sent)`. A different key (a new join) always takes
the value that was sent. The host sets `KEEP_NEWER` on every restore. On a new join it
sends `last_seen = 0` without the flag.

## 5. Messages

### 5.1 Commands (host → NB)

| ID | Name | Body | Idempotent | Note |
|---|---|---|---|---|
| `0x01` | `GET_INFO` | — | yes | RSP: the NB status, §3.1 |
| `0x02` | `SET_RADIO_CONFIG` | `freq_hz u32, sf u8, bw u8, cr u8, preamble u16, tx_power_dbm i8, flags u8` (b0 public sync word, b1 IQ inverted) | yes | `lora` only; not allowed in `GATEWAY` (`INVALID_STATE`), stop first |
| `0x04` | `RX_STOP` | — | yes | leaves `GATEWAY`, keeps the table, queue and counter |
| `0x05` | `TX_SCHEDULE` | `tx_id u8, t_ms u64, len u8, frame[len]` | no | **JoinAccept only** (sealed by the central); one slot; collision with an ACK/DL → `BUSY` or `EVT_TX_DONE LATE` |
| `0x06` | `TX_CANCEL` | `tx_id u8` | yes | |
| `0x10` | `TWR_START` | `net_id u32, turnaround_ms u8 (20..60), dl_gap_ms u8 (20..60), flags u8` (b0 `ACK_ENABLED`) | yes | needs a config and a counter block, otherwise `INVALID_STATE` |
| `0x11` | `TWR_NODE_ADD` | `addr u32, session_key[16], last_seen u32, flags u8` (b0 `HOME`, b1 `KEEP_NEWER` §4.2) | yes | upsert; a new key drops the node's queue; `BAD_PARAM` for addr `0` / `0xFFFFFFFF` / `net_id`; `NO_SPACE` |
| `0x12` | `TWR_NODE_REMOVE` | `addr u32` | yes (`NOT_FOUND` = done) | drops the peer and its queue |
| `0x13` | `TWR_CTR_BLOCK` | `first u32, last u32` | yes | replaces the block; the NB never goes below the old `next` (a block with `last < next` → `BAD_PARAM`) |
| `0x14` | `TWR_QUEUE_PUSH` | `addr u32, item u16 ≠ 0, ttl_s u16, flags u8` (b0 `CONFIRMED`), `len u8, plaintext ≤ 78` | **yes, by `(addr, item)`**: the same item already queued replaces it (`OK`, no duplicate); `BUSY` while it is on air | plaintext = the whole `0x81…` / `0x91…` envelope; sealed at TX time. P2: one TX attempt per item, also for `CONFIRMED` (retries are the central's) |
| `0x15` | `TWR_QUEUE_DROP` | `addr u32, item u16` (0 = all) | yes | no `EVT_TWR_TX` for dropped items |
| `0x17` | `TWR_NODE_LIST` | `start u16` | yes | paged `{addr, last_seen, flags}`, max 24 per RSP, **never keys** |
| `0x18` | `TWR_EVT_ACK` | `seq u8` | yes | cumulative; no RSP (saves the link) |

### 5.2 Events (NB → host)

| ID | Name | Body | Delivery | Note |
|---|---|---|---|---|
| `0x80` | `EVT_BOOT` | `fw_ver[3], git_sha[4], reset_cause` (8 B) | best-effort | the table, queue and block are empty; the host runs §6.2 |
| `0x81` | `EVT_RX` | `t_ms u64, rssi i16, snr i8, reserved u8, len u8, frame[len]` | best-effort | `dest == 0` (JoinReq) or an unknown `src`; no ACK |
| `0x82` | `EVT_TX_DONE` | `tx_id, status, t_actual_ms` | best-effort | the `TX_SCHEDULE` outcome only |
| `0x83` | `EVT_LOG` | ASCII | best-effort | all builds, rate-limited |
| `0x84` | `EVT_TWR_UPLINK` | `t_ms u64, rssi i16, snr i8, frame_flags u8, addr u32, counter u32, ack u8, len u8, plaintext` | ring | fresh frames only; `ack`: 0 none, 1 sent, 2 sent + `PENDING`, 3 late, 4 no counter, **5 suppressed (ring full, §4.1)** |
| `0x85` | `EVT_TWR_TX` | `addr, item, outcome, gw_counter, node_ack_counter, ack_rssi` | ring | outcome `DELIVERED / SENT / NOT_DELIVERED / EXPIRED / RADIO_ERR / NO_COUNTER` |
| `0x86` | `EVT_TWR_CTR_LOW` | `next u32, last u32` | best-effort, once per block; the host also checks `ctr_next`/`ctr_last` in every `GET_INFO` | below 25 % left; fail-closed when the block is used up |

`snr` is in quarter dB (SX126x native, `snr_q`).

## 6. State machine and sequences

### 6.1 NB states

```
BOOT ──SET_RADIO_CONFIG──▶ CONFIGURED ──TWR_CTR_BLOCK + TWR_START──▶ GATEWAY
  ▲                              ▲                                        │
  └─────────── reset ────────────┴──────────────── RX_STOP ───────────────┘
```

- `TWR_NODE_ADD`, `TWR_QUEUE_PUSH` and `TWR_CTR_BLOCK` are accepted in `CONFIGURED` and
  `GATEWAY` (the restore runs before `TWR_START`, so the first frame is already ACKed).
- In `GATEWAY`: `SET_RADIO_CONFIG` → `INVALID_STATE`. A radio change is
  `RX_STOP → SET_RADIO_CONFIG → TWR_START`.

### 6.2 NB boot (restore)

`EVT_BOOT` → `GET_INFO` (status, clock anchor) → `SET_RADIO_CONFIG` → `TWR_CTR_BLOCK`
(a new block, persisted **before** it is sent) → `TWR_NODE_ADD` for every joined node
(exact `last_seen`, `HOME`, `KEEP_NEWER`) → `TWR_START` → `TWR_QUEUE_PUSH` of every DL
without an outcome.

### 6.3 Host service restart (NB keeps running)

`GET_INFO`:
- `uptime_ms` shows no reboot;
- the NB is in `GATEWAY`, so no reconfiguration is needed.

Then:
1. `TWR_CTR_BLOCK` with a new block (the host does not know how far the NB got).
2. `TWR_NODE_ADD` + `KEEP_NEWER` for all nodes (§4.2).
3. `TWR_QUEUE_PUSH` of the DLs without an outcome (idempotent by `item`).
4. The NB resends the unconfirmed events from the ring (§4.1).

### 6.4 Join

`EVT_RX (dest 0)` → the central verifies it and derives the session → `TX_SCHEDULE`
(JoinAccept at `t_rx + rx_delay`) → on `RSP OK` persist the session →
`TWR_NODE_ADD{last_seen = 0}` (a new key, without `KEEP_NEWER`). This is decision D2 of the
central (proximos-v2 `p2p_tower_gateway.md` §3.2).

- The NB also transmits on its own (ACK, DL), so `TX_SCHEDULE` can come back `BUSY` or
  `LATE`. A refused JoinAccept then leaves the node on the pairing it had. Persisting
  first would swap the NB's key and cut off a node that never got the accept.
- The crash window between `RSP OK` and the persist is a local file write of a few ms.
  The worst case is one supervision rejoin.
- No race with the first uplink: `TWR_NODE_ADD` follows `RSP OK` within ms, and the
  accept goes on air at `t_rx + 1 s`.

### 6.5 Uplink and downlink

- **Uplink:** air → NB (ACK after 20 ms) → `EVT_TWR_UPLINK` → the central persists
  `last_seen`, decodes, sends `TWR_EVT_ACK`.
- **Downlink:** `TWR_QUEUE_PUSH` → on the next confirmed uplink of the node the NB sets
  `PENDING` in the ACK → the DL goes out `dl_gap_ms` after the ACK TxDone →
  `EVT_TWR_TX` → the central correlates `seq` / answers.

## 7. Security

- Session keys go over the UART **in plaintext**. The UART is a trace on the Hub board,
  inside the enclosure. Accepted, same class as the key in NB RAM (T3). The threat is
  physical access to the Hub, which leads to rejoin / new AppKeys (#470 §6.6).
- No command reads a key back (`TWR_NODE_LIST` and `GET_INFO` have none). The
  `EVT_LOG` must never print keys or plaintext.
- `/dev/ttyAMA3` only for `control-radio` (`DeviceAllow=`, opened after landlock, as
  today).
- The NB never persists keys, counters or the queue (D15). RDP on in production.

## 8. Toward multi-gateway (P7, not now)

The same messages also cover the target topology of #470 §6.3.1 (central Hub + client
Hubs):

- On a client Hub, a thin forwarder carries the same payloads (`type ‖ body`) over the
  LAN, in TCP with length framing instead of HDLC. The central talks to local and remote
  NBs the same way.
- `HOME` (D17 A): only the home gateway ACKs; the others report `EVT_TWR_UPLINK` with
  `ack = 0` for dedup and RSSI statistics. Moving the home = `TWR_NODE_ADD` with a
  different `HOME` + `KEEP_NEWER`.
- Counter blocks per NB are disjoint, allocated by the central. No change on the NB side.
- An additional `gw_id` in the events is not needed: the central knows which link an
  event came from.

## 9. Tests

- **Golden vectors** (a JSON file shared by the NB native_sim and the central's unit
  tests):
  - every command, response and event byte for byte, including HDLC escapes and the CRC
    vector;
  - together with `tower_frame_kat.json` / `tower_join_kat.json` (air side).
- **Deframer fuzzing:** garbage, cut frames, `0x7E` inside the body, bad CRC — it must
  resync and never get stuck.
- **Central with a scripted NB:**
  - the §6.2 / §6.3 sequences;
  - `KEEP_NEWER`;
  - resend from the ring and dedup;
  - idempotent `QUEUE_PUSH`;
  - `CTR_LOW` → a new block;
  - `NO_COUNTER` → fail-closed.
- **HIL (H5):**
  - `systemctl restart` of the central during a stream of confirmed uplinks → 0 frames
    lost, 0 duplicates northbound;
  - an NB reset → restore without a rejoin;
  - the host service down for > ring capacity → the STICKER keeps its data (no ACK),
    catches up after the restart.

## 10. Impact on the existing H1/H3 work

Everything in HC's frozen set holds. §4 adds:
- NB: an event ring + resend, the `acks_suppressed` backpressure, the `KEEP_NEWER` merge,
  `UNSUPPORTED`, `GET_INFO` per §3.1 (replaces `TWR_GET_STATS`), no `RX_START`;
- central: no legacy proto-1 path and no `proto_version` switch (§3), the `TWR_EVT_ACK`
  send after persisting, `KEEP_NEWER` on restore, dedup by
  delivery `seq`.

Reviewed by the Hub controller 2026-09-28: §4.1 and §4.2 are agreed with the fixes above,
the join order stays D2 (§6.4), at ~2 h for the NB and the central in parallel, after the first E2E. The normative copy is
folded into proximos-v2 `plan/control/radio/p2p_tower_gateway.md`. Golden vectors
`nb_link_golden.json` are generated from the central's Rust codec and consumed verbatim by
the NB native_sim.

Timing: in P2 right after the first E2E (H5), before P3; the estimate is the Hub
controller's.
