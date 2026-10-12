# One TOWER protocol on both PHY profiles (fsk / lora)

> PR #486 → `v1.5.0`. Follows #470/#484 (`doc/plan/470 - TOWER protocol as the P2P
> transport.md`, "the TOWER plan"). Epic #118.

## Principle (Hynek, 2026-10-12)

`fsk` (TOWER-native GFSK) and `lora` are **only the physical layer**. Everything above
it is the same on both: the frame, the crypto, the counters, the confirmed send, ACK and
PENDING, the envelopes, the commands and the data they carry. A frame the node builds
must never depend on the PHY it goes out on. TOWER compatibility stays a hard requirement.

Only the PHY and the timing derived from the time-on-air may differ per profile (TOWER
plan §5). Timing is not on the wire.

## Decisions

| # | Decision |
|---|---|
| **U-1** | **One MTU: the TOWER 96 B frame, 74 B payload, 72 B body** after the STICKER envelope, on both profiles (2026-10-12). It replaces D14 (`lora` ≤ 100 B). A larger `lora`-only MTU was rejected for three reasons. The data would depend on the PHY. The gain is one header plus one ACK on long telemetry only. 96 B is already the SF12 ceiling of the radio's fixed 4 s TX timeout (3941 ms). The Capabilities MTU field could negotiate a larger frame later without a wire change. |

## Work

| # | Item | Where | State |
|---|---|---|---|
| W1 | `P2P_FRAME_MAX` 100 → 96, Capabilities MTU 96, tests, decoder | node | **done** `eebf82c4` |
| W2 | Docs: TOWER plan D14, `doc/p2p.md`, `doc/radio_p2p.md`, `doc/version 1.5.md`, link doc | node | **done** (doc commit) |
| W3 | NB `P2P_TWR_DL_MAX_PT` 78 → 74; RX refuses frames over 96 B (it parses up to 255 B today) | proximos-v2 NB | **done**, NB 0.4.1+d69b5d68 on Hubs 182 and 158 (2026-10-12) |
| W4 | Central `MAX_FRAME` / `MAX_PAYLOAD` / `MAX_COMMAND_BODY` 100/78/76 → 96/74/72, Capabilities MTU 96; check that no command body needs more than 72 B (e.g. a large `SetParam`) | proximos-v2 central | **done**, branch `hynek/p2p-mtu96` 136cd2fa (no MR yet); the JoinAccept `tx_power` is capped at 14 too |
| W5 | TOWER interop target: (a) stock TOWER nodes on the Hub and/or (b) a STICKER on a stock TOWER Radio Dongle. (b) needs a static-key mode or upstream E4, plus E3 for the `0x81`/`0x91` envelopes. **Decided 2026-10-12 (Hynek):** (a) dropped, no TOWER nodes will ever run on the Hub; (b) moved to #490 | decision | **decided**; (b) → #490 |
| W6 | `fsk` profile on the node (TOWER plan P5): SX126x GFSK 19.2 kbps, dev 20 kHz, sync `0xDB624715`, CRC-16 `0x1021`, PN9, TOWER timing (ACK window 200 ms, DL 20 ms after the ACK) | node + fork driver | moved to #490 |
| W7 | Control commands: DevStatus `0x30`, Backoff `0x15`, HomeGateway `0x06`, PendingInfo `0x22` implemented; Poll `0x21`, RadioParamReq `0x11`, Rekey `0x05` dropped; LinkPolicy `0x14` and others reserved (Hynek 2026-10-12) | node + central | **node done** in PR #487 (`doc/plan/487 - P2P control commands.md`); central open |
| W8 | `p2p_tx_power` max 22 → 14 dBm (the RFO_LP limit). The board wires only RFO_LP and the driver already capped at 14, so the range is unchanged; the config now says so. A stored 15–22 loads as 14; a JoinAccept assigning 15–22 is ignored (the node keeps its config) | node | **done** |
| W9 | Reserve ACK flag bit 1 (`CTRL` tail, TOWER plan §13.5) upstream. The ACK flags live in tower-firmware `tower-net-core` (`ack.rs`), not in tower-protocol: `ack_flags::CTRL = 1 << 1`, documented as reserved for STICKER, plus a test that a stock parser ignores the tail. No behaviour change | tower-firmware | **done** on hardwario/tower-firmware branch `hynek/ack-flag-ctrl-reserved` (865f509), hardwario/tower-firmware#1 |

**Deployment order (W1 + W3/W4):** the Hub and the nodes together, with every NB of the
Hub cluster. Both directions break across the change:

- A node with W1 drops a downlink over 96 B, so a node must not home at an NB without W3
  (a 100 B downlink from an old NB is lost).
- An NB with W3 drops a received frame over 96 B. A node built before #486 still sends
  up to 100 B (a long telemetry page, e.g. with 1-Wire sensors), and those uplinks are
  lost until the node is updated.

Uplinks from a W1 node to an old NB and central work: they accept the smaller frames.
No P2P node is in the field yet, so the bench was moved in one step (2026-10-12).

## HIL (2026-10-12)

The Hub runs W3/W4: NB 0.4.1+d69b5d68 on Hubs 182 and 158, central `hynek/p2p-mtu96`
136cd2fa. STICKER 5722 runs `c4ec6587`, the debug P2P bench image, and homes at Hub 158.

| Step | Result |
|---|---|
| Fresh join | PASS: both sides report Capabilities MTU 96; the node reports max payload 72 B and TX power 14 dBm |
| `SetParam` with a 72 B body (96 B frame), a no-op that rewrites every current value | PASS: received and applied, then answered; the node saves and reboots 8 s later (post-command action, by design), resumes the session and keeps its config (0 diffs) |
| 73 B body | PASS: the central refuses it at enqueue, nothing goes on air |
| GetConfig, GetSettings | PASS: one page each; the PendingInfo drain works |
| Regression | PASS: DevStatus answers (2897 mV, 14 dBm); every uplink from the 6 bench nodes arrives |

Not shown on HW:

- A 96 B uplink. The largest bench uplink is Settings-info at 62 B, since the bench has no
  W1 sensors. `tests/p2p_logic` covers it.
- The NB dropping a received frame over 96 B. Nothing sends one, and the `too_long`
  counter is only in the NB shell.

Hub follow-up: the central's GetParam does not cap the keys per group at the node's 16.
A 60-key GetParam made the node fail `pb_decode` with an array overflow; the node answered
with an error.

## Source

Gap analysis 2026-10-12 against `v1.5.0` 73c0b802, proximos-v2 `main` 2d1e90ed
(NB 0.4.0), and the TOWER upstream (`hardwario/tower` 24259e35, `tower-firmware` b7f3f4a,
`tower-protocol` 2351ea0). The upstream has not changed since 2026-07-13, so the KAT
reference is current.
