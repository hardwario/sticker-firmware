# P2P adaptive TX power (ADR parity with LoRaWAN)

Status: **design only**, nothing implemented (issue #442, PR #443). Base: feat-p2p. Part of the LoRaWAN ↔ P2P parity goal (`doc/plan/439 - Radio transport layer.md`).

## 1. Goal

On LoRaWAN, ADR makes a STICKER with a strong link transmit at lower power, and the recovery ladder (#424) restores full power when the link degrades. A P2P STICKER has nothing comparable today: it always transmits at `p2p-tx-power` (14 dBm by default), or at a power the central assigned once in the JoinAccept.

The owner decided on 2026-09-26:
- The **Hub always runs at full power**; its consumption is not a concern.
- The **STICKER in P2P should lower its TX power when the signal is strong** and behave like it does on LoRaWAN.

The data rate is out of scope. In P2P the SF is network-wide and owned by the Hub (the Northbridge has one receiver on one SF), so there is no per-node data rate to adapt. The node already rediscovers a changed network SF on its own through the join sweep (§5.3 of `doc/p2p.md`, proven on hardware 2026-09-26: SF10 → SF7 in 25 s after 8 failed cycles). A Hub-side network-SF policy, for example raising the SF when the weakest node's margin drops, is a Hub feature and not part of this plan.

## 2. Who decides: node-autonomous or central-controlled

| | Node-autonomous (from the Ack echo) | Central-controlled (LinkADRReq-like downlink) |
|---|---|---|
| Input | The central's own measurement of each uplink (RSSI/SNR in every Ack body, B1), i.e. exactly what an LNS bases ADR on | Same measurement, on the central |
| Protocol change | None | A new downlink (link-control frame or a `set_param` the M-3 gate currently blocks) plus central logic and per-node state |
| Reaction time | Immediate, at every Ack | One downlink round-trip (next uplink RX1) |
| Works with any central | Yes | Only with an updated central |
| Network-wide coordination (capture, interference, fairness) | No | Possible |

**Recommendation: node-autonomous.** Its input is the same number the central would use, it needs no protocol or Hub change, and at the target scale (20–30 nodes per Hub) network-wide coordination buys nothing. The central keeps control through the existing JoinAccept TX-power assignment, which takes precedence (§3.4).

## 3. Behaviour

### 3.1 Inputs

For every acknowledged uplink the node records the Ack's `snr`, which is the central's measurement of that uplink. Uplinks without an Ack are not recorded; they count towards the WARNING ladder instead (§3.3).

### 3.2 Step rule (same shape as LoRaWAN network ADR)

After `P2P_ADR_HISTORY` (20) recorded uplinks at the current power:

```
margin_db = max(snr over the history) - snr_required(SF) - P2P_ADR_MARGIN_DB (10)
n_step    = floor(margin_db / 3)
n_step > 0 : tx_power -= 2 dB * n_step, not below P2P_TX_POWER_MIN_DBM (2)
n_step < 0 : tx_power += 2 dB * |n_step|, not above the ceiling (§3.4)
```

- `snr_required`: SF7 −7.5, SF8 −10, SF9 −12.5, SF10 −15, SF11 −17.5, SF12 −20 dB (LoRa demodulator floor).
- The history is cleared after every change, so each step is judged on fresh uplinks at the new power.
- The central's SNR saturates at about +12…+14 dB on a strong link. A bench node therefore steps down to the 2 dBm floor, which is correct because its RSSI margin stays large.

### 3.3 Recovery (parity with the LoRaWAN ladder, rung 1)

As soon as the link reaches WARNING (`P2P_WARNING_FAIL_THRESHOLD` = 3 consecutive fully-failed confirmed cycles, plan 439 T1), the node goes back to the ceiling immediately and clears the history. A self-heal re-join (8 failed cycles) always transmits at the ceiling.

### 3.4 Ceiling and precedence

- The ceiling is `p2p-tx-power` (config, 2..22 dBm; the STICKER RFO_LP path tops out at 14 dBm).
- If the central **assigned** a power in the JoinAccept, the node uses exactly that power and ADR is inactive: the central owns the link budget (D3).
- With ADR off, the node transmits at the ceiling as today.

### 3.5 Configuration

A new, separate P2P parameter, as the owner asked. `lrw_adr` stays untouched: no rename, no migration.

| Parameter | Type | Default | Access | Notes |
|---|---|---|---|---|
| `p2p_adr` | bool | `true` | readable/writable shell + NFC, never over the radio (M-3), `persistent: [device_reset]` | mirrors `lrw_adr` (#350: ADR on by default); new `proto_id` 4 in `proto_group: p2p` |

### 3.6 State and observability

- The ADR power is kept in RAM only. After a reboot the node starts at the ceiling and re-adapts after 20 uplinks, the safe direction.
- `ats radio status` shows `tx power: <n> dBm (adr, ceiling <c>)`, alongside today's `(config)` / `(assigned)`.
- Each change logs `P2P ADR: tx power <a> -> <b> dBm (max snr <s> dB, margin <m> dB)`. A WARNING reset logs `P2P ADR: link degraded, back to <c> dBm`.

## 4. Cost and benefit

- Energy: an SX126x RFO_LP draws roughly 20+ mA at 14 dBm and well under half that at 2 dBm during TX. On a strong link most of the TX energy per uplink is saved.
- Airtime is unchanged (same SF), and so is the duty ledger.
- Code: about one history ring (20 × int8), one step function and one hook in the Ack path and the WARNING transition, all in `app_radio_p2p.c`, plus the configen parameter.

## 5. Tests (when implemented)

- `tests/p2p_logic`: the step computation per SF, the floor and ceiling, history clearing, the WARNING reset, assigned-power precedence, and ADR off.
- HIL on the bench: a strong link steps 14 → 2 dBm within 2 × 20 uplinks. `ats radio ack_drop 12` (three failed cycles) forces WARNING, which must return the node to the ceiling at once.

## 6. Open points

- `P2P_ADR_HISTORY` of 20 means one decision per 20 min at a 60 s interval but one per 5 h at 900 s. A smaller window (10) or a time bound may suit P2P better; to be decided with the owner.
- Whether GetInfo should report the current TX power (a new field) or `ats radio status` is enough.
