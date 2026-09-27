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

- **Commands:**
  - `downlink(bytes)` → `app_cmd_handle(transport, cap = budget())`.
  - The answer is queued as ANSWER, and a page stream starts when the answer does not fit.
  - The transport stays `APP_CMD_TRANSPORT_LRW` / `_P2P` (app_cmd already gates both with `radio_transport()`).
- **One post-command executor:** `app_cmd_run_action(action)` in `app_cmd.c`.
  - It holds the union of today's three tables: `main.c` NFC, LoRaWAN and P2P.
  - NFC calls it after its LED result hook.
  - `app_radio` calls it after a drain-wait on the common queues: the answer queue is empty and no confirmed frame is in flight, checked every 8 s, at most 6 deferrals.
  - Which actions a transport can reach is still decided by the configen transport lists, not by the executor.
- **Page stream:** paced at 2 s. It runs only while ≥ 2 answer slots are free, so an alarm and another answer always fit.
- **History replay:** one state machine (cursor, per-frame cap from `budget()`, 8 retries, 3 s gap / 15 s retry, finish → report kick). The re-entrancy guard from P2P B8 (a request re-delivered from inside the replay's own Ack) stays.
- **clock_sync:** the backend reports `time(unix)` (LoRaWAN `LORAWAN_TIME_UPDATED`, P2P Ack time tail). A pending clock_sync then answers with the Info. The "landed < 60 s ago" fast path is F1.
- **Announce:** Info → settings → first telemetry (decision from #452), with one retry pace of 5 s. A retry only re-checks queue room, so it does not send anything.

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
