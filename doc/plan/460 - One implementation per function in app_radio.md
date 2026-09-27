# One implementation per function in app_radio

Goal (Hynek, decision #23, 2026-09-27): every STICKER radio function has one implementation.
Most behaviour lives once, in `app_radio.c`. A backend keeps only what its medium forces:

- **LoRaWAN:** join and LoRaMac, region/channel plan, ADR, NVM.
- **P2P:** JoinRequest/Accept, crypto, fcnt, the RX1 window and the time tail.

Parity is proven by one test suite that runs the same scenario against both radios.

Base: `hynek/radio-t2` (#459, T2a: one radio work queue), which is stacked on #457 (decision #22). This plan continues [`439 - Radio transport layer.md`](439%20-%20Radio%20transport%20layer.md). It implements that plan's steps T2b, T2c, T2d, T3 and T4 in this PR, together with the parity fixes F1–F4 agreed with the Nodes test session.

## 1. What is still twice (feat-p2p + #457 + #458 + #459)

| Function | LoRaWAN (`app_radio_lrw.c`) | P2P (`app_radio_p2p.c`) |
|---|---|---|
| TX queues | answers 4, alarms 4, downlinks 2; telemetry is a coalescing flag | one 2-slot queue for answers + alarms, a 1-slot duty-deferred frame, a 3-slot ACK-retry queue |
| Priority | answer > alarm > telemetry | FIFO for answers/alarms, telemetry after the queue drains |
| Refused frame | requeue, retry after 15 s, telemetry ≤ 8 × then `app_compose_reset()` | duty-blocked → wait out the ledger; a hard error drops the frame |
| Over budget | by kind: Info/settings re-arm, answer → BUDGET_TOO_SMALL, alarm dropped | fixed 64 B body, not handled |
| Confirmed uplinks | none (alarms unconfirmed) | answers + alarms + every N-th report; ACK retry 1..2^n s, ≤ 3, same counter |
| Link check cadence | LinkCheckReq piggyback | FCtrl CONFIRMED report — **one rule since F1** |
| Link supervision | 3 fails → WARNING, 1 ok → HEALTHY; TX-power/DR ladder; rejoin; backoff 60 s → 1 h ±25 % | 3 fails → WARNING, +2 dB step, rejoin after `radio-link-check-fail-rejoin`, backoff 60 s → 1 h |
| Downlink = success | any downlink (#458) | any Ack |
| Post-command actions | 9 actions, drain-wait 8 s × 6 on 2 signals | 5 actions, drain-wait on 4 signals; NFC has a third table in `main.c` |
| Page stream | ≥ 2 free answer slots, 2 s pace | empty TX queue, 2 s pace |
| History replay | cursor/cap/8 retries/3 s gap/15 s retry, DR-driven cap | same machine copied, fixed cap |
| clock_sync answer | Info on `LORAWAN_TIME_UPDATED` | Info on an Ack with the time tail — **common entry since F1** |
| M-2 stale watchdog + duty hold | heartbeat in the backend | own copy |
| Announce retry | 15 s | 5 s |
| Duty cycle | LoRaMac's hourly band credits, blind 8 × 15 s retries | exact sliding-hour ledger, one 1 % band |

## 2. Target shape

```
app_report  app_alarm  app_cmd  app_history  NFC  shell
      \         |         |         |         |     /
       +----------------- app_radio.c ----------------+
       | frame queues + scheduler + refused-frame rule |
       | confirmed ladder, duty ledger, link supervision|
       | downlink dispatch, post-cmd executor, paging,  |
       | history replay, clock_sync, M-2, announce      |
       +------------------ ops + events ----------------+
              app_radio_lrw.c            app_radio_p2p.c
```

Everything stays in the three existing files (decision 4 of plan 439: no helper modules). The one exception is the post-command executor. It is not transport code, so it goes into `app_cmd.c`, where NFC can call it as well.

### 2.1 Backend ops

A small const struct, selected once at `app_radio_init()` from `radio-mode`. F4 brings the TX part; F2, T2c and T2d add their ops (`request_check`, `warning_step`, `rejoin`, `airtime_ms`) when they land:

```c
struct app_radio_backend {
	int (*send)(const struct app_radio_frame *f,
		    struct app_radio_tx_result *res); /* blocking, radio WQ */
	uint8_t (*budget)(void);        /* payload budget of the next uplink, 0 = none now */
	bool (*tx_ready)(void);         /* the link carries uplinks now */
	uint8_t (*report_flags)(void);  /* a report starts: CONFIRMED / LINK_CHECK */
	void (*report_done)(void);      /* the last frame of a report left */
	bool (*replay_active)(void);    /* a history replay owns the radio */
	uint8_t confirm_kinds;          /* kinds sent confirmed: LRW none, P2P answer/alarm/history */
	uint16_t frame_gap_ms;          /* between the frames of a report: LRW 3 s, P2P 0 */
};
```

`send()` returns one result set. The common layer handles each result the same way whichever radio is running:

| Result | Meaning | Common reaction |
|---|---|---|
| `0` | transmitted | the next frame; `report_done()` after the last frame of a report |
| `-EAGAIN` + `res->wait_ms` | duty-held, or LoRaWAN sent a MAC flush instead (`-ECONNREFUSED`, budget 0) | resend at `wait_ms` (0 = 15 s). Not counted for an answer or alarm, which waits as long as the hold lasts; counted for telemetry, whose data ages |
| `-EBUSY` | a confirmed uplink is in flight (P2P ack retry, listen window) | park; the backend kicks `app_radio_tx_kick()` when it ends |
| `-ENOTCONN` | not joined / not paired | park, don't count; kicked on link-up. Telemetry abandons the report (snapshot reset) |
| `-EMSGSIZE` + `res->budget` | over the current budget | over-budget recovery by kind (§2.3); a telemetry frame is dropped and the report goes on (M-10) |
| other | radio / MAC error | retry after 15 s, counted in `RETRY`; dropped after 8 retries |

Each refused send is counted in `TX_ERR` by the backend. A kick never cuts short a wait that is already scheduled (retry, duty hold, frame gap).

A backend reports upward through events: `joined`, `join_failed`, `link_result(ok)`, `downlink(bytes, rssi, snr)` and `time(unix)`. They are all called on the radio work queue.

### 2.2 Frames and queues (F4 = T2b)

```c
enum app_radio_frame_kind { ANSWER, ALARM, TELEMETRY, HISTORY };  /* app_radio_kind is LoRaWAN/P2P */
struct app_radio_frame { uint8_t kind, tag, port, flags; uint16_t len; uint8_t *buf; };
/* flags: CONFIRMED (P2P FCtrl), LINK_CHECK (LoRaWAN LinkCheckReq), MORE (report frames follow) */
```

- `tag` is OTHER / CMD_RESPONSE / INFO / SETTINGS, and it drives the over-budget recovery. `port` is the LoRaWAN fPort of an answer (0 = the command port). P2P maps each kind to its frame type.
- **Queues:** answers 4, alarms 4, 64 B slots (decision 10: "4+4"), in `app_radio.c`. The frame being sent is taken off its queue and kept until it left or was given up, so a retry keeps the order. A page stream leaves two answer slots free.
- **Telemetry** stays a coalescing request (#340 M5): requests before the report left fold into it; a request that arrives while a report is on the air is a new report after it. It is composed frame by frame at send time against the budget of that moment; `report_flags()` is asked once per report, and the link check rides the first frame only (#188).
- **Budget 0** (pending LoRaWAN MAC answers fill the frame, H-1): an empty telemetry frame flushes the MAC; mid-report the report ends with a snapshot reset.
- **History** stays the backends' replay state machine until F3 (§2.5). It holds telemetry while it runs, never answers or alarms.
- **One scheduler work item** on the radio work queue sends one frame per run: the frame a retry waits for, then answer > alarm > telemetry (> history after F3). The first report after a link-up therefore follows the Info, settings-info and held alarms on either radio.
- **Refused-frame rule:** at most 8 counted retries, 15 s apart. After that the frame is abandoned; an abandoned telemetry frame always calls `app_compose_reset()`.
  - This fixes the P2P drop on a hard error, and P2P's missing `app_compose_reset()`.
  - LoRaWAN no longer requeues a failing answer at the tail of its queue forever.
- **Parking:** frames wait while the radio is not connected. They are not dropped, which fixes the P2P drop while unpaired; `pairing_clear` purges only the P2P ack-retry queue. A link lost mid-report abandons that report with a snapshot reset (#93.5).
- **seq_release** (the boot/join hold of #452) no longer waits for the backend to go idle: the held alarms and the first report are queued behind the announce frames and the scheduler order keeps them there.

P2P loses `m_tx_msgq`, `m_tx_deferred`, its frame retry state and the telemetry wait-for-queue logic. LoRaWAN loses `m_response_msgq`, `m_alarm_msgq`, `send_work_handler`, `tx_send_queued` and its frame/telemetry work items. The downlink queue stays in the LoRaWAN backend, because P2P receives inside `send()`.

### 2.3 Over-budget recovery (common)

`-EMSGSIZE` is handled by kind:

- **Info / settings-info:** re-arm the announce for the next budget change.
- **Answer:** replace it with `Error BUDGET_TOO_SMALL` for the same seq (`app_cmd`).
- **Alarm:** drop it and log. The alarm engine re-fires while the condition holds.
- **History:** finish the replay with `BUDGET_TOO_SMALL`.

On P2P the budget is fixed, so these paths are dead but harmless.

### 2.4 Link supervision (F2 = T3)

Implemented in `app_radio.c` ("Link supervision" block); tests in `tests/radio_common` (both profiles).

- **State:** WARNING is app_radio's overlay on the backend's HEALTHY: `app_radio_get_state()` returns WARNING while the backend says HEALTHY and the link is degraded. JOINING / RECONNECT / IDLE / DISABLED stay backend states (join and MAC are per carrier).
- **Backend ops:**
  - `report_flags(due)`: how a due link check rides a report. LoRaWAN adds a LinkCheckReq unless one is still pending; P2P sends the report CONFIRMED, and a pending clock_sync forces up to 3 reports confirmed.
  - `get_state()`.
  - `warning_step()`: one recovery rung. LoRaWAN takes the DR / TX-power ladder; P2P raises TX power 2 dB towards `p2p-tx-power`. Returns false once no rung is left.
  - `rejoin(forced)`: returns `-ENOTSUP` when it cannot rejoin (LoRaWAN ABP unless forced, P2P unprovisioned).
- **Outcome:** the backend reports `app_radio_link_result(ok)` and keeps its own answer timeout (LoRaWAN 10 s, P2P the RX1 window plus retries).
  - Any authenticated downlink counts as a success. Successes are idempotent, so a downlink followed by its LinkCheckAns cannot double count.
  - While the backend is not HEALTHY, results are ignored (the next `app_radio_link_up()` starts afresh). `APP_RADIO_CNT_FAIL` still counts every failure.
- **Cadence:** `app_radio_link_check_due(reports, N, warning)`, common since F1. app_radio counts completed reports, resets the count at `app_radio_link_up()`, and passes `due` to `report_flags()`.
  - `app_radio_force_link_check()` (`ats lrw check`) makes the next report due. It stays armed until a check actually rode.
- **Thresholds:** 3 failures in a row → WARNING, which takes the first rung at once. 1 success → HEALTHY.
- **In WARNING:**
  - every report is a link check;
  - every failure first tries `warning_step()`;
  - a rejoin is never spent while a rung is untried. After `MAX(radio-link-check-fail-rejoin, 1)` failures in WARNING, a failure with no rung left calls `rejoin(false)`;
  - `-ENOTSUP` stays in WARNING and restarts the budget.
- **Backoff:** `app_radio_rejoin_backoff_ms()` (60 s ×2, capped at 1 h) and `app_radio_backoff_jitter_ms()` (±25 % of the backoff, never under a floor; P2P passes its duty-cycle wait as the floor). Common to both radios.
- **M-2:** the stale-uplink watchdog is common:
  - `app_radio_note_send(sent, duty_held)` feeds the F29 duty hold;
  - a sent telemetry report, a history frame (`app_radio_note_uplink()`) and a link-up refresh the clock;
  - staleness calls `rejoin(true)`.
  - The liveness heartbeat (#182, 5 s tick, 30 s wdog) runs on the radio work queue. Either backend starts it with `app_radio_heartbeat_start()`; P2P feeds it inside its RX window.
- **Semantic changes:**
  - P2P now keeps LoRaWAN's rule of no rejoin while a rung is left. Before, it rejoined after `fail-rejoin` failures in WARNING even while TX power was still rising.
  - P2P keeps the fail streak during RECONNECT until the next link-up, as LoRaWAN does.
  - LoRaWAN takes the first rung on entering WARNING, and counts its fail streak in `app_radio` (RadioState / `ats radio status`).

### 2.5 Downlink path (F3 = T4)

F3a/F3b are implemented (39cb04a); F3c and F3d are next.

- **Commands:** a backend hands each authenticated command to `app_radio_downlink(buf, len)` on the radio work queue. LoRaWAN drains `m_dl_msgq`; P2P calls it from `recv_ack()`.
  - It calls `app_cmd_handle(transport, cap)`. The transport comes from the backend op `cmd_transport` (`APP_CMD_TRANSPORT_LRW` / `_P2P`), and the cap from `app_radio_tx_answer_cap()`.
  - The answer is queued as `CMD_RESPONSE` on port 0.
  - A `PAGE_STREAM` result starts the page stream. Any other action goes to the post-command executor.
- **One post-command executor:** `app_cmd_run_action(action)` in `app_cmd.c`.
  - It is the union of the three old tables (NFC in `main.c`, LoRaWAN, P2P): settings save + reboot, reboot, device / factory / vendor reset, secret-key save, claim-active save, enter calibration, radio session reset, forced rejoin, counters save.
  - `app_cmd_action_reboots()` tells NFC which actions reboot, so it publishes its result first.
  - `app_radio` runs the action once the answer has left. While an answer is queued, or a confirmed frame still awaits its Ack (backend op `in_flight`, P2P only), it re-checks every 8 s, at most 6 times, then runs the action anyway.
  - Which actions a transport can reach is still decided by the configen transport lists.
- **Page stream:** one page every 2 s, and only while ≥ 2 answer slots are free. It stops when the link leaves HEALTHY, and kicks the announce when it ends.
- **Announce:** Info → settings-info → first telemetry (#452), now in `app_radio`.
  - A retry every 5 s only re-checks queue room, and the announce waits for a running page stream.
  - The backends only call `app_radio_announce_kick()`; LoRaWAN does so on a DR change.
- **Next, F3c:** history replay as one state machine (cursor, per-frame cap from `budget()`, 8 retries, 3 s gap / 15 s retry, finish → report kick). The re-entrancy guard from P2P B8 stays.
- **Next, F3d:** clock_sync as a backend `time(unix)` event (LoRaWAN `LORAWAN_TIME_UPDATED`, the P2P Ack time tail). The "landed < 60 s ago" fast path is F1.
- **Footprint (release):** 180872 B flash / 55104 B RAM, against 179528 / 55232 for F2. About 2 KB of the flash growth is LTO inlining into `app_cmd_handle_set_param`.

### 2.6 Confirmed uplinks (T2c) and `radio-alarm-ack`

- **One confirmed frame in flight.** Retry n goes after a random 1..2^n s plus the duty wait, at most 3 retries.
  - P2P resends the same counter; its `send()` sees `attempt > 0`.
  - LoRaWAN sends a new FCnt.
- **New config `radio-alarm-ack`:** bool, default false. It is readable everywhere and writable over shell, NFC and radio, not vendor; the proto group is `alarms`.
  - With false, alarms go unconfirmed on both radios. That is LoRaWAN today, and it reverses #22 §3.2 for P2P; Nodes test amends #22 accordingly.
  - With true, alarms go confirmed on both radios.
- The proto_id is assigned in this PR, and the Manager-App side follows in apps/manager#143.

### 2.7 Duty-cycle ledger (T2d)

- P2P's exact sliding-hour ledger (with folding) moves into `app_radio`. Each frame is charged with `airtime_ms(len)`.
- The limit comes from an EU868 sub-band table (0.1 / 1 / 10 %) keyed by frequency. P2P charges its real band.
- LoRaWAN checks the ledger before `lorawan_send()` and returns `-EAGAIN` with the exact wait, instead of burning retries.
- **Open:** LoRaMac keeps its own hourly credits, which reset only after the hour. To stop the two disagreeing, either take the MAC's `DutyCycleWaitTime` through the sticker-zephyr glue, or keep the ledger conservative. To be decided in T2d from a bench measurement.
- RadioState airtime is filled for LoRaWAN too.

### 2.8 Flash writes vs radio exchanges (fix from the F4 HIL)

- **Problem:** the STM32WLE5 has one flash bank.
  - A program or page erase stalls every instruction fetch, interrupt handlers included. A page erase takes ~22 ms, and an NVS garbage collection takes several.
  - A stall between TX done and the RX1 opening makes the receiver miss the downlink. In the F4 HIL on 2026-09-27, an `alarm new` save lost three P2P Acks, and a LoRaWAN JoinAccept can be lost the same way.
  - The writers run on other threads: shell, NFC, the app_report work queue (history, counters) and the sensor work queue (counters).
- **Gate in `app_radio`:**
  - Backends bracket each exchange, from TX start until its receive windows close, with `app_radio_air_begin()` / `_end()`:
    - `lorawan_send()`;
    - a LoRaWAN join, until JOINING is left;
    - P2P `lora_send()` through its RX1 window (`p2p_rx_window()`);
    - the P2P JoinRequest through the JoinAccept window.
  - Writers bracket each write with `app_radio_flash_hold()` / `_release()`:
    - `app_settings` save and single-key saves (nonce counter, secret key, P2P SF);
    - alarm rules;
    - counters;
    - the NFC claim state;
    - every history program and erase.
- **Waits:**
  - A writer waits for a running exchange, at most 10 s (a LoRaWAN DR0 join).
  - An exchange waits for a running write and for writers already waiting, at most 1 s, so a busy radio cannot starve them.
  - Past either cap the write or the TX goes ahead with a warning. Nothing is dropped.
  - Writers on the radio or the system work queue never wait: the exchange itself runs on the radio work queue, and `LoRaMacProcess()` and the DIO1 work run on the system work queue. They still count as writers, so no TX starts mid-write.
- **Not gated:**
  - LoRaMac's own NVM saves (system work queue). They run after the MAC is done with the RX windows.
  - Resets that erase the storage and then reboot.
- **Tests:**
  - `tests/radio_common`: the writer waits for the exchange, the exchange waits for the write, a waiting writer goes before the next exchange, both caps, and work-queue writers never wait.
  - `tests/p2p_logic`: every exchange ends its air window, failed sends included.
- **Footprint (release):** +780 B flash, +64 B RAM (181652 / 55168).
- **Save time (found in the gate HIL):**
  - A full `settings_save()` walked every NVS name record for every key: a median 3.4 s of CPU per alarm rule save.
  - That is longer than the 1 s an exchange waits, so the TX went ahead mid-save. The saving shell thread shares priority 14 with the radio work queue and a 20 ms timeslice, so the P2P RX1 window opened late: 5 of 8 alarm frames needed retries.
  - `CONFIG_NVS_LOOKUP_CACHE` and `CONFIG_SETTINGS_NVS_NAME_CACHE` bring the same save to a median 0.10 s, for +1024 B RAM and +624 B flash (release 182276 / 56192).
- **HIL (2026-09-27, 0413 + 5722, Hub c55):**
  - Rule saves back to back through 8 alarm uplinks: every confirmed frame acked at the first try, no gate timeout.
  - 3 fresh P2P joins with rule saves inside the JoinAccept window: each joined on the first JoinRequest, and the save waited for the window to close.
  - Downlink get-info, get-settings, set-config and reboot on the F3 path: every answer acked.

## 3. Same-scenario tests

- **New native suite `tests/radio_common`:**
  - It compiles `app_radio.c` with a fake backend that implements the ops struct.
  - There are two backend profiles:
    - LoRaWAN-like: 51 B budget (DR-driven in the scenarios that need it), 3 s gap between report frames, nothing confirmed.
    - P2P-like: 239 B budget (`P2P_MAX_BODY`), no frame gap, answers/alarms/history confirmed.
  - Every scenario runs once per profile.
- **Scenarios:**
  - priority answer > alarm > history > telemetry;
  - parking while not connected;
  - duty wait, then 8 retries, then abandon with `app_compose_reset()`;
  - over-budget recovery by kind;
  - link-check cadence (#1, #N+1, every report in WARNING);
  - thresholds 3/1 and rejoin after N;
  - a downlink counted once per cycle;
  - reconnect backoff bounds;
  - post-command drain-wait and the 6-deferral cap;
  - the page stream keeps 2 slots free;
  - history replay end on cursor exhaustion;
  - clock_sync fast path and pending path;
  - announce order Info → settings → telemetry;
  - `radio-alarm-ack` on/off.
- **Existing suites:**
  - `tests/p2p_logic` keeps the P2P-only logic: crypto, join, fcnt, RX1, time tail.
  - The LoRaWAN glue keeps its own tests.
  - Tests of moved logic move to `radio_common`.

## 4. Steps (commit series in this PR, green builds + native suites after each)

| Step | Content | Parity items |
|---|---|---|
| **F1** | Common link-check cadence (PF-1); fresh-time clock_sync answer + P2P ≤ 3 confirmed reports for a pending clock_sync, answered only on a time tail (PF-2); factory_reset help text (PF-5) | cadence, clock_sync |
| **F4 = T2b** | Frame model, per-kind queues 4+4, one scheduler, refused-frame rule, parking, over-budget recovery; backend `send()` result set | decision 10, frame drop, compose reset |
| **F2 = T3** | Common link supervision, reconnect backoff with jitter, downlink-once-per-cycle, M-2 + duty hold out of the backends | link health |
| **F3 = T4** | Downlink dispatch, `app_cmd_run_action()` for LoRaWAN/P2P/NFC, page stream, history replay, clock_sync `time` event, announce pace | command parity |
| **Fix** | Flash-write gate around radio exchanges plus the NVS lookup caches (§2.8), found in the F4 HIL | RX1 / JoinAccept loss |
| **T2c** | Confirmed ladder in `app_radio`; `radio-alarm-ack` (configen, proto, docs; apps/manager#143) | decision 9 |
| **T2d** | Common duty ledger with the EU868 sub-band table; LoRaWAN exact wait; RadioState airtime | decision 8 |

T5 of plan 439 (the reset tiers, `p2p-*` in GetConfig, shell per backend, suspend timers) stays a separate PR.

## 5. Verification

- **Per step:** release and debug LoRaWAN builds (`LORAMAC_DIR` = fork `v4.3.0-sticker2`), the P2P bench build, `clang-format` 22.1.5, `pytest scripts/west_commands/tests`, `tests/run_native.sh`. Flash and RAM are compared with #459.
- **HIL** on 0413 + 5722:
  - P2P via the Northbridge after F4 and F2, which change on-air behaviour;
  - LoRaWAN via the Hub NS (dual slot after Hub c54);
  - a full both-radio pass before the merge.
- **Merge order:** #457 (flag-day HIL) → #459 → #460. Merges only with the owner's OK.
