# P2P end-to-end test plan — STICKER firmware vs. Proximos central

Acceptance test for the P2P downlink protocol (PR #408 items B1–B5, B2, B3) against the
Proximos `control-radio` central runtime (proximos-v2 MR!30 items S1–S4). The topology is the
**real STICKER firmware ↔ northbridge modem ↔ Proximos central**, not two STICKERs — the
network side is Proximos, exercising both halves of the protocol at once.

This mirrors `doc/us915-test-plan.md` in spirit: a per-item scenario table with the exact
observable on each side, so an HIL run is a checklist rather than a debugging session.

## 1. Rig

The two-probe P2P rig from the PR #404 app-key/CMAC HIL (see the commit history) is the same
rig here, plus the Proximos central driving the northbridge.

**The J-Link serial numbers below are one specific bench's** (the PR #404/#408 rig) and are
recorded only so that run's logs stay readable. Re-read your own before every session —
`lsusb -d 1366: -v 2>/dev/null | grep iSerial` (values print zero-padded) or `ShowEmuList`
inside `JLinkExe` — and substitute them everywhere a serial appears.

| Role | Hardware | Firmware |
|---|---|---|
| STICKER DUT | J-Link Compact Base **822005109** (or EDU Mini 801053709's STICKER) | this branch (`feat-p2p` + B1–B5), a **debug + P2P bench** build (`debug.conf` + `debug_p2p_bench.conf`) so `ats radio …` shell + RTT log are available |
| Northbridge modem | J-Link Compact Base **822005110** (STM32WL5MOC) | `proximos/firmware@hynek/northbridge-p2p-protocol`, `fiber-northbridge/app` — the HDLC P2P modem. Bench uses the **RTT-bridge** transport variant, built with `-DBENCH_RTT_BRIDGE=ON` (`fiber-northbridge/app/CMakeLists.txt`; `APP_P2P_BENCH_RTT_BRIDGE` is the C define it sets, not the build switch), since no USB-UART adapter is attached |
| Central | this machine | Proximos `control-radio-p2p-host` (MR!30, S1–S4), talking to the northbridge over the RTT-bridge |

**Transport note:** there is no USB-UART adapter enumerated (`/dev/ttyUSB*`/`ttyACM*` absent),
so the Proximos↔northbridge link is an RTT↔PTY bridge. The current implementation is
`proximos/firmware` `fiber-northbridge/tests/hil_p2p/nb_rtt_bridge.py`, which speaks RTT
directly over `pylink` and hands the host a PTY (`--nb-sn`, `--elf`|`--rtt-addr`, `--link`);
see §6's bench findings for why the PTY must be raw and the RTT down-buffer 256 B. The earlier
`JLinkExe -RTTTelnetPort` + `socat` pairing described in the PR #404 HIL is superseded by that
script — it lacked the raw-PTY and chunked-write handling the JoinAccept timing needs. The HDLC
framing and protocol are byte-identical between the RTT bridge and the production USART1 build —
only the transport differs — so either validates the real protocol.

**Bench hygiene:** J-Link ownership changes daily — confirm no peer session is driving
either probe before attaching, and always name the probe explicitly: `JLinkExe -USB <SN>`,
`west flash --dev-id <SN>` (or `make flash JLINK_SN=<SN>`), `rttt --serial <SN>`. Flashing
the DUT needs explicit per-image OK. `ats radio unjoin` (reboot) gives a clean never-paired
start; never reuse a stale pairing (always fresh join per bench convention).

## 2. Preconditions

- STICKER DUT: `lrw_appkey` provisioned (non-zero) and identical to the app_key the central
  has registered for this serial — the P2P session key derives from it (doc/p2p.md §4). A
  zero app_key makes the radio refuse to start.
- `radio-mode p2p` set + saved on the DUT; reboot into P2P.
- Central: started against the northbridge, with the DUT's serial→app_key registered
  (`node-add`). Time delivery **on** for the S3 step (`control-radio-p2p-host --deliver-time`,
  MR!30 commit `dff883c`). Full central commands in §7.
- Both sides logging: DUT over RTT (Terminal 0), central `eprintln!` to **stderr** (`2>`).

## 3. Scenario table

Each row: the action, the STICKER-side observable, and the Proximos-side observable. IDs
prefix `P2E-` (P2P end-to-end).

| ID | Item | Action | STICKER observable | Proximos observable |
|---|---|---|---|---|
| P2E-01 | join | boot DUT into P2P (fresh) | JoinRequest sent; on accept → PAIRED, `ats radio status` shows net_id/dev_addr/rx1_delay | JoinRequest verified (CMAC over app_key), dev_addr allocated, JoinAccept scheduled into RX1, session persisted with `proto_version` |
| P2E-02 | data plane | let a telemetry uplink go (or `ats radio compose`→send) | TELEMETRY (0x02) TX; Ack received, log `Ack (counter N) rssi=… snr=…` | data frame decrypt+dedup, ACK emitted echoing `counter`, RSSI/SNR = its EVT_RX measurement |
| P2E-03 | **B1** | inspect the Ack | `ats radio status`: `last ack rssi` / `last ack snr` are real values, not `n/a` | ACK body = `flags\|rssi(i8)\|snr(i8)` (3 B) |
| P2E-04 | **B5** | enable time delivery on the central; next uplink | DUT RTC set — `clock get` returns wall-clock after the Ack; log `Ack … [time]` | ACK body = 7 B, `flags` bit1 set, 4-byte BE Unix tail |
| P2E-05 | **B4** happy path | `node-send --radio p2p --hex <cmd>` on the central (e.g. a GetInfo Command) | after the announcing Ack shows `[pending]`, the NEXT uplink's window yields a `0x56`; log `Command received (counter N, k B)`; a `0x55` RESPONSE is queued and sent | announce (pending flag) → on the next new uplink deliver `0x56` into RX1 → receive the `0x55`, pop the queue head → phase Idle |
| P2E-06 | **B4** chaining | queue two downlinks | two consecutive command→response cycles; pending stays set until the queue drains | queue depth 2 → 1 → 0 across the cycles |
| P2E-07 | **B4** gating | `node-send` a `set_param` writing `region`/`radio_mode`/a key | RESPONSE carries `NOT_WRITABLE` (Error) — the field is not writable over P2P | central just relays; the refusal is the DUT's |
| P2E-08 | **B4** command gating | `node-send` an LRW-only command (e.g. `req_history`) | RESPONSE `NOT_READY`/"transport not allowed" | — |
| P2E-09 | **B2** duty | queue an alarm right behind a telemetry frame | the alarm goes out promptly, not after a ~227 s block; sustained sends throttle to ~1 % | frames arrive at the expected cadence | *(latency half still valid; the ~1 % half is **superseded by P2E-16**, which measures the sliding-hour ledger that replaced the token bucket)* |
| P2E-10 | **B3** self-heal | `ats radio ack_drop 32` on a **freshly booted** DUT, armed within seconds of boot (each failed cycle burns 4 Acks — the uplink and its 3 retries — so 32 forces exactly 8 fully-failed cycles; 24 stops at 6 and the next real Ack resets the streak. The storm needs ~21 s of air, so the duty ledger must be near-empty or it blocks the run partway) | after 8 give-ups → `self-healing re-join (§7)`, JoinRequest with exponential backoff | central sees a re-join of a known device outside the pairing window |
| P2E-11 | persistence | reboot the DUT | resumes PAIRED from NVS (no JoinRequest), counter resumes at the reserved high-water | next uplink decrypts under the same session key, counter ≥ reservation |

### 3.1 Added for the control-radio completion PR

These cover what the node gained after PR #408: Detach/RejoinRequest, deferred
command actions, exact RX-window sizing, the duty ledger and the TX-power
assignment. Full prerequisites, exact anchor strings and failure modes are in the
companion bench guide; this table is the checklist form.

`(F#)` marks the central-side step a row depends on — a row cannot pass against a
central that predates it.

| ID | Item | Action | STICKER observable | Proximos observable |
|---|---|---|---|---|
| P2E-12 | Detach obeyed | `node-remove --radio p2p --serial <s>` | on the next uplink `Detach received (counter N): pairing cleared, radio idle until reboot or \`join\``; `ats radio status` → `state: UNPAIRED`; no further `TX type` lines and **no** `self-healing re-join` within 10 min | `detach pending for … — sending Detach(0xFD) and dropping the session`, `TX_SCHEDULE ok: Detach dev_addr=…`, then silence from that dev_addr |
| P2E-13 | RejoinRequest obeyed *(F7)* | make the session `RejoinPending` (re-register the serial, or the rekey-threshold hook) | `RejoinRequest received (counter N): re-joining` → `JoinRequest sent (dev_nonce N+1 …)` within 60 s + jitter → `Joined: …` | `TX_SCHEDULE ok: RejoinRequest …` then `JoinAccept -> serial=…` for the same serial |
| P2E-14 | Deferred action after the 0x55 | `node-send` a `SetParam{application.interval_report=120, save=true}` | `Command received …` → `TX type 85 …` → `Ack (counter …)` → `Post-command action 1 scheduled in 8s` → ≥8 s later `Command: saving settings + reboot` → reboot banner → `state: PAIRED`, `config interval-report` reads 120 | `response decrypted+authenticated`, `0x55 RESPONSE cleared …`; telemetry resumes after the reboot with no re-join |
| P2E-15 | Exact RX window *(F4)* | queue a 2 B GetInfo (`2200`) | announcing `Ack (counter …) [pending] pending_len=17`; next cycle `Command received (counter …, 2 B)`; the window is ~468 ms, never the ~2434 ms it used to be | `TX_SCHEDULE ok: ACK … flags=0x01 (4 B body)` then `COMMAND(0x56) … (2 B body)` |
| P2E-16 | Sliding-hour duty ledger | run ≥ 70 min with a burst at t≈0. **Use `config interval-report 120`** — see the note below | `TX duty-cycle blocked for %lld ms` appears only when the trailing hour's air would exceed 36 000 ms; no hour in the log sums above it (script the `%u ms air` values) | frames arrive with the predicted gaps |
| P2E-17 | TX-power assignment *(F5)* | Hub `node_tx_power_dbm: 8`, then re-join | `Joined: …` then `Session TX power assigned: 8 dBm (config 14 dBm)`; `ats radio status` → `tx power: 8 dBm (assigned)`, and it survives a reboot | `JoinAccept -> … tx_power=8`; subsequent `EVT_RX … rssi=` lower at fixed geometry |
| P2E-18 | CAD / listen-before-talk | — | **BLOCKED — not implemented in this PR.** The Zephyr LoRa driver API has no CAD entry point; see the plan doc's S8 section | — |
| P2E-19 | Persistence cleared | `settings erase`, re-provision | `JoinRequest sent (dev_nonce 0 …)` then `JoinAccept not received/invalid …` until the boot window expires | `JoinRequest dev_nonce 0 not accepted for … (replay or implausible jump) — dropping`. Documents the lockout; `node-remove` + `node-add` clears it |
| P2E-20 | Reboot between announce and delivery | queue a 40 B command, wait for `Ack … [pending]`, power-cycle before the next uplink | after boot the window is the 23 B Ack size (the announcement was RAM-only), so a 55 B `0x56` is missed once | *(F3)* three uplinks later `re-announcing downlink seq=…`, then a fresh `Ack … [pending] pending_len=55` and the command lands |
| P2E-21 | Wrong key | central registered with a different `app_key`; `ats radio unjoin` | `JoinRequest sent …` repeating, never `Joined`; window expires → `state: UNPAIRED` | `JoinRequest tag INVALID for serial … — dropping (forged, corrupt, or wrong app_key)` |
| P2E-22 | Tampered / replayed downlink | rig B (DUT + `tests/p2p` gw-sim, FIBER idle): inject a corrupted-tag JoinAccept, a stale-counter Ack, and a tampered Ack body | `JoinAccept: auth failed`; stale counter silently ignored then `Uplink retry 1/3 sent`; tampered body → `Ack auth failed (counter N)`. A tampered Detach/RejoinRequest gives `Detach/RejoinRequest auth failed (counter N)` and the pairing survives | — (the central's own replay logic is a separate F-row) |

**P2E-16 cadence.** The ledger holds one entry per transmission still inside the
hour, `P2P_DUTY_LEDGER_ENTRIES` = 48. Above ~48 uplinks/hour the *entry count*
becomes the binding constraint rather than the air-time budget, so at
`interval-report 60` the duty blocks you see are slot exhaustion, not the 1 %
limit, and the row's stated criterion will not hold. Run it at 120 s (30
frames/hour). doc/p2p.md §6 records the limitation.

## 4. Pass criteria

- P2E-01..06 all pass = the S1/S2/S3 ↔ B1/B4/B5 wire contract is confirmed end-to-end
  (extended ACK, time tail, `0x56`/`0x55` round-trip and chaining).
- P2E-07/08 pass = the P2P command gating (field M-3 + command allow-lists) holds over the
  air — a remote command cannot reconfigure the radio or reach an off-transport command.
- P2E-09/10/11 = the device-side hardening (B2 duty, B3 self-heal, counter persistence) holds
  against a live central.

## 5. Known gaps / not covered here

- **B4 deferred actions** (settings_save/reboot/reset from a P2P command) are logged but not
  executed yet (FW follow-up, paired with the central's structured-command phase-2), so a
  P2P `settings_save`/reboot command will not take effect — do not test those as functional.
- **S4** (per-node channel/SF in JoinAccept) is a design-only stub on the central (regulatory
  decision pending); the `reserved(4)` field is all-zero, so nothing to exercise.
- Real USART1 transport (vs. the RTT-bridge) is not exercised until a USB-UART adapter is
  attached; the protocol is identical, only the northbridge transport differs.

## 6. Live HIL results — 2026-08-28

First live run of the real STICKER (this firmware) ↔ northbridge ↔ Proximos `control-radio`
central, over a custom RTT↔PTY bridge (see §7's runbook and the bench notes below). DUT on
J-Link 822005109, northbridge on 822005110.

| ID | Item | Result | Evidence |
|----|------|--------|----------|
| P2E-01 | join | ✅ PASS | DUT `ats radio status` → `state PAIRED, net_id b595cf19, dev_addr 0001`; central `JoinAccept -> serial=80e007b2 net_id=0xb595cf19` + `EVT_TX_DONE status=Ok` |
| P2E-02 | data plane | ✅ PASS | central `telemetry decrypted+authenticated serial=80e007b2 counter=N`; DUT fcnt advances |
| P2E-03 | **B1** ACK RSSI/SNR | ✅ PASS | central `ACK … flags=0x00 (3 B body)`; DUT `last ack rssi -56 dBm / snr 9 dB` |
| P2E-04 | **B5** clock sync | ✅ PASS | with `--deliver-time`: `ACK … flags=0x03 (7 B body)`; DUT `clock get` → `unix=1787921251 UTC 2026-08-28 12:47:31` (was "RTC not set yet") |
| P2E-05 | **B4** 0x56 downlink | ◑ PARTIAL | central `TX_SCHEDULE ok: COMMAND(0x56) counter=4 (2 B body)` + `EVT_TX_DONE status=Ok` — the 0x56 radiated; the DUT's `0x55` RESPONSE was not confirmed at the central (see below). Full round-trip proven in the central's FakeModem tests + FW unit tests. |
| P2E-09/10/11 | B2 duty / B3 self-heal / persistence | ⏳ not run | left for a follow-up bench pass |

### Bench findings (carry into the runbook / firmware)

- **Northbridge RTT-bridge build MUST set `CONFIG_SEGGER_RTT_BUFFER_SIZE_DOWN=256`** (default 16).
  With the 16 B down-buffer a host `p2p_inject` chunks too slowly, so the JoinAccept
  `TX_SCHEDULE` reaches the NB after its `aim_t_ms` → the NB silently skips the TX (`tx`=0, no
  `EVT_TX_DONE`) and the join never completes. With 256 B the inject lands in one write, the
  JoinAccept radiates, and the DUT pairs. Same class of fix as PR #404's down-buffer note.
  **Gotcha:** the larger buffer moves `_SEGGER_RTT` — re-read the address from the `.elf`
  (`nm | grep _SEGGER_RTT`) for the bridge (it moved 0x20000410 → 0x20000500 here).
- **RTT↔PTY bridge (`nb_rtt_bridge.py`) essentials:** the PTY must be raw (`tty.setraw`) or the
  line discipline mangles binary HDLC; the bridge must `os.close(sfd)` so the central can open
  the slave with `TIOCEXCL`; chunk the RTT down-write and retry.
- **B4 cross-process note (server side, MR!30):** a `node-send`-queued downlink is picked up by
  the host at **startup** (from `sessions.db`), so a restarted host delivered the 0x56 fine —
  the cross-process gap only blocks *live* injection into an already-running host, which is
  exactly what the central's control-socket routing (MR!30 phase-2) addresses.
- **B4 0x55 open item:** the DUT's RESPONSE completing the round-trip was not observed at the
  central. Likely the bench runtime filters the FW's `<inf>` logs (so `Command received` wasn't
  visible) or RX-window timing for the larger 0x56 frame; to be confirmed on a follow-up pass.

## 7. Central runbook (Proximos `control-radio`, MR!30)

Two binaries from crate `control-radio`: **`control-radio-p2p-host`** (the always-on runtime;
observability on stderr) and **`control-radio-harness`** (the `control.radio …` operator CLI;
JSON on stdout). Both read the same env:

| Var | Meaning | Bench value |
|---|---|---|
| `PROXIMOS_CONFIG` | device YAML — **must** set `radio.mode: p2p` (fixtures default to `lorawan`, else CLI fails `P2pUnavailable`) | copy `crates/control-radio/tests/integration/radio.yaml`, `mode: lorawan`→`p2p` |
| `PROXIMOS_RADIO_DATA` | state dir (`p2p/net_id`, `p2p/sessions.db`) — **identical** for the host and every CLI call | e.g. `/data/proximos/radio` |
| `PROXIMOS_P2P_UART` | northbridge serial 115200 8N1 (host `--uart` overrides) | the PTY bridging the northbridge USART1 over the RTT-bridge |

```sh
# Step 0 — provision once, BEFORE starting the host
control-radio-harness create --radio p2p                       # -> {"net_id":"…"}
printf '{"app_key":"<32-hex>"}' > /tmp/sticker.key              # key via file, never argv
control-radio-harness node-add --radio p2p --serial 0x<serial> \
    --dev-eui <16-hex> --profile default --keys-file /tmp/sticker.key

# Step 1 — start the central (add --deliver-time for the S3 step; --no-mqtt on a bench)
PROXIMOS_P2P_UART=<pty> control-radio-p2p-host --no-mqtt 2> p2p-host.log

# Step 3 (S2) — enqueue a downlink AFTER the DUT has joined; --hex must be a real fPort-85 Command
control-radio-harness node-send --radio p2p --serial 0x<serial> --hex <cmd-hex>
control-radio-harness node-list --radio p2p                    # shows "downlink_pending": N
```

**Log-line grep anchors** (`p2p-host.log`): `JoinAccept ->` · `TX_SCHEDULE ok: ACK … flags=0x??
(N B body)` (N=3 base, 7 with time; flags bit0 pending, bit1 time) · `TX_SCHEDULE ok:
COMMAND(0x56)` · `0x55 RESPONSE cleared` · `DECRYPT FAILED … NOT ACKing` · `REPLAY/implausible
counter` · `retransmit of counter … re-ACKing`.

## 8. Pre-HIL dry-run (no hardware)

The host always opens a real UART, but the **FakeModem engine tests drive the identical
`Central::on_event` path and print the same trace** — the supported smoke check before the
bench is wired:

```sh
cargo test -p control-radio --lib p2p::runtime::tests::a_downlink_is_announced_then_sent_as_0x56_then_cleared_by_0x55 -- --nocapture --exact  # S2 full EVT_RX→ACK(0x01)→0x56→0x55→ACK(0x00)
cargo test -p control-radio --lib p2p::runtime::tests::the_extended_ack_delivers_unix_time_when_enabled -- --nocapture --exact               # S3
cargo test -p control-radio --lib p2p::runtime -- --nocapture                                                                                # whole engine
```

Run these to see "what good looks like" for P2E-03..06 before committing bench time.

## 9. References

- `doc/p2p.md` §5–§7 (join, data plane, lifecycle) — the protocol this validates.
- `doc/plan/408 - LoRa improvements - P2P hardening.md` — the FW items (B1–B9).
- proximos-v2 MR !30 — the central runtime (S1–S4) and its `control-radio-p2p-host` runbook.
- PR #404 app-key/CMAC HIL — the same two-probe rig + RTT-bridge pattern.
