# PR #421 — LoRaWAN glue fixes in sticker-zephyr (L-7, #181, #241)

Fork branch: `hardwario/sticker-zephyr` **`v4.3.0-sticker2-branch`** (= `v4.3.0-sticker1-branch` + 3 commits).
App branch: `hynek/lorawan-glue-fixes` → `v1.5.0`. The migration to Zephyr v4.4.2 is tracked separately in #420;
the glue there is identical, so these commits cherry-pick onto it.

## 1. Problems

| ID | Where | What goes wrong |
|---|---|---|
| **L-7** (#241) | `subsys/lorawan/lorawan.c` `mlme_confirm_handler()` | Every MLME confirm (join, link check, device time, and every failure) set `last_mlme_confirm_status` and gave `mlme_confirm_sem` (max 1), but only `lorawan_join()` takes it. STICKER sends a DeviceTimeReq after each join and a LinkCheckReq on every 5th uplink, so the next (re)join nearly always starts with a stale token and returns right after TX with the **previous** result: a stale OK means "success" before RX1; a stale `JOIN_FAIL` means `-EINVAL` before RX1 while the MAC actually joins. |
| **#181** | `lorawan_send()` / `lorawan_join()` | `k_sem_take(..., K_FOREVER)`. A lost confirm wedges `m_work_q` forever. Only the #182 liveness watchdog recovered it, by resetting the SoC. |
| **#241** | `lorawan.c` + `drivers/lora/loramac_node/{hal_common,sx126x}.c` + `app_lrw.c` | LoRaMac (not thread-safe) is entered from app threads (API + direct MIB / `LoRaMacIsBusy` calls) **and** from the system WQ (timer glue, SX126x DIO1 bottom half, `LoRaMacProcess()`), with no shared lock. |

HW evidence for L-7, 2026-09-22 (EU868, ChirpStack v4 on the ProXimos Hub): DevNonce 6 → `Join failed: -22` about
1 s after TX, then `ats lrw status` showed DevAddr `017e92a8` while app_lrw was in `RECONNECT`. The session was
discarded and a third join (DevNonce 7) was needed.

## 2. Design

### 2.1 L-7 — only the join waiter is signalled

- `mlme_confirm_handler()` records the status and gives the sem **only for `MLME_JOIN`**.
- `lorawan_join()` does `k_sem_reset(&mlme_confirm_sem)` before issuing the join, under the MAC lock. This catches a late
  confirm left by a join whose wait timed out.

### 2.2 #181 — bounded confirm waits

- `CONFIG_LORAWAN_CONFIRM_TIMEOUT_MS` (fork default 20000, `0` = `K_FOREVER`, the upstream behaviour).
- `lorawan_join()` and `lorawan_send()` return `-ETIMEDOUT` when the confirm does not arrive in time.
- `lorawan_send()` drains `mcps_confirm_sem` before each request, so a late confirm can't satisfy the next send.
- App: `CONFIG_LORAWAN_CONFIRM_TIMEOUT_MS=20000`. A `BUILD_ASSERT` keeps it below `LRW_HEARTBEAT_TIMEOUT_MS` (30 s),
  so the graceful return always beats the #182 liveness reset. #182 stays as the backstop for other wedges.
- Legitimate waits: class A unconfirmed uplink ≈ ToA + RX1 (1 s) + RX2 (2 s) + RX2 ToA, at most ≈ 8–9 s at SF12;
  join ≈ ToA + 5/6 s + RX2 ≈ 9 s.
- All app call sites already treat any error as a bounded retry. Trade-off: a timed-out send may have been on air, so a retry can
  duplicate an uplink.

### 2.3 #241 — one recursive MAC lock

- `lorawan_mac_lock()/unlock()` (declared in `include/zephyr/lorawan/lorawan.h`) is a recursive `k_mutex`, **defined in
  `hal_common.c`**, so every build that uses the loramac-node HAL has it, including radio-only builds without LoRaWAN.
- Taken in:
  - `timer_work_handler()` (all LoRaMac timer events) and the SX126x DIO1 work handler (radio events). Together these cover
    all `LoRaMacProcess()` invocations, which run synchronously inside them via `MacProcessNotify`.
  - `mac_process_notify()`, `datarate_observe()`, and every LoRaMac call in the LoRaWAN API (join, send, link check,
    device time, MIB get/set, `LoRaMacQueryTxPossible`, `lorawan_start()`, `SysTimeGet`).
  - `app_lrw.c`: join polling (`LoRaMacIsBusy`, `MIB_NETWORK_ACTIVATION`), `LoRaMacDeInitialization` on rejoin, ABP RX
    delays, private sync word, and `app_lrw_get_info()` (DevAddr + `FCntUp`, called from shell / NFC / `m_work_q`).

### 2.4 Deadlock / latency analysis

- **Never held across a confirm wait.** The confirm is delivered by `LoRaMacProcess()` on the system WQ, which needs
  the lock. `lorawan_send()` and `lorawan_join()` release it right after the request, then block on the sem.
- **Recursion.** LoRaMac callbacks (`mcps_confirm_handler` → `datarate_observe`, the app's DR-changed callback →
  `lorawan_get_payload_sizes`, the NVM data-change callback) re-enter the MAC on the same thread. A `k_mutex` handles
  that.
- **Lock order** is always `lorawan_{join,send}_mutex` → MAC lock. The system WQ handlers take only the MAC lock. No app
  callback called under the MAC lock calls `lorawan_send()`/`lorawan_join()` (the Info build is already deferred off the
  callback stack, #219).
- **No IRQ-context use.** The k_timer and DIO1 ISRs only submit work, and `MacProcessNotify` is never called inside
  LoRaMac's `CRITICAL_SECTION` (checked in `LoRaMac.c`).
- **Priority.** The system WQ is cooperative and high-priority; `m_work_q` is the lowest app priority. The `k_mutex`
  priority inheritance boosts a low-priority holder.
- **RX timing.** App-held sections are tiny MIB accesses (µs), except the rejoin `LoRaMacDeInitialization` /
  `lorawan_start`, which run while no RX window is pending. LoRaMac already compensates the RX-window timer setup for
  processing delay (`TimerGetCurrentTime() - TxDoneParams.CurTime`).
- **Not covered:** the raw LoRa API (`sx12xx_common.c`, P2P builds) and the LoRaWAN services (not built for STICKER).

## 3. Test matrix

Temporary HIL hooks (uncommitted patches: `lorawan_test_drop_confirm` in `lorawan.c`,
`ats lrw dropconf mcps|join` + `ats lrw mibstress <s>` in `app_ats.c`) are used for T5/T6 only.

| # | Scenario | Pass criteria |
|---|---|---|
| T1 | Boot → OTAA join → boot Info + settings-info ConfigDump + telemetry | First-attempt join; frames decode; no errors |
| T2 | L-7: `join` right after link-check / device-time confirms; genuine `JOIN_FAIL` (NS ignores the join) then a successful join | Join result arrives only after the RX windows (timing); no early `-22`; no discarded session |
| T3 | Steady state: telemetry, link checks, ADR, DeviceTime, MAC-only downlinks | No wedge, no WDT reset, heartbeat fed |
| T4 | GetConfig, SetParam `interval_report=600` (staged), `settings_save` → reboot → boot ConfigDump shows 600 | Values round-trip |
| T5 | #181: drop one McpsConfirm (`dropconf mcps` + `send`); drop one MLME join confirm (`dropconf join` + `join`) | `-ETIMEDOUT` after ≈ 20 s, `m_work_q` recovers, next uplink OK, **no WDT reset** |
| T6 | #241: `mibstress 60` during uplinks + queued downlinks | No deadlock/hang, all downlinks answered, no WDT reset |
| T7 | Rejoin after network loss (NS temporarily ignores the device) | Link checks fail → RECONNECT → first rejoin after recovery succeeds (no false `-22`) |
| T8 | Release image (PM on, no shell): join + telemetry + downlink | Same as T1/T3 via NS |

## 4. Results

- Builds (vs `v1.5.0` `0dd8cb4`): release 73.46 % / 80.19 %, debug 87.88 % / 93.94 %, debug+W1 96.34 % / 94.72 %.
- Host: `tests/run_native.sh` all suites PASS, `ttn.test.js` 70/70, configen/proto pytest 28/28, `clang-format` 22.1.5 clean.
- Extra fix found during the HIL: `app_lrw_get_info()` read LoRaMac's crypto context in the IDLE boot
  window before `lorawan_start()` (shell showed FCntUp `0x080232D6`). It is now guarded by `m_mac_started` (`545b679`).
- CI (PR #421 @ `545b679`): all green. CI fetches `v4.3.0-sticker2-branch` via `west.yml`.

### HIL (2026-09-23, STICKER on J-Link 822005110, DevEUI 5876070000000413, EU868, ChirpStack v4 + RAK5146 on the ProXimos Hub; the NS side was driven by the "Hub controller" session)

| # | Result | Details |
|---|---|---|
| T1 | PASS | Debug `build-glue-debug`: first-attempt join (DevNonce 17), Info (fw 1.5.0), ConfigDump, telemetry, MAC downlinks. |
| T2 | PASS | NS disabled → `join` failed only after RX2 (MLME "Rx 2 timeout" 8.4 s after the command) → enabled → the automatic rejoin (DevNonce 19) succeeded on its first attempt. |
| T2b | PASS, decisive | Temporary WRN timing log around `lorawan_join()`, run after the join's DeviceTime + LinkCheck confirms: **old v1.5.0 = `ret=0 after 26 ms` / `25 ms`** (stale token) vs **#421 = `ret=0 after 8305 ms`** (the real JoinAccept). |
| T3 | PASS | 8 min at interval 60 s with LinkCheckReq on every uplink: 8/8 LinkCheckAns, FCnt 4→11 and FCntDown 3→10 with no gaps, no WRN/ERR. |
| T4 | PASS | GetConfig page 0/6 correct. Raw SetParam without save → Ack, staged 600, no reboot. `settings_save` → reboot → boot ConfigDump 600. CLI `set-config` (save=true) → reboot → 900. |
| T5 | PASS | Temporary hooks. Dropped McpsConfirm → `McpsConfirm timeout` → `lorawan_send` -116 exactly 20 s after the request, and the retry was sent (duplicate uplink as expected). Dropped join confirm → `lorawan_join() ret=-116 after 20025 ms` → MAC polling → HEALTHY on that join. No WDT reset. |
| T6 | PASS | 3 × `mibstress 25` (22.5–22.7 k locked `get_info` calls each, ~900/s) concurrent with 4 chained raw GetConfig page downlinks. All answered with the correct seq/page, no hang, no reset. |
| T7 | PASS | NS disabled 8.8 min: LC failures → WARNING → RECONNECT (23:34:22Z) → rejoin 1 (DevNonce 30) refused → enabled → rejoin 2 (DevNonce 31), the first after re-enabling, succeeded. |
| T8 | PASS | Release (`PM=y`, no shell): first-attempt join, Info, ConfigDump with `w1_slot_type` 4× empty, telemetry + LinkCheckAns, GetConfig page 0, CLI `set-config` 900 with save → reboot → boot ConfigDump 900. |

Side findings (not caused by #421):
- The Hub CLI can't select a GetConfig page, and control-radio's `PageReassembler` is per-device, not per-seq
  (proximos-v2 #91).
- The J-Link re-enumerated on USB several times during the session; a failed `west flash` just needs a retry.
- **Always use `west flash --skip-rebuild`** for images built from temporarily patched trees: `west flash` otherwise
  rebuilds the stale build dir and silently drops the temporary hooks.

The unit was left on the final #421 debug image with its original config (60 / 900 s, LC interval 5).
