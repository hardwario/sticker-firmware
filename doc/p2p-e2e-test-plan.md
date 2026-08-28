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

| Role | Hardware | Firmware |
|---|---|---|
| STICKER DUT | J-Link Compact Base **822005109** (or EDU Mini 801053709's STICKER) | this branch (`feat-p2p` + B1–B5), a **debug + P2P bench** build (`debug.conf` + `debug_p2p_bench.conf`) so `ats radio …` shell + RTT log are available |
| Northbridge modem | J-Link Compact Base **822005110** (STM32WL5MOC) | `proximos/firmware@hynek/northbridge-p2p-protocol`, `fiber-northbridge/app` — the HDLC P2P modem. Bench uses the **RTT-bridge** transport variant (`APP_P2P_BENCH_RTT_BRIDGE`), since no USB-UART adapter is attached |
| Central | this machine | Proximos `control-radio-p2p-host` (MR!30, S1–S4), talking to the northbridge over the RTT-bridge |

**Transport note:** there is no USB-UART adapter enumerated (`/dev/ttyUSB*`/`ttyACM*` absent),
so the Proximos↔northbridge link is the RTT-bridge (`JLinkExe -RTTTelnetPort` on 822005110,
`socat` to a PTY the host opens), the same path the PR #404 HIL used. The HDLC framing and
protocol are byte-identical between the RTT-bridge and the production USART1 build — only the
transport differs — so this validates the real protocol.

**Bench hygiene:** J-Link ownership changes daily — confirm no peer session is driving
822005109/822005110 before attaching, and always pass the explicit `-SelectEmuBySN`. Flashing
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
| P2E-09 | **B2** duty | queue an alarm right behind a telemetry frame | the alarm goes out promptly (token bucket), not after a ~227 s block; sustained sends throttle to ~1 % | frames arrive at the expected cadence |
| P2E-10 | **B3** self-heal | `ats radio ack_drop 24` on the DUT (forces 8 fully-failed uplink cycles) | after 8 give-ups → `self-healing re-join (§7)`, JoinRequest with exponential backoff | central sees a re-join of a known device outside the pairing window |
| P2E-11 | persistence | reboot the DUT | resumes PAIRED from NVS (no JoinRequest), counter resumes at the reserved high-water | next uplink decrypts under the same session key, counter ≥ reservation |

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
