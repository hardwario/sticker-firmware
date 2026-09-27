# Radio transport layer: app_radio over app_radio_lrw / app_radio_p2p

Goal (owner decision, 2026-09-26): STICKER should behave the same on LoRaWAN and on LoRa P2P wherever the medium allows. All application traffic goes through one abstract radio layer, `app_radio`, which only then splits into the LoRaWAN or the P2P backend.

Base: feat-p2p. This plan shipped with the first step, PR #439 (T1a); the 2026-09-27 update (§3a, T2 split) ships with T2a.

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

## 3a. Decisions (2026-09-27)

Taken after the LoRaWAN vs P2P code comparison of feat-p2p `f852668` + #457 + #458.

6. **Unify everything:** every policy that exists in both backends moves into `app_radio`. A backend keeps only what the medium forces (§2).
7. **LoRaWAN keeps its rejoin on every boot.** The session is deliberately not kept across a reboot. Pulling the batteries is the field recovery for a broken link, and it must always end in a fresh join.
8. **LoRaWAN gets duty-cycle tracking.** The exact sliding-hour ledger of P2P becomes common. LoRaWAN uses it to wait exactly until its frame fits, instead of the blind 8 × 15 s retries that end in an abandoned frame when the EU868 band credit is spent (DR0: ~50 min).
9. **Confirmed alarms are a config option on both radios:** `radio-alarm-ack` (bool, default false). With false, alarm frames go unconfirmed, as LoRaWAN does today. On P2P this reverses decision #22 §3.2, where alarms are always confirmed. With true they go confirmed on both radios, through the common ACK retry ladder (decision 3).
10. **Per-kind TX queues on both radios:** answers, alarms, telemetry and history each get their own queue and priority, as LoRaWAN has today. P2P shares one 2-slot queue for answers and alarms today.
11. **Unchanged:** P2P stays EU868-only, and its session survives a reboot (the pairing is in NVS).

## 4. Steps

| Step | Content | Parity items |
|---|---|---|
| T0 | Sync v1.5.0 → feat-p2p (#437) | done (63c285a) |
| **T1a** (#439, merged 5f1ed63) | Rename `app_lrw`/`app_p2p` → `app_radio_lrw`/`app_radio_p2p` (files, symbols, log modules, docs), with a v1.5.0 sync (#438). No behaviour change. | — |
| **T1** (#440, merged a4a4e88) | Generic link state + events + last downlink + clock check through `app_radio`. GetInfo, NFC, LED, `device_status` and `ats device info` all go through the layer. Fix P2P readiness during a re-join. The TX path is unchanged. | readiness, LED, RADIO_LINK_DOWN, Info last_dl, clock check |
| T2 (split, 2026-09-27) | Common TX core, in four PRs: | |
| T2a | One work queue in `app_radio` (`app_radio_work_q()`); both backends run on it, and their own 4 KB stacks go. No behaviour change. | −4 KB RAM |
| T2b | Per-kind TX queues + priority scheduler in `app_radio` (answer > alarm > telemetry; a history replay owns the radio but never holds alarms). A common refused-frame policy: wait for the duty cycle or 15 s, at most 8 times, then abandon; telemetry is abandoned with `app_compose_reset()`. Over-budget recovery by kind (#409 3g). The backend gets `send(frame, attempt)` with one result set (sent / duty-blocked + wait / busy / not connected / too big / I/O). P2P stops dropping a frame on a hard error. | decision 10, frame drop |
| T2c | Confirmed uplinks in `app_radio`: one confirmed frame in flight, retry n after a random 1..2^n s plus the duty wait, at most 3 retries (P2P resends the same counter; LoRaWAN sends a new FCnt). New config `radio-alarm-ack` (default false): alarm frames confirmed or not on both radios. | decision 9 |
| T2d | Common duty-cycle ledger (P2P's exact sliding hour with folding), charged with each frame's time on air. Its limit comes from a table of the EU868 sub-bands (0.1 / 1 / 10 %) keyed by frequency; P2P charges the one 1 % band today whatever `p2p-frequency` is. LoRaWAN checks it before `lorawan_send()` and schedules the retry for when its frame fits. RadioState airtime for LoRaWAN too. | decision 8 |
| T3 | Common link supervision: HEALTHY / WARNING / RECONNECT, the check cadence (every N-th report, every report in WARNING), the thresholds and the rejoin budget. Any authenticated downlink counts as a success, at most once per RX window (fixes the #458 double count of a LinkCheckAns). Rejoin backoff with jitter; the M-2 stale state; the announce retry pacing. Backend hooks: `request_check` (LinkCheckReq / FCtrl CONFIRMED), `warning_step` (#424 DR/TX ladder / +2 dB), `rejoin`. | link health parity |
| T4 | Common downlink path: command dispatch and answer, the post-command executor (drain-wait on the common queues), the page stream, the history replay state machine (cursor, frame cap, retry, finish) and the deferred clock_sync Info. `radio` command allow-list; oversize → BUDGET_TOO_SMALL. | command parity |
| T5 | Rejoin/reset API (NFC, cmd, settings, reset tiers); `p2p-*` readable in GetConfig/NFC; shell sub-commands only for the active backend. P2P suspend stops its timers. The `ats radio rx1_delay` shell setter gets the JoinAccept guard of 1..15 s. | reset tiers, config, shell |

Each step is a PR into feat-p2p with native_sim tests of the moved logic (a fake backend) and HIL on both radios before its merge. The backends shrink with each step; T2b–T4 are expected to remove ~800–1200 lines.

In parallel, outside the refactor (bench findings from 2026-09-26):
- F-P2P-3: `SX126xWaitOnBusy()` has no timeout. A hang after an RX timeout mid-packet was reproduced on hardware. The bug is in the Zephyr driver, so it also affects LoRaWAN.
- F-P2P-2: the RX1 window has no fixed-ms margin. Measured: the window really closes ~22 ms after the formula, and SF7 needs the Hub aim at −15.
- F-P2P-1: the 48-entry duty ledger means a 60 s cadence is unsustainable.

## 5. Verification per step

- Builds: LoRaWAN debug and release, P2P bench. Flash and RAM are checked against the previous step.
- `clang-format`, decoder tests, `pytest scripts/west_commands/tests`, `tests/run_native.sh`.
- From T1 on: native_sim ztest for the common layer with a fake backend.
- HIL on the bench STICKER in both modes (LoRaWAN via the Hub NS, P2P via the Northbridge) before each merge.
