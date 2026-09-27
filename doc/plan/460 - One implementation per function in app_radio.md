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

A small const struct, selected once at `app_radio_init()` from `radio-mode`:

```c
struct app_radio_backend {
	int (*send)(const struct app_radio_frame *f, uint8_t attempt,
		    struct app_radio_tx_result *res);    /* blocking, radio WQ */
	size_t (*budget)(void);                          /* current max payload */
	uint32_t (*min_gap_ms)(void);                    /* LRW 3 s, P2P 1 s */
	void (*request_check)(void);                     /* LinkCheckReq / FCtrl CONFIRMED */
	void (*warning_step)(void);                      /* #424 ladder / +2 dB */
	int (*rejoin)(bool slow);                        /* -ENOTSUP for ABP */
	uint32_t (*airtime_ms)(size_t len);              /* T2d */
	/* existing: init, start, suspend, get_state, reset_link, ... */
};
```

`send()` returns one result set. The common layer handles each result the same way whichever radio is running:

| Result | Meaning | Common reaction |
|---|---|---|
| `0` | transmitted; `res->acked` for a confirmed frame | dequeue; ACK ladder when confirmed and not acked |
| `-EAGAIN` + `res->wait_ms` | duty-blocked or the medium is busy with its own traffic (the LoRaWAN MAC-flush uplink) | keep the frame and retry at `wait_ms`; a MAC flush does not count as a retry |
| `-EBUSY` | a confirmed frame is in flight | park; kicked when it ends |
| `-ENOTCONN` | not joined / not paired | park, don't count; kicked on the `joined` event |
| `-EMSGSIZE` | over the current budget | over-budget recovery by kind (§2.3) |
| other | I/O error | retry after 15 s, counted |

A backend reports upward through events: `joined`, `join_failed`, `link_result(ok)`, `downlink(bytes, rssi, snr)` and `time(unix)`. They are all called on the radio work queue.

### 2.2 Frames and queues (F4 = T2b)

```c
enum app_radio_kind { ANSWER, ALARM, TELEMETRY, HISTORY };
struct app_radio_frame { uint8_t kind, tag, port; bool confirmed; uint16_t len; uint8_t buf[64]; };
```

- `tag` is OTHER / INFO / SETTINGS, and it drives the announce recovery. `port` is the LoRaWAN fPort. P2P maps each kind to its frame type.
- **Queues:** answers 4, alarms 4 (decision 10: "4+4"). Answers go before alarms.
- **Telemetry** stays a coalescing flag (#340 M5), composed at send time with the backend budget.
- **History** is the replay state machine (§2.5). It owns the radio while it runs, but it never holds answers or alarms.
- **One scheduler work item** decides the order: answer > alarm > history > telemetry. The telemetry gate is kept while a replay or an announce is open.
- **Refused-frame rule:** at most 8 counted retries. After that the frame is abandoned. An abandoned telemetry frame always calls `app_compose_reset()`. An abandoned answer or alarm is logged and counted in `TX_ERR`.
  - This fixes the P2P drop on a hard error, and P2P's missing `app_compose_reset()`.
- **Parking:** frames wait while the radio is not connected. They are not dropped, which fixes the P2P drop while unpaired.

P2P loses `m_tx_msgq`, `m_tx_deferred` and the telemetry wait-for-queue logic. LoRaWAN loses `m_response_msgq`, `m_alarm_msgq`, `send_work_handler` and `tx_send_queued`. The downlink queue stays in the LoRaWAN backend, because P2P receives inside `send()`.

### 2.3 Over-budget recovery (common)

`-EMSGSIZE` is handled by kind:

- **Info / settings-info:** re-arm the announce for the next budget change.
- **Answer:** replace it with `Error BUDGET_TOO_SMALL` for the same seq (`app_cmd`).
- **Alarm:** drop it and log. The alarm engine re-fires while the condition holds.
- **History:** finish the replay with `BUDGET_TOO_SMALL`.

On P2P the budget is fixed, so these paths are dead but harmless.

### 2.4 Link supervision (F2 = T3)

- **State:** HEALTHY / WARNING / RECONNECT live in `app_radio`. JOINING / IDLE / DISABLED still come from the backend.
- **Cadence:** `app_radio_link_check_due()`, common since F1. When a check is due, the scheduler calls `request_check()` before the telemetry send.
- **Outcome:**
  - The backend reports `link_result(ok)`. It keeps its own answer timeout: LoRaWAN 10 s, P2P the RX1 window.
  - Any authenticated downlink counts as a success, but only once per uplink cycle. This fixes the #458 double count of a LinkCheckAns.
- **Thresholds:** 3 failures → WARNING, 1 success → HEALTHY.
- **In WARNING:**
  - every further failure → `warning_step()`;
  - after `radio-link-check-fail-rejoin` failures → `rejoin(slow = true)` → RECONNECT;
  - reconnect backoff 60 s ×2 up to 1 h, ±25 % jitter, on both radios;
  - ABP (`rejoin` returns `-ENOTSUP`) stays in WARNING.
- **M-2:** the stale-uplink heartbeat and the F29 duty-cycle hold move out of both backends. `app_radio_stale_check()` / `_note()` are already common.

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
    - LoRaWAN-like: DR-driven budget, 3 s gap, unconfirmed by default, ABP option.
    - P2P-like: 64 B, 1 s gap, confirmed via ack.
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
