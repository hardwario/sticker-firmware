# STICKER — P2P transport (TOWER protocol over LoRa)

> Tracking: epic **#118**; design **PR #470**, `doc/plan/470 - TOWER protocol as the P2P
> transport.md` (the plan). This document describes what the node firmware
> (`app/src/app_radio_p2p.c`) implements; the plan holds the rationale, the decisions
> (T1–T7, D-a..D-j) and the network-side work.
>
> **The old P2P wire is gone** (decision 2026-09-28, plan §10): its 12 B header,
> `0xF0`/`0xF1` join, `0xFA` ACK body, `0xFD`/`0xFE` link frames, the
> `p2p_join_kat`/`p2p_data_kat` vectors and the `tests/p2p` gw-sim firmware were
> removed, with no rollback path. A node that ran the old wire boots `UNPAIRED` after
> the update (its 24 B pairing record no longer loads) and joins afresh.

An alternative to LoRaWAN for deployments with no LoRaWAN infrastructure: the STICKER
talks the **HARDWARIO TOWER radio protocol** (`tower-protocol` wire v3) to a ProXimos
Hub. It uses TOWER's frames, AES-CCM, counters and confirmed-send model on the SX126x
**LoRa** modem, and adds an authenticated on-air join (§5). STICKER payloads travel
in two application envelopes that a TOWER gateway treats as opaque (§3.2).

Status: node side implemented (plan P1). The Hub side (Northbridge gateway net layer,
central `control-radio`) is plan P2/P3. The `fsk` profile (TOWER-native GFSK) is
plan P5; until then P2P refuses to start with `p2p-modulation fsk` (§2).

---

## 1. Architecture overview

```
 STICKER (battery, STM32WLE5)       Northbridge (STM32WL55, Hub)        central (Hub, control-radio)
+----------------------------+    +------------------------------+    +------------------------------+
| app_radio (common core)    | RF | TOWER gateway net layer      | UART| app_key registry, join,     |
| app_radio_p2p: TOWER node, | <->| peer table + session keys,   | <-> | session DB, downlink queue, |
| join, 0x81/0x91 envelopes  |    | local ACK (~20 ms), PENDING  |     | 0x81 decode, 0x91 answers  |
+----------------------------+    +------------------------------+    +------------------------------+
```

- The **node** pairs to a network, identified by `net_id`. The `net_id` is also the
  gateway's TOWER address in every frame.
- The **gateway** holds the session keys and ACKs locally within TOWER's turnaround
  (plan T3). The central feeds its peer table. It queues downlinks and announces them
  with PENDING in the ACK (§6.3).
- The **central** holds the root keys (`radio_appkey`) and answers the join. It
  decodes the `0x81` payload with the existing decoder stack and answers `0x91`
  control requests (time, link check).

The payload layer is shared with LoRaWAN. `app_compose` builds the same protobuf
snapshots, and `app_radio` owns the queues, retry ladder, duty ledger, link
supervision, announce and uplink phase for both radios. `app_radio_p2p` is only the
TX/RX backend under it (`struct app_radio_backend`).

---

## 2. Selecting the transport

```
config radio-mode p2p        # off / lorawan / p2p
settings save                # persists + reboots
```

The P2P radio parameters are **readable** over every transport (GetConfig / GetParam
group `p2p`). They are **`writable: [shell]` only**, because they must equal the
Hub's values or the link does not exist. Setting them over the air would sever the
link that carries the command. Setting them over NFC would need a coordinated
Manager-App release and a matching Hub change.

| Param | Default | Range | Note |
|---|---|---|---|
| `p2p-modulation` | `lora` | `lora` / `fsk` | `fsk` = plan P5, **refuses to start** today |
| `p2p-frequency` | 868100000 | 863–870 MHz | |
| `p2p-spreading-factor` | 7 | 6..12 | a fixed network constant: no sweep, no ADR |
| `p2p-tx-power` | 14 | 2..22 dBm | unless the JoinAccept assigns one (§5.3) |

On boot the firmware brings up **only** the selected stack. LoRaMac and raw LoRa
share the SX126x, so the stack cannot be switched live. The join, the session and
the start-up refusals (§4, §7) are all decided in `app_radio_p2p_start()`.

Build: `CONFIG_RADIO_P2P` (default `y` on release, `n` on the flash-tight debug
overlay; §11). It implies the fork's `CONFIG_LORA_SEND_RECV_ASYNC`, which arms the
TOWER receive windows at TX-done. `app/debug_p2p_bench.conf`, layered on
`debug.conf`, is the only debug image with P2P, and it has no LoRaWAN (§14).

---

## 3. Wire format

### 3.1 TOWER frame (plan §4, adopted verbatim)

```
ver_type(1) | flags(1) | src(4 LE) | dest(4 LE) | counter(4 LE) | ciphertext | tag(8)
```

| Field | Value |
|---|---|
| `ver_type` | `version << 5 \| type`, version 1. Type 0 = Data, 1 = Ack. Another version is `-EPROTO` |
| `flags` | Data: bit 0 = CONFIRMED |
| `src` / `dest` | the node = **low 32 bits of the DevEUI**; the gateway = `net_id`. 0 and `0xFFFFFFFF` are reserved |
| `counter` | the sender's TX counter, strictly increasing per session. **0 is reserved** |
| crypto | AES-128-CCM, 8 B tag. AAD = the 14 B header. Nonce (13 B) = `src(4 LE) ‖ counter(4 LE) ‖ 0*5` |

Frame MTU is **100 B**, which leaves 78 B of payload and a **76 B body** after the
envelope (`P2P_MAX_BODY`, the budget `app_compose` bin-packs against). An ACK
frame is 28 B:

```
ACK payload: acked(4 LE) | rssi(i8) | flags(1)      flags bit 0 = PENDING
```

Receivers accept an ACK payload of ≥ 4 B and ignore any appended fields. `rssi` is
the receiver's RSSI of the acknowledged frame.

### 3.2 STICKER envelopes (plan §8)

```
0x81 | port(1) | LoRaWAN fPort payload          data
0x91 | { cmd(1) | len(1) | value(len) } x n     control TLV list
```

- **`0x81` ports** are the LoRaWAN ones: telemetry 2, alarm 3, answers 85 (or
  the command's own port), commands 86 (downlink). The body is byte-identical to the
  LoRaWAN fPort payload, so one decoder serves both radios.
- **`0x91` command IDs** (plan §8.2):

| ID | Name | Direction | Value |
|---|---|---|---|
| `0x01` | Capabilities | ↑ | `proto(1)=1 \| mtu(1)=100 \| profiles(1) \| cmds(8) \| power_class(1)=1` |
| `0x02` | Hello | ↑ | `session_id(4 LE) \| reset_reason(1) \| fw(4: major, minor, patch, 0)` |
| `0x03` | Detach | ↓ | ignored |
| `0x04` | RejoinReq | ↓ | ignored |
| `0x07` | JoinReq | ↑ | §5.3 |
| `0x08` | JoinAccept | ↓ | §5.3 |
| `0x10` | LinkCheckReq / Ans | ↑ empty / ↓ | Ans: `rssi(i8) \| snr(i8) \| margin(i8) \| gw_count(1)` |
| `0x20` | TimeReq / TimeAns | ↑ empty / ↓ | Ans: `unix(4 LE) \| frac(1, 1/256 s) \| req_counter(4 LE)` |

Capabilities fields:

- `profiles`: bit 0 = fsk, bit 1 = lora. The node sends lora.
- `cmds`: a 64-bit bitmap of the IDs this node implements (`01 02 03 04 07 08 10 20`).
- `reset_reason`: the hwinfo `RESET_*` bits 0..7.
- `session_id`: random per boot.

TLV list rules:

- An unknown ID is skipped by its length.
- A TLV whose value runs past the list stops the walk; TLVs already applied stay applied.
- The node sends every pending TLV in one confirmed frame. Capabilities, Hello,
  LinkCheckReq and TimeReq together are 30 B.

### 3.3 Radio parameters (plan §3.3)

P2P has **no power control and no SF change**: every parameter is fixed per network.

| | Value |
|---|---|
| Modulation | LoRa, BW 125 kHz, CR 4/5, preamble 8, explicit header, CRC on |
| SF | `p2p-spreading-factor`, default **SF7** |
| TX power | `p2p-tx-power` (14 dBm = RFO_LP max), or the JoinAccept's |
| Channel | `p2p-frequency`, both directions |

The plan §3.3 cost table has the reasoning. SF7 with all-confirmed reports every
900 s averages ~3.3 µA, against a 74 µA idle floor.

### 3.4 Time on air and windows

`app_radio_lora_toa_ms()` (the common formula; the duty ledger charges both radios
alike):

| Frame | SF7 | SF10 | SF12 |
|---|---|---|---|
| empty Data (22 B) | 57 ms | 371 ms | — |
| ACK (28 B) | 67 ms | 412 ms | 1647 ms |
| JoinReq / JoinAccept (39 / 38 B) | 83 ms | 494 ms | 1975 ms |
| MTU (100 B) | 175 ms | 1027 ms | 3941 ms |

Windows, measured from TX-done (`p2p_twr_window_ms()`):

- **ACK window** = turnaround 20 ms + ACK ToA + 3 symbols + 20 ms margin, never
  under 200 ms. That is SF7 200 ms, SF10 479 ms, SF12 1786 ms.
- **Downlink window** after PENDING: the same formula for a 100 B frame. That is
  SF7 221 ms, SF12 4080 ms.
- **JoinAccept window** (`p2p_rx1_timeout_ms()`) opens `rx_delay` (1 s) after the
  JoinRequest, 25 ms early. `lora_recv()`'s timeout aborts a reception in flight, so
  the window covers 12 symbols of preamble catch, the whole JoinAccept and a 120 ms
  trailing margin for a late central (F-P2P-2). That is SF7 215 ms, SF10 712 ms,
  SF12 2488 ms.

A frame whose ToA exceeds 3950 ms (the radio's 4 s TX timeout) is refused with
`-EMSGSIZE`. At SF12 the MTU still fits.

---

## 4. Keys

| Key | Holder | Derivation |
|---|---|---|
| `radio_appkey` (16 B) | device, central | The LoRaWAN OTAA AppKey, set at the tester. It is the **only** root secret of P2P. `persistent: [device_reset]`, so `factory_reset` wipes it. |
| `join_key` | device, central | `AES128-CMAC(app_key, "HIO-TWR-JOIN" ‖ 0x01 ‖ dev_eui(8, MSB-first) ‖ zero-pad to 32 B)`. Seals the JoinRequest and the JoinAccept. |
| `session_key` | device, gateway, central | `AES128-CMAC(app_key, "HIO-TWR-SES" ‖ 0x01 ‖ dev_nonce(4 BE) ‖ central_nonce(4 BE) ‖ dev_eui(8) ‖ zero-pad)`. Seals the data plane and is fresh on every join. |

- The DevEUI is taken MSB-first, as the config stores it (NOT LoRaMac's OTAA order).
- Neither key goes on the air.
- `secret_key` (the NFC command channel key) is not used by P2P at all.
- Both ends are pinned byte for byte by `tests/ccm/tower_join_kat.json` (§14).

**Zero-identity guard.** An all-zero `radio_appkey` is a publicly known key: a forged
JoinAccept under it would pair the node into a hostile network. It is also the config
default and what `factory_reset` leaves behind. A zero or reserved address
(low32(DevEUI) = 0 or `0xFFFFFFFF`) cannot appear in a frame header. In these cases
`app_radio_p2p_start()`, `app_radio_p2p_rejoin()` and the RejoinReq handler refuse
to bring the radio up, and the refusal is loud:

- `LOG_ERR`;
- the state is `DISABLED`;
- `app_key: MISSING` shows in `ats radio status`.

The start-up check runs **before** the persisted-pairing shortcut, so a zero DevEUI
refuses even a paired session.

---

## 5. Pairing (on-air join, plan §6.3)

### 5.1 Preconditions

- `radio_appkey` and `radio_deveui` are provisioned, and the central knows the
  `app_key`, as for LoRaWAN OTAA.
- `p2p-modulation lora`.
- The central's pairing policy accepts the DevEUI.

### 5.2 Join triggers

- **Boot, never paired.** The episode has two policies, not a deadline:
  - **Fast** for the first 120 s after boot: short retries (≤ 2 s jitter),
    duty-cycle aware. The wait is capped at the window edge
    (`p2p_join_retry_delay_ms()`).
  - **Slow** afterwards: the node stays `JOINING` on the common backoff
    (`app_radio_rejoin_backoff_ms()`: 60 s → ×2 → 1 h cap, ±25 % jitter). The
    jitter never undercuts a pending duty wait.

  Every JoinRequest that reaches the air, or fails to, ends a round. A duty-held
  attempt spends nothing.
- **Operator:** the shell `join` (`app_radio_p2p_rejoin()`) forces a fresh episode
  on the fast policy, even when `PAIRED`. It pre-empts a pending slow retry.
- **Self-heal:** app_radio's link supervision (§7) calls the backend's `rejoin`. It
  starts on the slow policy.
- **Network:** an authenticated `RejoinReq` (§5.4) starts a slow-policy episode.

Every JoinRequest goes out on the configured SF. There is no sweep.

### 5.3 Handshake

```
JoinRequest (39 B)  Data, src = low32(DevEUI), dest = 0, counter = dev_nonce, under join_key
  payload  0x91 | 0x07 0x0e | product_type(1)=1 | proto_version(1)=1 | dev_eui(8, MSB-first) | fw(4)

JoinAccept (38 B)   Data, src = net_id, dest = low32(DevEUI), counter = the JoinRequest's dev_nonce, under join_key
  payload  0x91 | 0x08 0x0d | net_id(4 LE) | central_nonce(4 LE) | rx_delay_s(1) | tx_power_dbm(1) | reserved(3)
```

1. **Persist the dev_nonce.** The next `dev_nonce` (`MAX(dev_nonce, 1)`, since
   TOWER reserves counter 0) is persisted **before** the JoinRequest leaves,
   fail-closed. Without a durable dev_nonce nothing is sent. The dev_nonce is never
   reset, not even by `unjoin` or a reset tier.
2. **Wait for the JoinAccept.** The node sleeps `rx_delay` minus the open margin,
   then receives for the JoinAccept window (§3.4).
3. **Check the JoinAccept.** It must open under `join_key` with the expected counter
   and dest. Its `net_id` must equal the frame's `src` and must not be reserved. Its
   `rx_delay` must be 1..15 s. A longer value is accepted and its tail ignored.
4. **Apply the TX power.** `tx_power_dbm` in 2..22 is applied as the session TX
   power. 0 means none, and a value out of range is ignored; either way the node
   still pairs.
5. **Derive and persist.** The node derives `session_key`, then persists `net_id`,
   `session_key`, `rx_delay` and the TX power as the 22 B `p2pjoin/state` record.
   It resets the TX counter to 1 and the gateway replay lane to 0.
6. **Announce.** The node announces through `app_radio` (Info → settings → first
   telemetry). It queues Capabilities + Hello (+ TimeReq when app_radio wants the
   time) for a jittered `0x91` uplink.

`tests/ccm/tower_join_kat.json` pins all of it: both keys, the three frames, and the
first uplink of the session.

### 5.4 Detach and RejoinReq

Both arrive as `0x91` TLVs in an authenticated downlink.

- **Detach** clears the pairing (RAM + NVS). The node is left `UNPAIRED` (state
  IDLE), silent, with no automatic re-join. TLVs after it are ignored.
- **RejoinReq** starts a slow-policy join, unless the identity has been cleared
  since (§4). This is the network's rekey lever.

---

## 6. Data plane

### 6.1 A confirmed send (plan §7.1)

One `app_radio` frame is one TOWER send under a fresh counter.

- **Unconfirmed:** one transmission, no receive window.
- **Confirmed:** up to **3 byte-identical transmissions** (same counter, random
  0..100 ms pause), each followed by the ACK window armed at TX-done. The first valid
  ACK ends the send.

A valid ACK:

- opens under the session key;
- is from the `net_id`, to this node;
- acknowledges this counter;
- has a counter above the gateway replay lane (§6.4).

The ACK's `rssi` is recorded as the gateway's view of the uplink (`last_ack_rssi`).
The node's own RSSI/SNR of the ACK updates the downlink quality in `RadioState`.

Unanswered after three transmissions, the send returns `-ETIMEDOUT`. It is neither a
link success nor a failure: that verdict is app_radio's, after its own retries
(`APP_RADIO_ACK_MAX_RETRIES` = 3, a random 1..2ⁿ s apart). **Each app_radio retry
is a new TOWER send under a new counter** (plan §7.3).

Every exchange is bracketed by `app_radio_air_begin/end()`, the flash/exchange gate.
Frames are spaced ≥ 200 ms apart.

### 6.2 Which uplinks are confirmed

| Frame | Confirmed | Note |
|---|---|---|
| Alarms, answers (port 85), history | always | app_radio's queues |
| Telemetry | on the link-check report | every `radio-link-check-interval`-th report and the first after link-up, as on LoRaWAN; that report also queues a LinkCheckReq |
| Telemetry, answer fetch | ≤ 3 reports | while a TimeAns or LinkCheckAns is awaited; no Poll (plan H3.8) |
| `0x91` control | always | up to 3 tries, 30 s apart, then dropped |

### 6.3 ACK with PENDING — the downlink (plan §9.1)

An ACK with PENDING keeps the receiver on for **one** gateway Data frame (the
downlink window, §3.4).

- **`0x81` port 86:** handed to `app_radio_downlink()`, the common command path,
  once the radio is released.
- **`0x91`:** its TLVs are applied (LinkCheckAns, TimeAns, Detach, RejoinReq).
- **Anything else:** dropped.
- **A confirmed downlink** is ACKed by the node 20 ms after RxDone, under its own
  next counter. The node's ACK carries `acked` = the downlink's counter and the
  node's RSSI of it. PENDING is never set in the node's ACK.

### 6.4 Counters and replay

- **The node's TX counter** starts at 1 per session. It is reserved in NVS 256 at a
  time (`p2pfc/base`), fail-closed: at the window edge, with no durable reserve, the
  send is refused, never advanced in RAM. At `0xFFFFFFFF` the counter saturates
  (`-EOVERFLOW`); a RejoinReq rekeys long before that.
- **The gateway replay lane** `gw_last` is RAM-only (plan D-d) and reset per
  session. A received counter:

| Counter | Meaning | Action |
|---|---|---|
| `> gw_last` | fresh | taken, `gw_last` lifted |
| `== gw_last` | retransmission | re-ACKed, never re-delivered |
| `< gw_last`, or 0 | replay | dropped |

### 6.5 Control requests and answers

- **TimeReq** (`time_request` backend op, when app_radio wants the time). The central
  answers with a TimeAns that carries the time at the TimeReq's TX-done and its
  counter. The node:
  1. drops an answer to any other counter, or one older than 2 h;
  2. adds the time elapsed since TX-done, rounded to the second, and sets the clock
     (`app_clock_set_network_time()`);
  3. reports `app_radio_time_event()`.

  A TimeAns is consumed once.
- **LinkCheckReq** rides the 0x91 frame 5 s after the link-check report. While one
  is unanswered, no second one is queued. The LinkCheckAns:
  - sets the uplink RSSI/SNR and margin/gw_count in `RadioState`;
  - is a passed link check (`app_radio_link_result(true)`).

  An ACK to a confirmed frame is a passed link check too.

### 6.6 Duty cycle

The common sliding-hour ledger (`app_radio_duty_*`) is used:

- the budget comes from `p2p-frequency`'s EU868 sub-band;
- every transmission is charged at its ToA, including a failed one;
- a send the ledger holds returns `-EAGAIN` with the wait, and nothing goes on the air.

The ledger is RAM-only, so a reboot forgets the hour just transmitted. This is
accepted; persisting it would cost an NVS write per frame.

---

## 7. Lifecycle

**Link state** in the common `enum app_radio_state` (the `RadioState.state` wire values):

| P2P | app_radio |
|---|---|
| `PAIRED`, started | HEALTHY |
| boot / forced join | JOINING |
| self-heal / RejoinReq join (slow) | RECONNECT |
| `UNPAIRED`, not joining (after Detach) | IDLE |
| refused start (§4, or `fsk`) | DISABLED |

`app_radio_is_ready()` is true only while `PAIRED`, so nothing is composed while a
session is being replaced.

**Link supervision** is app_radio's single machine for both radios. A failed link
check is a confirmed frame with no ACK after its retries; any authenticated downlink
is a success.

- 3 failures in a row → **WARNING**. P2P has **no rung** here (`warning_step` returns
  false: no power or SF control, plan §7.4).
- `radio-link-check-fail-rejoin` further failures → a **self-heal re-join** on the
  slow policy.
- An RF outage alone never re-joins sooner than that. The session persists on both
  sides.

| Trigger | Behavior |
|---|---|
| Normal reboot | the persisted pairing is resumed: no JoinRequest, the announce runs |
| `factory_reset` / `vendor_reset` / `lrw_reset` | `app_radio_reset_link()` clears `p2pjoin/state` (the dev_nonce and TX counter are kept). `radio_mode` and `radio_appkey` revert to their defaults, so the radio stays off, and a re-enable without re-provisioning is refused (§4) |
| `ats radio unjoin` | clears `p2pjoin/state`, reboot required. Simulates a never-paired boot |
| Mute station (M-2) | paired but no telemetry for 4 × `interval_report` → self-heal re-join (`app_radio_stale_check()`, shared with LoRaWAN) |
| Detach / RejoinReq | §5.4 |
| Firmware changing the pairing record | a record of another length does not load: `UNPAIRED`, one automatic re-join (the old wire's 24 B record → TOWER's 22 B) |

---

## 8. Gateway and central (plan §9.4, §13.2)

- **Northbridge gateway** (STM32WL55 on the Hub). It is the TOWER gateway net
  layer for the lora profile:
  - peer table with the session keys (fed by the central);
  - local ACK within the 20 ms turnaround;
  - downlink queue per node, announced with PENDING;
  - its own TX counter and replay lane per node.
- **Central** (proximos-v2 `control-radio`):
  - the app_key registry and the join: it answers the JoinReq, allocates the
    `central_nonce`, and derives and hands the session key to the gateway;
  - `0x81` decode through the STICKER decoder (`app/decoder/p2p.js` +
    `ttn.js`);
  - `0x91` answers (TimeAns, LinkCheckAns);
  - Detach / RejoinReq.

---

## 9. Management surface

Pairing window, node list/remove (issues Detach), aliases and scan are central
operations (plan §9.4, proximos-v2). The node side has no management protocol beyond
§5.4.

---

## 10. End-to-end enrollment flow

```
 tester                        central (Hub)                    device
   | radio_appkey, DevEUI  -->  | (app_key registry)             |
   |                            | <--- JoinReq (join_key) -------|
   |                            | ---- JoinAccept -------------->| session_key derived
   |                            | ---- NodeAdd(key) -> gateway   |
   |                            | <== 0x81 data / 0x91 ctrl =====|
```

No phone step is needed for P2P. The `hio.stck:clm` claiming flow is orthogonal to it.

---

## 11. Build size and limitations

Measured 2026-09-28 on the TOWER node:

| Image | Flash | RAM |
|---|---|---|
| release | 185 588 B (of 212 992 B code) | 56 036 B |
| `debug.conf` + `debug_p2p_bench.conf` | 219 336 B (the debug budget is relaxed) | — |
| `debug.conf` | 237 072 B (P2P off) | 63 772 B |

Release has room. The plain debug image has no headroom for the dual stack and keeps
`CONFIG_RADIO_P2P=n`.

Limitations:

- **`fsk` is not implemented** (plan P5). With it, P2P refuses to start, and the
  `rejoin` backend op returns `-ENOTSUP`.
- **No ADR, no power control, no SF change** (plan §3.3, §7.4). The only recovery
  from a lost link is the rejoin budget.
- **One channel**, both directions: `p2p-frequency`. The 869.525 MHz downlink
  channel is plan P7.
- **No large transfers.** TOWER bulk for history is plan P6. History replays through
  the ordinary 76 B frames.
- **The ACK carries no SNR** (TOWER). The node learns its SNR only through a
  LinkCheckAns.

---

## 12. Related work

The comparison with TOWER, LoRaWAN and other LoRa P2P stacks that motivated the move
to TOWER is in #408 §1.1/§4 and the plan's header.

## 13. Phasing

Plan §13. P0 (LoRa physical verification) passed M1–M6 on 2026-09-28. P1 (this node
net layer) is this implementation. P2 onwards is the Hub side.

---

## 14. Verification

**Known-answer tests.** These vectors are shared with the network side:

- `tests/ccm/tower_frame_kat.json`: the frame codec. It is generated from the
  upstream Rust crates (`tests/ccm/tower_kat_gen`).
- `tests/ccm/tower_join_kat.json`: the keys and join frames. It is cross-checked by
  `tower_join_kat.py`.

Each vector is compiled into a header (`tests/p2p_logic/src/tower_kat.h`,
`tower_join_kat.h`) that records the JSON's sha256.

**Native tests.** `tests/p2p_logic` runs `app_radio_p2p.c` against a fake LoRa
device (`emul_lora.c`). A gateway emulator in the suite answers every frame. The
suite covers:

- ToA and windows;
- codec KATs;
- the join end to end, under the KAT;
- the retry policy;
- confirmed repetitions, ACK/PENDING/downlink/node-ACK, replay;
- the backend result mapping;
- the control TLVs.

The common core is covered by `tests/radio_common`. Run both with
`bash tests/run_native.sh`.

**Bench shell** (`CONFIG_RADIO_P2P`, `ats radio ...`):

| Command | Use |
|---|---|
| `status` | link state, address, `net_id`, SF / TX power, counters, `gw_last`, last ACK RSSI, LinkCheckAns, app_key |
| `compose` | build one telemetry frame under the current session without sending it; dump hex |
| `listen on\|off` | continuous RX: log the TOWER header, RSSI and SNR of every frame on the channel (no key, so nothing is opened). Uplinks get `-EBUSY` while it runs |
| `ack_drop <n>` | make the next n valid ACKs appear lost, to exercise the repetitions and app_radio's retries |
| `unjoin` | clear the pairing (reboot) |
| top-level `join` | force a fresh join now |

The HIL counterpart is a STICKER against a Northbridge running the TOWER gateway
(plan §13.1 for P0; the P1 bench is the Hub).
