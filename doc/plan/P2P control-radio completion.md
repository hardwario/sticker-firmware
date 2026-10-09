# P2P control-radio completion (node side)

**Base:** `hynek/lora-improvements-plan` @ `d62bb32` (PR #408 head, itself on `feat-p2p`).
**Branch:** `matej/p2p-control-radio`. **Target:** `hynek/lora-improvements-plan` while #408 is
open, re-target to `feat-p2p` when it merges.

> **Filename pending a PR number.** `doc/plan/README.md`'s convention is
> `<PR number> - <PR name>.md`; this file is to be renamed once the PR exists.

Completes the STICKER half of the P2P transport (#118) so a Proximos
`Control.radio.P2P` central owns the link end to end. The central
(`proximos-v2`, `control-radio`) and the NorthBridge (`proximos/firmware`,
`fiber-northbridge`) sides were already implemented; this is the node catching
up to bytes the central was already sending.

## 1. Why

At the base commit the node could pair, send acknowledged telemetry and answer
a downlink command, but five things were missing or wrong:

| Problem at `d62bb32` | Consequence |
|---|---|
| `recv_ack()` accepted only `0xFA` and `0x56` | The central sends `Detach` (`0xFD`) on `node-remove`; the node dropped it, kept retrying into a dead session, and after 8 failed cycles self-healed into an endless rejoin loop against an unregistered serial. `RejoinRequest` (`0xFE`) had no effect at all. |
| A dispatched command's deferred action was logged and discarded | `settings_save` and `reboot` over `0x56` did nothing: the central got its response and the node ignored the instruction. |
| The RX1 window guessed 255 B whenever a downlink was announced | 2434 ms of receiver-on at SF10 to collect a 2-byte command, because this driver has no hardware symbol timeout and a short window aborts an in-flight frame. |
| JoinAccept's `reserved(4)` was read as "unused" | No way for the central to set a node's TX power — the one radio parameter it can meaningfully own network-wide. |
| The duty governor was a token bucket | 1 % long-run average but **2.00 %** in the worst sliding hour (measured, see S5). Amortised compliance, not compliance. |

Plus a documentation layer that named three commands which do not exist.

## 2. Steps

One commit each. S8 is reserved but **not implemented** — see §4.

| Step | Commit | What |
|---|---|---|
| S0 | — | Base, build matrix, size baseline. No code. |
| S1 | `aba008e` | `doc(p2p)`: stale commands/defines; `app/Makefile` bench probe variables. |
| S2 | `d4cf2b9` | `feat(p2p)`: obey `Detach` (`0xFD`) and `RejoinRequest` (`0xFE`). |
| S3 | `c00e90f` | `feat(p2p)`: execute deferred command actions after the `0x55`. |
| S4 | `1399e14` | `feat(p2p)`: size the RX1 window from the Ack's `pending_frame_len`. |
| S5 | `0d30416` | `feat(p2p)`: exact sliding-hour duty ledger, replacing the bucket. |
| S6 | `4a0c209` | `feat(p2p)`: apply the JoinAccept TX-power assignment. |
| S7 | `7748b89` | `doc(p2p)`: the setup path — shell-only radio params, no `p2p_join`. |
| S8 | *(reserved)* | CAD / listen-before-talk. **Deferred, §4.** |
| S9 | `10640ca` | `test(p2p)`: pin the documented SF10 airtimes; coverage consolidation. |
| S10 | *(this)* | `doc(p2p)`: documentation and this plan document. |

### S1 — documentation and comment corrections
`ats radio join` does not exist; the rejoin command is the top-level `join`
(`app_radio_rejoin`) — corrected in `doc/p2p.md` §4/§7/§14 and
`doc/automated-test-playbook.md`. `factory_reset` reverts `radio_mode` to `OFF`
(#350), not `LORAWAN` — corrected in `app_p2p.c`, `doc/p2p.md` §7 and the
playbook. `dispatch_p2p_command`'s comment listed four P2P-reachable commands;
the generated dispatch guards leave ten ungated. `doc/p2p-e2e-test-plan.md` §1:
the bench build switch is `-DBENCH_RTT_BRIDGE=ON`, the RTT↔PTY transport is
`nb_rtt_bridge.py`, and the J-Link serials are one bench's rather than a fact.

`app/Makefile` gained optional `JLINK_SN` (→ `west flash --dev-id`) and
`RTT_ADDR`, because the P2P bench has two probes attached and `_SEGGER_RTT`
moves between images. Unset, every recipe expands to the command it always was.

### S2 — Detach and RejoinRequest
Both are empty-bodied, authenticated under `session_key` with direction RX and
the acknowledged uplink's counter (15 B on air). With no ciphertext the tag
covers the nonce and the 11 B header AAD alone, which is what makes them
unforgeable; the single-use counter and the closing RX1 window rule out replay.

`Detach` → `pairing_clear()`, factored out of `app_p2p_unjoin()` and compiled
unconditionally: delete `p2pjoin/state`, drop to `UNPAIRED`, purge the TX and
Ack-retry queues, clear `m_started` so `app_report`'s cadence skips the uplink.
**No automatic re-join** — the node was removed deliberately. `dev_nonce` is
never touched, so a later re-registration still authenticates.

`RejoinRequest` → `start_join_episode(true)`, the self-heal policy: exempt from
the 120 s boot window, backed off 60 s → ×2 → 1 h.

### S3 — deferred command actions
Mirrors `app_lrw.c::post_cmd_work_handler()`, constants and log strings
included so one bench anchor matches both transports. Where LoRaWAN watches two
"undelivered" signals, P2P needs four: the `0x55` can be still queued
(`m_tx_msgq`), duty-cycle-bounced (`m_tx_deferred_valid`), awaiting a
confirmation retry (`m_ack_retry_msgq`), or between a reschedule and its fire
(`m_tx_work`). Bounded at 6 × 8 s, then the action runs regardless — a
permanently failing TX must not postpone it forever.

Only the actions a `0x56` can reach are handled: `settings_save`, `reboot`,
`reset_counters`, `lrw_reset`, `lrw_join`. The last two are LoRaWAN-specific yet
carry no `transports:` guard; `lrw_reset` is honoured where the stack exists and
logged-and-ignored where it does not, `lrw_join` is refused in P2P mode.

`seq` correlation needed no node code — the generated `app_cmd_dispatch()`
already copies `cmd->seq` onto the response. The contract is documented in
`doc/p2p.md` §6 with the node-side idempotency that makes the central's
re-announce safe.

### S4 — exact RX-window sizing
Ack body becomes
`flags(1) | rssi(i8) | snr(i8) | [pending_frame_len if bit0] | [unix_be32 if bit1]`,
valid lengths 3/4/7/8. `pending_frame_len` is the pending `0x56`'s total on-air
length, so the window costs what the command needs: **468 ms instead of 2434 ms**
for a 2-byte GetInfo at SF10.

`p2p_parse_ack_body()` is keyed on the length, with the flags only refining it.
That asymmetry pre-dates the change (a time bit with no tail was already ignored
rather than trusted, which is what stops an over-claiming central walking the
parser off the end of the body) and it is what makes the byte adoptable without
a wire version: the legacy 3/7-byte body with bit 0 set still parses as
"pending, length unknown" and the caller keeps the 255 B fallback.

Note `recv_ack()` had a **second** length gate ahead of the decrypt that
accepted only 3 and 7; left alone it would have dropped every new body before
the parser saw it and the step would have been inert.

### S5 — exact sliding-hour duty ledger
Records `(end time, air time)` per transmission and admits a frame only when
the air already inside the trailing hour plus that frame fits
`P2P_DUTY_BUDGET_MS`. Every sliding hour therefore sums to ≤ 1 %, exactly
rather than on average, while keeping the bucket's latency behaviour.

Uptime is truncated to 32 bits and compared as an unsigned difference, correct
across the ~49.7-day wrap since an entry only lives one hour. A blocked call
returns the time until the oldest entry expires: a lower bound, since freeing
one entry may not free enough budget, but every caller already re-checks and
reschedules so it converges.

**Costs, both deliberate:**
- **384 B of RAM** (48 entries × 8 B, replacing the bucket's 16 B).
- **A bounded frame count per hour.** One entry per in-window transmission
  means the ledger, not the air budget, binds above ~48 uplinks/hour; at SF10
  the 36 000 ms allowance would otherwise buy ~109 minimum-size frames. Safe by
  construction — a full ring can only *delay* a frame — but a bench run wanting
  the air budget to be the visible limit needs `interval-report` above ~75 s.
  Folding the two oldest entries together on overflow would remove the limit at
  any SF; ~10 lines, not needed for the cadences this product ships with, and
  recorded here rather than done.

The ledger is RAM-only, so a reboot loop can still exceed 1 % — the same hole
the bucket had (it restarted full), accepted for the same reason: persisting it
would cost an NVS write per frame.

### S6 — JoinAccept `reserved(4)`
`channel_idx(1) | sf(1) | tx_power(1) | flags(1)`. `tx_power` (2..22 dBm,
0 = none) is applied to the session, persisted with the pairing so it survives
reboots, and shown by `ats radio status` as `assigned` versus `config`.
`channel_idx` must be 0 and `sf` is a documented hook — both logged and ignored,
because the modem's single receiver is exactly why SF is network-wide.

Every unsupported or out-of-range field is warned about and ignored rather than
refused: the JoinAccept is authenticated and otherwise valid, and declining to
pair over a byte this release cannot honour would strand the node. All-zero —
what the central sends until its `node_tx_power_dbm` key is set — means "no
assignment", so this is compatible with the central as it ships.

**Upgrade note:** the persisted pairing record grew one byte and
`join_settings_set()` accepts only the exact current length, so a node upgraded
across this change reads its old 23 B record as invalid, boots `UNPAIRED` and
re-joins once. Deliberate and harmless pre-deployment — that re-join is what
fetches the assignment. Recorded as a `doc/p2p.md` §7 lifecycle trigger since it
recurs for any future layout change.

### S7 — setup path
Documentation only. The three `p2p_*` radio parameters stay `writable: [shell]`
and the NFC `p2p_join` trigger stays unimplemented, both because the
alternatives are cross-project commitments rather than firmware toggles: over
the radio, changing frequency or SF severs the link carrying the command (and
the M-3 gate already denies it, #271); over NFC, the config schema is shared
with the Manager-App, and without a matching Hub change a phone that can set
these only creates a way to strand a node.

## 3. Verification

Run after **every** step, not just at the end:

```sh
cd app
west build -p always -b sticker                                                  # Release
west build -p always -b sticker -- -DEXTRA_CONF_FILE=debug.conf                  # debug, no P2P
west build -p always -b sticker -- -DEXTRA_CONF_FILE="debug.conf;debug_p2p_bench.conf"
cd .. && bash tests/run_native.sh                                                 # 12 suites
git ls-files 'app/src/*.c' 'app/src/*.h' | xargs clang-format --dry-run --Werror
(cd app/decoder && node --test)
pytest scripts/west_commands/tests
```

The bench configuration is not in CI, so it must be built locally each step —
it is the only debug image containing P2P, and the one where the RAM cost of a
change shows up soonest.

### Sizes

Regions: Release FLASH 208 KB / RAM 64 KB; debug and bench FLASH 240 KB / RAM 64 KB.

| Step | Release FLASH | Release RAM | debug FLASH | debug RAM | bench FLASH | bench RAM |
|---|---|---|---|---|---|---|
| S0 base | 162 064 | 56 264 | 216 676 | 61 532 | 186 268 | 50 744 |
| S1 | 162 064 | 56 264 | 216 676 | 61 532 | 186 268 | 50 744 |
| S2 | 162 232 | 56 264 | 216 676 | 61 532 | 186 804 | 50 744 |
| S3 | 162 432 | 56 264 | 216 676 | 61 532 | 187 268 | 50 808 |
| S4 | 162 536 | 56 264 | 216 676 | 61 532 | 187 396 | 50 808 |
| S5 | 162 552 | **56 648** | 216 676 | 61 532 | 187 412 | **51 192** |
| S6 | 162 688 | 56 648 | 216 676 | 61 532 | 188 084 | 51 192 |
| S7/S9/S10 | 162 688 | 56 648 | 216 676 | 61 532 | 188 084 | 51 192 |

**Total: Release +624 B flash / +384 B RAM; bench +1 816 B flash / +448 B RAM;
`debug` unchanged** (it contains no P2P, which is also the check that no change
leaked outside `CONFIG_RADIO_P2P`). All the RAM growth is S5's ledger. Headroom
left: Release 50 304 B flash / 8 888 B RAM; bench 57 676 B flash / 14 344 B RAM.

S1 was verified byte-identical to S0 by sha256 on the bench hex, since it
touches only comments outside the build.

### Tests

`p2p_logic` 23 → **35**: −6 token-bucket, +6 ledger, +6 `reserved(4)` parser,
+3 Ack-body shapes, +1 empty-body codec, +1 frame-type constants, +1 airtime
pin, and `test_ack_body_bad_length_rejected` / `test_ack_body_with_time`
reworked in place. All 12 native suites green; decoder 81/81 (+2); configen
28/28.

Two of these earn their keep beyond regression cover:

- **`test_duty_sliding_hour_never_exceeds_1pct`** simulates 24 h and checks the
  window ending at *every* transmission. It was run against a standalone
  reimplementation of the token bucket it replaces, which it **rejects at
  71 860 ms of air in the worst sliding hour — 2.00 % duty**. That independently
  confirms the "~2 %" figure PR #408 documented, and confirms the test
  discriminates rather than restating the implementation.
- **`test_ack_body_with_time`** caught its own obsolescence: it sized its buffer
  with `P2P_ACK_BODY_MAX_LEN`, so when S4 grew that 7 → 8 the test silently
  became an 8-byte case with no pending bit, and failed. Any test naming a
  constant whose *meaning* changes needs re-reading, not just re-running.

### Bench

`doc/p2p-e2e-test-plan.md` §3.1 adds P2E-12…P2E-22. One cadence caveat:
**P2E-16 must run at `config interval-report 120`** (30 frames/hour), not 60 —
above ~48 uplinks/hour the ledger's slot count binds before the air-time budget
and the duty blocks observed would be slot exhaustion rather than the 1 % limit.

## 4. S8 — CAD / listen-before-talk: follow-up PR (fork + app)

**Reserved, not implemented here.** The step number is kept so this plan and
both bench guides stay consistent; nothing is renumbered.

**Why it cannot land in this repo alone.** The Zephyr LoRa driver API has no CAD
entry point: `struct lora_driver_api` (`include/zephyr/drivers/lora.h`) is
`config`/`send`/`send_async`/`recv`/`recv_async`/`test_cw`, and
`drivers/lora/loramac_node/sx12xx_common.c` registers only `TxDone`/`RxDone`
(plus the error events) in its `RadioEvents_t`. There is nothing for app code to
call.

**What it needs.** `loramac-node` already exposes everything required, so that
module needs no change: `RadioStartCad()` (`src/radio/sx126x/radio.c`),
`RadioEvents->CadDone(bool)` (same file, dispatched from the IRQ handler) and
`SX126xSetCadParams()` (`src/radio/sx126x/sx126x.c`). The work is therefore:

1. **One additive `cad` op in the `hardwario/sticker-zephyr` fork** on
   `v4.3.0-sticker1-branch`:
   - `include/zephyr/drivers/lora.h` — `int (*cad)(const struct device *dev,
     k_timeout_t timeout, bool *busy)` in `struct lora_driver_api`, plus a
     matching `lora_cad()` inline wrapper.
   - `drivers/lora/loramac_node/sx12xx_common.c` — register `events.CadDone`
     alongside the existing `TxDone`/`RxDone`/`RxError`/`TxTimeout` (line ~387)
     and implement `sx12xx_lora_cad()` as acquire-modem →
     `SX126xSetCadParams(LORA_CAD_02_SYMBOL, detPeak, detMin, LORA_CAD_ONLY, 0)`
     → `Radio.StartCad()` → wait on the `dev_data.operation_done`
     `k_poll_signal` exactly as `sx12xx_lora_recv()` does (same file, ~line 259)
     → release.
   - `drivers/lora/loramac_node/sx126x.c` — add `.cad = sx12xx_lora_cad` to the
     `lora_driver_api` instance (~line 470). Note the api struct is **per-chip**,
     not in `sx12xx_common.c`; `sx127x.c` has its own and can be left unset
     (a NULL op is the "unsupported" signal, so `lora_cad()` should check it).

   **Shared with PR #410 Step 1** (TOWER GFSK needs the same additive-API
   precedent), so it should be opened once, with Hynek, rather than twice.
2. **A small app step**: call `lora_cad()` before `lora_send()` in
   `tx_frame_at()` and `send_join_request()`; on busy return `-EAGAIN` with a
   random 100–500 ms backoff, capped at 3 attempts, then transmit anyway.
   `CONFIG_APP_P2P_CAD` under `RADIO_P2P`. `tests/p2p_logic/src/emul_lora.c`
   needs a `.cad` op returning `busy = false` so the suite still links —
   untouched in this PR, since no `cad` op exists to stub.
3. **A prerequisite fix** — see §5.1: `ack_retry_work_handler()` would lose a
   frame on the `-EAGAIN` that CAD introduces.

**Why deferring is safe.** CAD is collision avoidance, not a regulatory
requirement in EU868 — the 1 % duty limit is what regulation demands, and S5
enforces it strictly. P2P works without CAD; a collision costs a retry, which
the confirmed-uplink path already handles. Bench row **P2E-18 is
BLOCKED(S8 not implemented in this iteration)**, and it was already in the
acceptance matrix's "BLOCKED allowed" set, so no Trello item depends on it.

## 5. Recorded, not fixed

Two pre-existing defects that no step in this PR owns. Both are deliberately
left alone to keep each commit's scope honest.

### 5.1 `ack_retry_work_handler()` drops a dequeued retry on a send error
It dequeues the entry from `m_ack_retry_msgq` **before** calling
`tx_frame_at()`, and on error only logs `Ack retry (counter %u) send failed`.
The frame awaiting confirmation is then gone.

Latent today: at present `tx_frame_at()` can only fail on hard errors
(`-EBUSY` in listen mode, `-EMSGSIZE`, `-ENOTCONN`, or a `lora_send()` failure),
where dropping is defensible. It becomes a real bug the moment S8 introduces a
soft, expected `-EAGAIN` from CAD. **Fix with S8:** re-queue on `-EAGAIN`, or
CAD-check before the dequeue alongside the existing duty check.

### 5.2 `-Wcomment` warning in `app_p2p_start()`
Its comment contains `p2pjoin/*`, and the `/*` inside a block comment makes gcc
emit `"/*" within comment` on every native build. Pre-existing at `d62bb32`
(then line 1864). A one-word fix, but it belongs to no step here.

## 6. Checklist

- [x] **S0** base + build matrix + size baseline
- [x] **S1** doc/comment corrections, `app/Makefile` probe variables
- [x] **S2** Detach (`0xFD`) and RejoinRequest (`0xFE`)
- [x] **S3** deferred command actions + the `seq` contract
- [x] **S4** Ack `pending_frame_len`, exact RX-window sizing
- [x] **S5** exact sliding-hour duty ledger
- [x] **S6** JoinAccept `reserved(4)` TX-power assignment
- [x] **S7** setup path documented (shell-only params, no `p2p_join`)
- [ ] **S8** CAD / listen-before-talk — **deferred to a follow-up PR** (§4)
- [x] **S9** native coverage consolidation (35 tests)
- [x] **S10** documentation + this plan document
