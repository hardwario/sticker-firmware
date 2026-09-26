# Radio transport layer: app_radio over app_radio_lrw / app_radio_p2p

Goal (owner decision, 2026-09-26): STICKER should behave the same on LoRaWAN and on LoRa P2P wherever the medium allows. All application traffic goes through one abstract radio layer, `app_radio`, which only then splits into the LoRaWAN or the P2P backend.

Base: feat-p2p. This plan ships with the first step, PR #439 (T1a).

## 1. Where we start

- `app_radio.c` (230 lines) is a switch. Every call does `if (p2p) app_p2p_x() else app_lrw_x()`.
- All policy lives twice, once in `app_lrw.c` (~2600 lines) and once in `app_p2p.c` (~3350 lines), slightly different each time: queues, retries, jitter, the boot announce, the stale-uplink watchdog, link state, paging, post-command actions and history replay.
- A parity review of feat-p2p @ 43db2a9 found 15 user-visible differences. Examples:
  - P2P sends no boot Info or #412 settings-info.
  - GetInfo/NFC report `lrw_state` idle and no last-downlink RSSI/SNR in P2P mode.
  - P2P reports HEALTHY while it re-joins.
  - P2P drops responses and alarms while not paired.
  - P2P drops a telemetry snapshot without `app_compose_reset()`.
  - P2P has no stale-uplink watchdog and no pre-send jitter.
  - `force_send`, `sample` and `buzzer_play` are refused over P2P.
  - No reset tier clears the P2P pairing.
- Parts of the app call `app_lrw_*` directly, bypassing the facade: `app_cmd.c`, `main.c` (NFC `lrw_join`/`lrw_reset`), `app_settings.c`, `app_ats.c` and `app_calibration.c`. In P2P mode those calls read an uninitialised LoRaWAN module.
- Both backends allocate their own 4 KB work-queue stack statically, although only one runs.

## 2. Target

```
app_report  app_alarm  app_cmd  app_history  main/LED  NFC Info  shell
        \        |        |          |           |        |       /
         +--------------------- app_radio ------------------------+
         | one TX work queue + scheduler, queues, retry (incl. the  |
         | confirmed-uplink/ACK retry ladder), jitter, announce,    |
         | stale watchdog, link state, downlink commands, post-cmd  |
         | actions, paging, history pacing, last downlink, clock    |
         | check, rejoin/reset API, "radio" command allow-list      |
         +---------------+------------------------+-----------------+
                         |   backend ops + events  |
                 app_radio_lrw (LoRaMac glue)  app_radio_p2p (P2P link, crypto, PHY)
```

- **Common (`app_radio`):**
  - the TX scheduler and queues, with frames parked, not dropped, while not connected or duty-blocked;
  - retry and abandon policy (always `app_compose_reset()`) and the retry ladder for confirmed uplinks;
  - pre-send jitter and a `_now` variant;
  - the boot/join announce (Info + settings-info, deferred when the budget is too small);
  - the stale-uplink watchdog, including the F29 duty-cycle hold;
  - a generic link state feeding the LED, `device_status` and Info;
  - downlink command dispatch and post-command drain-wait;
  - paging and history replay pacing;
  - last-downlink RSSI/SNR/age;
  - the clock sanity check;
  - `app_radio_rejoin()` / `app_radio_reset_session()`.
- **LoRaWAN backend:** join, LoRaMac, region/sub-band/channel plan, ADR and manual DR, the link-check ladder, DeviceTimeReq, over-budget recovery, the MAC-flush uplink, NVM.
- **P2P backend:** JoinRequest/Accept, SF sweep, session keys and crypto, fcnt reservation, RX1 window and pending flag, resending the same counter on a retry, self-heal, the duty ledger, the time tail, the assigned TX power and (next) the downlink channel.
- **Backend interface:** a small ops struct (init/start/rejoin/reset_session/suspend/max_payload, a blocking `send(kind, buf, len, attempt)` returning a unified result, and get_status). Events go upward: joined, join_failed, degraded, link_lost, downlink(bytes, rssi, snr), time(unix).
- **One work queue** owned by `app_radio`. This saves one 4 KB stack.

## 3. Decisions (2026-09-26)

1. Branch: feat-p2p. T0 syncs v1.5.0 first; T1a–T5 are PRs into feat-p2p.
2. The layer keeps the name `app_radio`; the backends become `app_radio_lrw` and `app_radio_p2p`.
3. ACK retries move into the common layer (confirmed uplinks generalised; the backend gets `attempt`, and P2P resends the same counter). LoRaWAN may use confirmed uplinks as an option; the default stays unconfirmed.
4. No separate helper module per feature: transport code lives in `app_radio.c`, `app_radio_lrw.c` and `app_radio_p2p.c` only, tested with the TESTABLE pattern (see `tests/p2p_logic`). `app_lrw_stale` was folded back by #438.
5. No tracking issue; this plan is the tracker.

## 4. Steps

| Step | Content | Parity items |
|---|---|---|
| T0 | Sync v1.5.0 → feat-p2p (#437) | done (63c285a) |
| **T1a** | Rename `app_lrw`/`app_p2p` → `app_radio_lrw`/`app_radio_p2p` (files, symbols, log modules, docs), with a v1.5.0 sync (#438). No behaviour change. | — |
| T1 | Generic link state + events + last downlink + clock check through `app_radio`. GetInfo, NFC, LED, `device_status` and `ats device info` all go through the layer. Fix P2P readiness during a re-join. The TX path is unchanged. | readiness, LED, RADIO_LINK_DOWN, Info last_dl, clock check |
| T2 | Common TX scheduler + queues + result codes + retry ladder; `send()` in both backends; jitter, compose reset, parking on not-connected; one work queue. | telemetry drop, frame drop, jitter |
| T3 | Common announce (Info + settings-info) + common stale watchdog (from `app_radio_lrw.c`). | boot announce, M-2 in P2P |
| T4 | Common downlink path, post-cmd actions, paging, history replay; `radio` command allow-list; oversize → BUDGET_TOO_SMALL. | command parity |
| T5 | Rejoin/reset API (NFC, cmd, settings, reset tiers), `p2p-*` readable in GetConfig/NFC, shell sub-commands only for the active backend. | reset tiers, config, shell |

In parallel, outside the refactor (bench findings from 2026-09-26):
- F-P2P-3: `SX126xWaitOnBusy()` has no timeout. A hang after an RX timeout mid-packet was reproduced on hardware. The bug is in the Zephyr driver, so it also affects LoRaWAN.
- F-P2P-2: the RX1 window has no fixed-ms margin. Measured: the window really closes ~22 ms after the formula, and SF7 needs the Hub aim at −15.
- F-P2P-1: the 48-entry duty ledger means a 60 s cadence is unsustainable.

## 5. Verification per step

- Builds: LoRaWAN debug and release, P2P bench. Flash and RAM are checked against the previous step.
- `clang-format`, decoder tests, `pytest scripts/west_commands/tests`, `tests/run_native.sh`.
- From T1 on: native_sim ztest for the common layer with a fake backend.
- HIL on the bench STICKER in both modes (LoRaWAN via the Hub NS, P2P via the Northbridge) before each merge.
