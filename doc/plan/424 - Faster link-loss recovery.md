# PR #424 — Faster link-loss recovery (DR step-down in WARNING, US915/AU915 sub-band fix)

App branch: `hynek/lrw-link-recovery` → `v1.5.0`. No Zephyr fork change. Builds on the #71 state machine
(HEALTHY → WARNING → RECONNECT) and the #421 MAC lock.

## 1. Problem

Scenario: the device loses its network. Either its gateway goes away, or the device is moved and no
longer reaches a gateway on the data rate ADR had optimised it to. Uplinks are unconfirmed, so the
only signals are the periodic LinkCheck, any downlink, and LoRaMac's internal ADR ACK counter.
The M-2 stale-uplink watchdog does not help here: an unconfirmed `lorawan_send()` succeeds locally
even with no gateway.

Behaviour on `v1.5.0` @ `41cb9d4`:

| Mechanism | Effect |
|---|---|
| App state machine | 3 consecutive LC failures (LC every 5th report) → WARNING. 5 more LC failures (still every 5th report) → RECONNECT → OTAA rejoin. WARNING changed nothing, neither the DR nor the check cadence. |
| Rejoin | DeInit + `lorawan_start()` + JoinRequest at the region default DR (EU868 DR0/SF12). This was the **only** way the device ever reached a lower DR. |
| LoRaMac ADR backoff | `AdrAckCounter` counts uplinks without an RX1/RX2 downlink: 64 → ADRACKReq, 96 → default TX power, 128/160/192/… → DR −1. First DR step after ~32 h at 900 s, so the rejoin always came first. |
| Network ADR | Only reacts to uplinks it receives, so it can do nothing on a total loss. ChirpStack never lowers the DR by design (that is the device's backoff). |

Detection time ≈ `(3 + lrw-link-check-fail-rejoin) × lrw-link-check-interval × interval_report`.
With the defaults (5, 5, 900 s) that is 35–40 reports ≈ **9–10 h** of blind transmission on the old DR
before the first rejoin. HIL T7 (0923, 60 s / LC 1) measured 7 min, consistent with the formula.

Separately, **US915/AU915** lost the configured sub-band after repeated failed joins:
- `apply_subband()` ran only at boot and only set `MIB_CHANNELS_MASK`.
- `ResetMacParameters()` (every OTAA join) copies `ChannelsDefaultMask` (all-64) over the active mask.
- Once the sub-band's 8 channels in `ChannelsMaskRemaining` were used up, `RegionUS915NextChannel()`
  refilled the pool from all 64 channels.
- JoinRequests then cycled over all eight sub-bands, so only ~1 in 8 hit an 8-channel gateway,
  ≈ 8 h extra at the hourly rejoin backoff.

## 2. Design

### 2.1 Link-check on every report in WARNING (`685d450`)

`should_request_link_check()` returns true in WARNING, right after the `interval <= 0` early-out:
`lrw-link-check-interval 0` still disables link checks, which also keeps the `ats lrw lc` injection
tests deterministic. HEALTHY keeps the N-th-report cadence, so there is no extra airtime while the link is fine.

### 2.2 Recovery ladder (`b0254ca`)

`lrw_backoff_step()`, one rung per failed LC. Each rung does two things:

1. **TX power** — if it is weaker than the region default (index > default), restore the default,
   which is the maximum EIRP.
2. **Data rate** — if above `lorawan_get_min_datarate()` (dwell-aware), drop by one step.
   - ADR on: `MIB_CHANNELS_DATARATE` under `lorawan_mac_lock()`. `lorawan_set_datarate()` refuses
     while ADR is on, and with ADR on the MAC uses `ChannelsDatarate` as the ADR starting point.
   - ADR off: `lorawan_set_datarate()`. `lorawan_send()` passes its own DR with every frame and
     that overrides the MIB.

It returns true if a rung was taken. The payload budget is refreshed after each rung.

Where it runs:
- On the transition into WARNING (the three failures are already evidence).
- On every LC failure in WARNING.
- **Rejoin rule:** RECONNECT only when the rung was *not* taken (the ladder is at the floor) **and**
  `m_warning_lc_fail_total ≥ lrw-link-check-fail-rejoin`.
- The ladder is finite (min DR, default power), so a rejoin always follows eventually.
- On LC success in WARNING the device returns to HEALTHY **on the lower DR, same session**. With ADR on,
  the network raises the DR again from the uplinks it now receives.

LC timeout timing:
- `m_lc_timeout_timer` now starts after `lorawan_send()` returns. The send returns once the RX windows
  have closed, so any LinkCheckAns has already been handed to `m_work_q`.
- Previously the timer started before the send and the 10 s had to cover airtime + RX1/RX2 delays.
  At DR0/SF12 with a 5 s RX1 delay (TTS default) that is ≈ 2.8 s + 5 s + 1.2 s ≈ 9–10 s, a race
  exactly on the ladder's bottom rung.

`ats lrw status` prints the TX power index (0 = max).

Timeline from EU868 DR5, defaults:

| Step | Reports | Note |
|---|---|---|
| Link loss → WARNING | 11–15 | unchanged (HEALTHY cadence) |
| WARNING entry | — | rung 1: default TX power, DR5 → DR4 |
| WARNING fails 1–4 | 4 | rungs 2–5: DR4 → DR0 |
| WARNING fail 5 | 1 | floor + budget 5 reached → RECONNECT |
| **Total** | **16–20 (≈ 4–5 h)** | was 35–40 (≈ 9–10 h) |

A device that reaches its gateway again on DR3 returns to HEALTHY two reports after entering WARNING,
without a rejoin.

### 2.3 Sub-band as the default mask (`308f7cd`)

- `apply_subband()` sets `MIB_CHANNELS_DEFAULT_MASK` before `MIB_CHANNELS_MASK`. The active-mask
  setter trims the default's 500 kHz word, so the order matters.
- New `apply_channel_plan()` wraps the region check. It is called after every `lorawan_start()`:
  at boot as before, and **also** in the rejoin path, because `LoRaMacInitialization()` resets the
  masks and the NVM restore brings back a snapshot.
- The ADR backoff's "activate default channels" at the minimum DR now also stays on the sub-band.

## 3. Interactions / non-goals

- **#409 A3 (manual DR, not in `v1.5.0` yet):** a pinned DR is now stepped down by the ladder until
  the next join re-pins it. This mitigates the "joined but silent" loop, where a pinned DR is out of
  reach after a relocation.
- **ABP** never rejoins. It now walks the ladder and then stays in WARNING with the per-report LC,
  instead of waiting for LoRaMac's 128-uplink backoff.
- **Not addressed here** (from the same analysis):
  - Telemetry and alarms sent while the link is down are lost (unconfirmed; `history_enable` defaults
    to false; replay only on a server `ReqHistory`). See #409 A4 (confirmed alarms).
  - US915/AU915 land on an 11 B budget after a rejoin (DR0 / DR2 dwell). See #409 A5a.
  - EU868 multi-frame snapshots at DR0 vs. the 1 % duty cycle (8 × 15 s frame retries) are to be verified on HW.
  - The HEALTHY phase still needs 3 failures at the normal cadence.

## 4. Cost

| Build | FLASH | RAM |
|---|---|---|
| release | 73.46 % → 73.61 % (+328 B) | 80.19 % (+0 B) |
| debug | 87.88 % → 88.20 % (+784 B) | 93.94 % (+0 B) |

## 5. Verification

- clang-format 22.1.5 clean; release + debug build against `v4.3.0-sticker2-branch`, no new warnings.
- native ztest 11/11, JS decoder 70/70, configen pytest 28/28. `app_lrw.c` has no native coverage.
- HW: `doc/manual-test-plan.md` **L18** (ladder, EU868) and **L19** (US915 sub-band persistence).
  L8 and L13 were updated for the new rejoin rule.
