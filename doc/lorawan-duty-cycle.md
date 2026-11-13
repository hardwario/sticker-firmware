# LoRaWAN duty cycle

How a STICKER on LoRaWAN keeps the regional duty cycle, and what it means for
the uplink capacity (#526).

## Who enforces it

**LoRaMac alone.** It keeps time credits **per band** (the bands of
`RegionEU868`, ETSI EN 300 220) and, for every frame, picks a channel of a band
that still has credits. Only when every band with an enabled channel is out of
credits does it refuse the frame (`LORAMAC_STATUS_DUTYCYCLE_RESTRICTED`).

The application then holds the frame for exactly the wait the MAC computed
(`lorawan_get_duty_cycle_wait_ms()`, from the sticker-zephyr fork) plus 100 ms,
and notes the hold for the M-2 stale-uplink check, so a node held by the duty
cycle is never forced to rejoin (F29). Nothing is retried blindly.

The app ledger (`app_radio_duty_*`) runs without a limit on LoRaWAN: it only
reports the air of the trailing hour (`airtime_hour` in `get_radio_state`).
On P2P it still enforces the limit (doc/p2p.md §6.6).

Before #526 the ledger enforced **one 1 % allowance over all bands** on EU868,
on top of the MAC. That was half of the legal air with the usual channel plan,
and a full ledger held every uplink, telemetry included, for up to an hour.

## EU868 bands

| Channels | Band | Limit | Source |
|---|---|---|---|
| 868.1 / 868.3 / 868.5 MHz | 868.0–868.6 MHz | 1 % | default channels |
| 867.1, 867.3, 867.5, 867.7, 867.9 MHz | 865.0–868.0 MHz | 1 % | CFList at join |
| 868.7–869.2 MHz | | 0.1 % | not used by the usual plan |
| 869.4–869.65 MHz | | 10 % | RX2 only in the usual plan |

With the usual 8-channel plan (ChirpStack `eu868`, TTN) a STICKER therefore has
**2 × 36 s of air per hour**.

**Network requirements: none.** The 867.x channels come in the standard CFList
and an 8-channel gateway listens on all of them. A network that sends no CFList
leaves only the three default channels, i.e. one band and 1 % (the same as
before #526).

Notes:

- The MAC credits live in RAM. A reboot or a rejoin starts with full credits,
  but before the join LoRaMac applies its own stricter join duty cycle
  (first hour 36 s, then 36 s per 10 h, then 8.7 s per 24 h).
- Other regions: US915/AU915 have no duty cycle (dwell time only), AS923
  follows its band plan in LoRaMac. The ledger has no limit there either.

## Capacity (EU868, DR0 / SF12, bench 2026-11-12)

Event log in `event-format raw`: 10 B header + 3 B per event, i.e. 13 events
in a full 51 B frame (~2.8 s of air at SF12). Burst of 400 alarm edges, fixed
DR0, ChirpStack v4, 8-channel gateway:

| | 1 % ledger (before) | LoRaMac per band (after) |
|---|---|---|
| frames until the duty cycle holds | 13 (10 on fPort 4) | 22 (18 on fPort 4), both bands |
| events in them | 107 | 209 |
| ≈ events per hour at DR0 | ~110–130 | ~220–260 |
| next uplink after the hold | after the oldest air left the hour (> 20 min, telemetry held too) | telemetry 2.5 min later |

At DR5 (SF7) a full frame takes ~0.1–0.3 s of air, so the duty cycle is
rarely the limit there.

## Checking it on a device

- `ats device info` → `Radio link: … duty hold N s`: how long sends have been
  refused by the duty cycle.
- `get_radio_state` → `airtime_hour`: air of the trailing hour (can exceed
  36 s on EU868, up to ~72 s with two bands).
- ChirpStack device events: consecutive uplinks alternate between 867.x and
  868.x channels while a backlog drains.
