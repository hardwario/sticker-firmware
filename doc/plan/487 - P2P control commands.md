# P2P control commands: DevStatus, Backoff, HomeGateway, PendingInfo

> PR #487 → `v1.5.0`, on top of #486 (`doc/plan/486 - One TOWER protocol on both PHY
> profiles.md`, item W7). The command table is in `doc/plan/470 - TOWER protocol as the
> P2P transport.md` §8.2 ("the TOWER plan"). Epic #118.

## Decisions (Hynek, 2026-10-12)

| # | Decision |
|---|---|
| **C-1** | Implement `DevStatus` 0x30, `Backoff` 0x15, `HomeGateway` 0x06 and `PendingInfo` 0x22 on the node. |
| **C-2** | The central sends `Detach` 0x03 and `RejoinReq` 0x04 (the node already handles them). |
| **C-3** | Drop `RekeyReq` 0x05 (`RejoinReq` rekeys), `RadioParamReq` 0x11 (no ADR: SF and power are network-wide) and `Poll` 0x21 (`PendingInfo` covers it). The IDs stay unused. |
| **C-4** | Reserve, not implement: `RxParamSetup` 0x12, `ChannelPlan` 0x13, `LinkPolicy` 0x14, `PowerMode` 0x23, `Ping` 0x31, `RadioStats` 0x32. |
| **C-5** | The Capabilities cmd bitmap stays 64 bits (core IDs 0x00–0x3F only). |

All of it is additive. A node without #487 skips the new IDs by their length, a central
without it never sends them. There is no flag day. The central should send a command only
to a node whose Capabilities bitmap names it.

## Wire

All values little-endian, in the `0x91` TLV codec (`id(1) ‖ len(1) ‖ value`).

| ID | Name | Dir | Value | Where |
|---|---|---|---|---|
| `0x06` | HomeGateway | ↓ | `gw_addr(4) ‖ flags(1)` (flags reserved, 0) | `0x91` downlink |
| `0x15` | Backoff | ↓ | `seconds(2)`; 0 ends a running one | `0x91` downlink **or any ACK tail** |
| `0x22` | PendingInfo | ↓ | `more(1)`: downlinks queued after the one PENDING announces | `0x91` downlink **or any ACK tail** |
| `0x30` | DevStatusReq | ↓ | empty | `0x91` downlink |
| `0x30` | DevStatusAns | ↑ | `battery_mv(2) ‖ dl_rssi(i8) ‖ dl_snr(i8) ‖ tx_power(i8) ‖ uptime_s(4)`, 9 B | `0x91` uplink, confirmed |

The Capabilities cmd bitmap is now `de 01 21 00 05 00 01 00`: IDs 0x01–0x04, 0x06–0x08,
0x10, 0x15, 0x20, 0x22, 0x30.

**ACK tail (TOWER plan §13.5, extended).** Besides the `LinkCheckAns` / `TimeAns` that answer
a request, the gateway may append `Backoff` and/or `PendingInfo` to **any** ACK (CTRL flag
bit 1), after the answers. The node's ACK window always has room for these 7 B (SF7: still
the 200 ms floor; SF10 +82 ms, SF12 +164 ms). Any other ID in a tail is skipped: a
command (`Detach`, `DevStatusReq`, `HomeGateway`) must come as a downlink.

## Node behaviour

**DevStatus 0x30.** A `DevStatusReq` downlink queues a `DevStatusAns` in the next `0x91`
uplink (one confirmed frame, up to 3 tries 30 s apart, like Hello).

- `battery_mv`: the last battery sample, 0 = none yet.
- `dl_rssi` / `dl_snr`: the node's RSSI/SNR of the downlink that carried the request.
- `tx_power`: the session TX power (the JoinAccept's, or `p2p-tx-power`).
- `uptime_s`: seconds since boot.

There is no MCU temperature: the board has no die sensor wired. A battery percentage is
the central's to compute (LoRaWAN maps 2.4–3.6 V linearly).

**Backoff 0x15.** `app_radio_backoff_set()` in the common layer.

- Every uplink but alarms waits until the backoff ends: telemetry, answers, history and
  `0x91` control. Alarms go at once.
- It is held exactly like a duty-cycle wait. The send returns `-EAGAIN` with the wait, and
  M-2 counts it as a hold, so a held node is not rejoined. A held telemetry frame uses one
  of its 8 retries, as for a duty hold.
- Answers stay in their queue while it runs, so an alarm is never stuck behind one. A
  queued alarm restarts the TX run at once.
- The node cuts it to `APP_RADIO_BACKOFF_MAX_S` = 3600 s, inside the 75 min M-2 cap. 0 ends
  it, but a frame already waiting keeps its wait. RAM only: a reboot or a new session
  (join, Detach) ends it.
- `ats radio status`: `backoff N s`.

**HomeGateway 0x06.** The node records `gw_addr` and `flags` for the session and shows them
in `ats radio status`. It does not re-address its uplinks: `dest` stays the `net_id`.
Multi-gateway (TOWER plan P7, §9 D17) decides what the node does with it. A new session
forgets it.

**PendingInfo 0x22.** With `more > 0`, the node sends an empty confirmed `0x91` frame (the
envelope byte alone, 23 B on air) at the next TX gap. Its ACK carries PENDING and the next
downlink. This replaces Poll.

- The chain is bounded: at most `P2P_DRAIN_MAX` = 8 drain frames in a row. An `app_radio`
  frame (telemetry, answer, alarm) that leaves starts the count again.
- `more = 0` does nothing.
- The drain frame respects the duty cycle, the TX gap and Backoff.

## Hub side (central + Northbridge)

| # | Item | Who |
|---|---|---|
| H-1 | `Detach` on node removal before `NodeRemove`; `RejoinReq` (kind 0 rediscover, 1 rekey) as a Portal / CLI action | central |
| H-2 | `DevStatusReq` as a `0x91` downlink (Portal / CLI action, optional periodic poll). `DevStatusAns` northbound as a device-status document: battery mV (and % computed), DL RSSI/SNR, TX power, uptime | central |
| H-3 | Accept an empty `0x91` uplink (no TLV) and ACK it normally. With a queued downlink, set PENDING. | central + NB |
| H-4 | `PendingInfo` in the ACK tail: when the ACK sets PENDING and more downlinks wait for the node, append `0x22 01 <more>` (capped at 255). Without a CTRL-tail NB, the central may put it in the `0x91` downlink instead. | NB (tail) / central |
| H-5 | `Backoff`: as a `0x91` downlink, or in the ACK tail when the NB itself is congested or out of duty budget | central / NB |
| H-6 | `HomeGateway` when the central assigns or moves a node's home gateway (P7) | central |
| H-7 | Gate by the node's Capabilities bitmap; a node without the bit skips the ID anyway | central |
| H-8 | Decoders: the JS decoder `app/decoder/p2p.js` reads all four | central / Portal |

## Tests

- `tests/p2p_logic`:
  - DevStatusAns layout from a PENDING downlink, no battery → 0 mV, a DevStatusReq in an
    ACK tail skipped;
  - Backoff from a plain ACK tail and from a downlink, 0 ends it, a `0x91` uplink waits it
    out;
  - HomeGateway recorded, too short ignored, a new session forgets it;
  - PendingInfo drain: the envelope byte only, confirmed, 8 in a row, then an `app_radio`
    frame restarts the count;
  - the Capabilities bitmap, the full control set (41 B), the ACK windows with room for
    the 7 B.
- `tests/radio_common`:
  - Backoff holds an answer but not an alarm, with no retry counted;
  - a report waits it out;
  - the 3600 s cap and 0 ends it;
  - M-2 stays quiet through a 3600 s backoff and rejoins after it.
- `app/decoder/p2p.test.js`: the four commands, the 41 B control set, the bitmap.
- HIL (with the Hub side): DevStatus round trip in the Portal; Backoff 120 s via the
  central (telemetry held, an alarm passes, no rejoin); PendingInfo with 3 queued
  commands (all delivered within one report interval).

## Size

Release: FLASH 183 836 B (+776 B over #486), RAM 48 628 B (unchanged).
