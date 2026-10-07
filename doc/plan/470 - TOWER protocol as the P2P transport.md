# TOWER protocol as the STICKER P2P transport (GFSK or LoRa)

> **Supersedes** the design half of `doc/p2p.md` (the P2P wire format, join, ACK body,
> link-control frames) and the draft plan of PR #410 (`radio-mode tower` as a fourth,
> separate transport). **Reverses** the 2026-08-27 decision "no protocol merge — P2P stays
> its own protocol" (#410 plan header).
>
> Decisions taken 2026-09-28 (Hynek):
>
> | # | Decision |
> |---|---|
> | **T1** | STICKER P2P is **replaced** by the TOWER radio protocol, **wire-compatible with TOWER** (`tower-firmware` net layer, `tower-protocol` wire v3). Improvements go **upstream** into TOWER, not into a private dialect. |
> | **T2** | The network side is the **ProXimos Hub**: Northbridge (STM32WL55, dumb modem today) + central (proximos-v2 `control-radio`). |
> | **T3** | **Keys live on the gateway and it ACKs locally** (TOWER model, ~20 ms turnaround). The central feeds the gateway's peer table; the gateway is no longer keyless. |
> | **T4** | The **modulation is selectable**: `fsk` (TOWER-native GFSK, bit-exact with SPIRIT1) or `lora` (the same TOWER frames over LoRa). Same frames, same crypto, same state machines; only the PHY and the timing constants differ. |
> | **T5** | **First stage: no TOWER wire change at all.** STICKER data ride in the application envelope **`0x81`**, control signals (time, link check, device status, detach, …) in a second envelope **`0x91`** (§8). Both are opaque to a TOWER gateway. |
> | **T6** | **Later stage:** the control commands move **natively into the TOWER protocol** (own frame type, piggyback on ACK/Data — §11 N1), keeping the `0x91` command IDs and TLV codec. |
> | **T7** | **Phase 0 is a physical verification on the LoRa modulation** (TOWER frames and timing on SX126x LoRa, §13.1). The FSK bit-exact interop with TOWER hardware moves to a later phase. |
>
> Surveyed reference: `hardwario/tower-firmware` @ `b7f3f4a`, `hardwario/tower-protocol` @
> `2351ea0` (crate 1.3.x, `RADIO_SCHEMA_VERSION = 2`). The comparison that motivated this
> is in #408 §1.1/§4.

---

## 1. Goals and non-goals

**Goals**

- A STICKER in `radio-mode p2p` is a TOWER net-layer node. In `fsk` mode it can talk to a
  stock TOWER Radio Dongle, and a stock TOWER node (Core Module, `radio_push_button`) can
  talk to the Hub.
- One implementation of the net layer in `app_radio_p2p.c` with a two-backend PHY shim
  (no new transport module — see memory *no extra transport modules*; the #410 plan's
  `app_tower` module is dropped).
- Pairing, uplink, downlink, link supervision, clock sync and bulk transfer all mapped onto
  TOWER mechanisms; everything STICKER already does better and that does **not** touch the
  wire (duty ledger, replay persistence, access gating, key derivation) is kept.
- Every change that *does* touch the wire is written up as an upstream proposal to
  tower-protocol / tower-firmware (§11) and lands there before (or together with) us.

**Non-goals (this plan)**

- US915 FHSS and EU LBT+AFA access modes (TOWER has them; STICKER P2P stays EU868 single
  channel for now — §13 P7 picks them up).
- Multi-gateway macro-diversity (T3 makes it a gateway-coordination problem; §9.4 sketches it,
  implementation later).
- Radio FOTA (bulk makes it possible; out of scope).
- Concurrent LoRaWAN + P2P. One radio, one stack at boot, as today.

## 2. What "wire-compatible" means, layer by layer

| Layer | Compatibility target | Notes |
|---|---|---|
| **L1 PHY** | `fsk`: bit-exact with the SPIRIT1 config in `src/radio/config.rs`. `lora`: our own — TOWER hardware cannot do LoRa. | §3 |
| **L2 frame + crypto** | Byte-exact: header, frame types, flags, nonce, CCM (8 B tag), ACK payload, JOIN, BULK. | §4; KAT vectors generated from the Rust reference |
| **L3 timing** | `fsk`: TOWER constants exactly. `lora`: scaled per SF. Timing is not on the wire, so scaling does not break L2. | §5 |
| **L4 app payload** | TOWER envelope `[schema] ‖ body`; STICKER needs its own schema id → upstream (E3). | §8 |
| **L5 gateway ↔ host** | **Not a TOWER interface — ours.** The Northbridge ↔ Hub link is internal to the Hub, so TOWER compatibility is only needed on air. We keep our own HDLC link and extend it (D5, §13.2 H2). | §9.4, decision D5 |

## 3. PHY profiles (T4)

| | `fsk` (TOWER-native) | `lora` |
|---|---|---|
| Modulation | GFSK 19 200 bps, deviation 20 kHz, BT 1.0 | LoRa, BW 125 kHz, CR 4/5 |
| Rate knob | fixed | fixed SF7 (§3.3); `p2p-spreading-factor` shell-only |
| RX bandwidth | SX126x 234.3 kHz (nearest to SPIRIT1's ~216 kHz; must pass TOWER's ±40 ppm crystal offset, ~69 kHz) | 125 kHz |
| Preamble | 4 B `0xAA…` | 8 symbols |
| Sync | 4 B `0xDB624715` | private LoRa sync word (as today) |
| Packet | variable length (1 B length), CRC-16 poly `0x1021`, PN9 whitening | explicit header, CRC on |
| Max frame | **96 B** (SPIRIT1 FIFO) | **96 B** as well — one MTU for both profiles, so a frame never depends on the PHY |
| Channels (EU868) | TOWER ch0/1/2 = 868.1 / 868.3 / 868.5 MHz | 869.525 MHz, one channel both ways (§3.3, proposal); fallback 868.1 |
| TX power | ≤ 14 dBm ERP (g1); TOWER itself runs +11.6 dBm | fixed 14 dBm on node and gateway (§3.3) |
| ToA 96 B / ACK 28 B | ~45 ms / ~16 ms | SF7 ~164 ms / ~67 ms · SF10 ~985 ms / ~412 ms · SF12 ~3.9 s / ~1.65 s |
| Link budget vs `fsk` | — | ~+20 dB (SF7) … ~+30 dB (SF12) |

### 3.1 Radio access on the SX126x

- Zephyr's `lora.h` is LoRa-only. The FSK profile needs either direct loramac-node `Radio.*`
  calls or an FSK extension of the sticker Zephyr fork's sx126x driver (`v4.3.0-sticker2`).
  This is the **same driver change as #408 B7 (CAD)** — design once (#410 Step 1 carries
  over unchanged).
- loramac-node's `MODEM_FSK` path hard-codes a 3-byte sync `C1 94 C1`
  (`radio.c:692/802`) and the CCITT CRC seed `0x1D0F` (`sx126x.h:59`). Both must be
  overridden after `SetRx/TxConfig` (`SX126xSetSyncWord`, `SX126xSetCrcSeed`,
  `SX126xSetWhiteningSeed`), or patched in the fork.
- **Fallback that guarantees bit-exactness:** hardware CRC and whitening **off** on the SX126x,
  CRC-16 and PN9 done in software over the exact byte ranges SPIRIT1 uses. Costs a few
  hundred bytes of flash and removes the whole "which bytes does the modem whiten/CRC" risk.

### 3.2 Selection

`p2p-modulation` = `fsk` | `lora`, `writable: [shell]`, applied at boot — the same
reasoning as the existing `p2p-frequency`/`-spreading-factor`/`-tx-power` (doc/p2p.md §2:
changing it over the air cuts the link carrying it). Both ends must match; a Hub runs one
profile per Northbridge (one radio).

### 3.3 Fixed `lora` radio parameters (decided 2026-09-28)

Hynek 2026-09-28: P2P has no power control and no SF change. Every radio parameter is a
fixed network constant, the same on every node and gateway. The values below balance range,
speed and battery life:

| Parameter | Fixed value | Why |
|---|---|---|
| Modulation | LoRa (D4) | ~+20 dB link budget over TOWER `fsk` |
| Frequency | **869.525 MHz** for uplink, ACK and downlink (h1.6: 10 % duty, ≤ 500 mW ERP) — **proposal, extends D7 to the uplink** | The gateway's ACK duty is the binding limit (§5). At 10 % the gateway can send ~5 400 ACKs/h at SF7, against ~540/h on 868.1 MHz. No LoRaWAN uplinks use this channel; 868.1 MHz carries every LoRaWAN join and ~⅓ of the uplinks, at the same SF7, so frames there collide on the same SF. One channel keeps the fast TX→RX path (sticker-zephyr#2) exactly as P0 measured it (P0 ran on 869.525 MHz). Risk: LoRaWAN RX2 downlinks (rare) and Meshtastic EU868 (SF11/BW250, partly orthogonal) share the channel, so check the noise at install. Fallback: 868.1 MHz (1 %) via `p2p-frequency`. |
| Spreading factor | **SF7** | Lowest energy and airtime, highest gateway capacity. Each SF step up buys ~2.5–3 dB but doubles airtime and energy and halves the gateway capacity (cost table below). |
| Bandwidth | 125 kHz | BW250 halves the airtime but costs 3 dB, the same trade as one SF step. 125 kHz is what P0 validated. |
| Coding rate | 4/5 | 4/8 adds +48 % airtime (52 B frame: 152 vs 103 ms) for ~1 dB. Errors are caught by the CRC and the CCM tag and repaired by the net-layer reps. |
| Preamble | 8 symbols (8.2 ms) | Dropping to 6 symbols saves 2 ms (2 %). 8 gave the margin in P0 M2 and is the SX126x/LoRaWAN norm. |
| Header / CRC / sync word | explicit header, CRC on (sticker-zephyr#3), private sync word 0x12 | LoRaWAN gateways (public 0x34) do not decode P2P frames, and P2P does not decode LoRaWAN frames. |
| Node TX power | **14 dBm** | This is the board's RFO_LP maximum (`rfo-lp-max-power = <14>`, `sticker.dts`) and ≤ 25 mW ERP with an antenna gain ≤ 2.15 dBi. Power is the cheapest link budget: going from 10 to 14 dBm gives +4 dB for ~+0.9 µA average. The same +4 dB from the SF costs 2–3× more. |
| Gateway TX power | 14 dBm | The path is reciprocal and both ends have the same sensitivity, so the node's 14 dBm uplink is the limit. A stronger ACK adds no range (h1.6 would allow 27 dBm); it only adds self-interference with the Hub's LoRaWAN concentrator. |
| RX gain | boosted (`REG_RX_GAIN` 0x96) on both ends — **proposal, fork change** | Gives ~+2 dB sensitivity. On the mains-powered gateway it is free and lifts the uplink, which is the limiting direction. On the node it costs ~+1 mA for ~90 ms ≈ +0.1 µA. Today the Zephyr sx12xx driver calls `Radio.Rx()` at normal gain. The change applies to the P2P path only; LoRaWAN is untouched. |
| JoinAccept `tx_power` | the central sends 0 (= none) | The node keeps its fixed `p2p-tx-power`. The field stays on the wire (KAT, TOWER layout), and the power does not change after the join. |

**Cost per SF.** Assumptions: 52 B telemetry frame, 28 B ACK, 14 dBm, a report every 900 s
with every report confirmed (worst case). TX ~24 mA and RX ~5.3 mA come from the
SX126x/STM32WL datasheets until M7 measures them. Gateway capacity = duty budget / ACK
airtime. Collision chance: 30 nodes, pure ALOHA.

| SF | Sensitivity | Link budget | Airtime 52 B / ACK | Charge per confirmed uplink | Avg current | Gateway ACKs/h at 10 % / 1 % | Collision chance |
|---|---|---|---|---|---|---|---|
| **7** | −123 dBm | 137 dB | 103 / 67 ms | 3.0 mC | **3.3 µA** | 5 390 / 540 | ~1 % |
| 8 | −126 dBm | 140 dB | 185 / 123 ms | 5.3 mC | 5.8 µA | 2 920 / 290 | ~2 % |
| 9 | −129 dBm | 143 dB | 329 / 226 ms | 9.3 mC | 10.3 µA | 1 590 / 160 | ~4 % |
| 10 | −132 dBm | 146 dB | 616 / 412 ms | 17.2 mC | 19.1 µA | 875 / 87 | ~7 % |
| 12 | −137 dBm | 151 dB | 2 466 / 1 647 ms | 68.4 mC | 76 µA | 219 / 22 | ~24 % |

**Battery.** Against the board's ~74 µA idle (`doc/power-consumption.md`), the SF7 radio is
~4 % of the budget. SF10 would add +26 %, and SF12 doubles the consumption. The old P2P
(1 s RX1) spent 7.8 mC per confirmed SF7 uplink, so TOWER timing saves ~60 %. At SF7 the
node's own duty is ~0.4 s/h (0.01 %).

**Speed at SF7.** 5.5 kbit/s raw. A confirmed uplink with its ACK takes ≈ 190 ms. A 96 B
downlink after `PENDING` adds 185 ms (P0 M6).

**Range.** SF7 at 14 dBm gives ~137 dB, or ≈ 139 dB with boosted gateway RX. That is ~+20 dB
over TOWER `fsk`. Indoors, +6 dB buys ≈ 1.4–1.6× range, so SF10 would give roughly 1.7–2× the
range for 6× the energy and ⅙ of the gateway capacity. A site that does not close at SF7
gets a second gateway (§6.3.1, D17) or a better Hub position, not a higher SF. Range per
building type is measured in M9 / P2 HIL.

**Configuration.** `p2p-frequency`, `p2p-spreading-factor` and `p2p-tx-power` stay shell-only
(bench, range tests), with the values above as defaults; production never changes them.
Proposed for P1 commit C:
- `p2p-frequency` default 869.525 MHz; `fsk` sets 868.1 MHz (TOWER ch0) explicitly, D9;
- `p2p-tx-power` max 22 → 14 dBm (the RFO_LP limit);
- the `p2p-spreading-factor` help text without the SF sweep.

## 4. Frame layer — adopted verbatim

From `tower-firmware/src/radio/frame.rs` and `crates/tower-net-core`:

```
| ver_type | flags | src(4) | dest(4) | counter(4) | [bulk_idx(3)] | ciphertext | tag(8) |
  ver_type = version(3 bits, =1) ‖ type(5 bits); all multi-byte fields little-endian
  header (14 B / 17 B bulk) = CCM AAD; payload ≤ 74 B (bulk chunk ≤ 64 B)
```

- Frame types: `Data 0, Ack 1, BulkReq 2, BulkData 3, JoinReq 4, JoinResp 5,
  JoinConfirm 6, Beacon 7`. Flags: `CONFIRMED` bit 0, `LAST_CHUNK` bit 2,
  `BULK_ANNOUNCE` bit 3.
- AES-128-CCM, N = 13, L = 2, **8-byte tag**, on the existing `app_ccm` (HW AES) — only the
  nonce layout is new: `src(4) ‖ counter(4) ‖ bulk_index(3) ‖ 0x0000`. No direction byte:
  the two directions differ by `src`.
- ACK payload: `acked_counter(4) ‖ rssi(i8) ‖ flags(1)` (`PENDING` = bit 0); the ACK uses the
  ACKer's own fresh counter; receivers accept any ACK ≥ 4 B (the rule that makes appends
  interop-safe).
- Replay rule: CCM-verify first, then `counter > last_seen` → fresh; `== last_seen` →
  retransmit, re-ACK without re-delivery; `<` → drop. Counter 0 and addresses
  `0x00000000`/`0xFFFFFFFF` reserved. TX counter saturates at 2³²−1 (fail-closed).
- **Local policies that stay ours** (not on the wire, so free to be stricter than TOWER):
  - TX counter reservation: keep the existing NVS reservation (`p2pfc`, +256 per boot).
  - The counter is spent **right after sealing, before TX** on every send path (TOWER's
    `afa_send` violates this — upstream fix U1, §11).
  - Downlink replay lane on the node is **RAM only**, reset per session and on reboot (P1
    D-d). This is replay-safe without persistence: the node listens only right after its own
    uplink, and accepts a gateway frame only after an authenticated ACK whose `acked` equals
    the counter it just spent. The TX counter is NVS-reserved and never reused, so no
    recorded ACK can match. That ACK lifts the lane above every older gateway frame before
    the DL window opens. TOWER's lazy `P = 32` persistence is not needed.
  - Duty: the existing sliding-hour ledger (`struct app_radio_duty`), not TOWER's token bucket.

Everything in this section is covered by KAT vectors **generated from the Rust reference**
(`tower-radio-core`, `tower-net-core`, `frame.rs` host code) into
`tests/ccm/tower_*_kat.json` — shared byte-for-byte with the Northbridge and the central,
the way `p2p_join_kat.json` is today.

## 5. Timing per profile

| Constant | `fsk` (= TOWER) | `lora` (formula) | SF7 | SF10 | SF12 |
|---|---|---|---|---|---|
| RX→TX turnaround | 20 ms | 20 ms | 20 | 20 | 20 |
| ACK window | 200 ms | turnaround + ToA(28 B) + 60 ms | 200 | 500 | 1 750 |
| Retransmit backoff | rand 0..100 ms | rand 0..ToA(frame) | | | |
| Confirmed reps (net layer) | 3 (1..10) | 3 | | | |
| Downlink window after `PENDING` | 1 s | ToA(96 B) + 200 ms | 1 s | 1.2 s | 4.2 s |
| JoinResp / JoinConfirm window | 300 ms | as ACK window | | | |

A gateway ACK in 20 ms is only possible with the net layer on the Northbridge MCU — that is
what T3 buys. Over the UART/RPi path the current P2P needs a 1 s RX1.

**Gateway duty is the real LoRa constraint.** At 1 % on 868.1 MHz the gateway can send
~2 250 ACKs/h in `fsk`, but only ~540 (SF7), ~87 (SF10), ~21 (SF12) in `lora`. Two options
for `lora`, both wire-neutral (they change timing/frequency, not bytes):

1. Sparse confirmation (decision #22 policy) — required anyway.
2. **Gateway ACKs and downlinks on 869.525 MHz** (h1.6, 10 %, like LoRaWAN RX2) while
   uplinks stay on 868.1 → ~870 ACKs/h at SF10. Recommended for `lora` (decision D7).
   Not applicable to `fsk` (a stock TOWER node listens for its ACK on the uplink channel).

## 6. Identity, keys and pairing

### 6.1 Addresses

TOWER addresses are 32-bit and chosen by the node. STICKER: `addr = low 32 bits of DevEUI`
(D6 — verify uniqueness across the HARDWARIO DevEUI range; fall back to FNV-1a-32(DevEUI)),
shown in `ats radio status`. The central knows every DevEUI, so it detects collisions,
which TOWER cannot. The gateway address is the network's `net_id`, learnt in the JoinAccept (§6.3).

### 6.2 Enrollment and keys (decided 2026-09-28)

Enrollment stays **as it is today** (proximos-v2, verified 2026-09-28): the user adds the
device in the Portal form (serial, DevEUI, AppKey, `radio = p2p`). The Portal runs on the Hub
(`fiber-portal.service`) and does not store the AppKey; it sends a `node-add` command to the
Hub central, which keeps one registry for both carriers
(`/data/proximos/radio/node_registry.json`, 0600, `radio: lorawan|p2p`). The central
verifies the JoinRequest and derives and persists the session
(`/data/proximos/radio/p2p/sessions.db`). `net_id` is a Hub-local random 4 B value created
with the P2P transport and persisted on the Hub. The device joins with a **JoinRequest**. There are no new
config parameters (`p2p-gw-addr` and `p2p-key-epoch` are dropped).

```
join_key    = AES-CMAC(radio_appkey, "HIO-TWR-JOIN" ‖ 0x01 ‖ DevEUI(8, MSB-first) ‖ zero pad to 32 B)
session_key = AES-CMAC(radio_appkey, "HIO-TWR-SES"  ‖ 0x01 ‖ dev_nonce(4) ‖ central_nonce(4) ‖ DevEUI(8) ‖ zero pad)
```

- Same construction as the current P2P session key (doc/p2p.md §4, PR #404), with TOWER
  labels for domain separation. The AppKey and the session key never go on air.
- A new session key on every join, as in LoRaWAN OTAA. Rekey = rejoin or a new AppKey, so no
  `key_epoch`.
- Zero-AppKey / zero-DevEUI guard stays as today: the radio refuses to start.

### 6.3 Join in TOWER frames — no wire change

The join handshake of the current P2P (`send_join_request()` / `recv_join_accept()`, `PAIRED`
state persisted in NVS, no rejoin on reboot) is kept and only re-framed:

- **JoinRequest** = TOWER Data frame, `src = addr`, `dest = 0`, `counter = dev_nonce`
  (persisted, monotonic for the device lifetime, so the `join_key` nonce never repeats),
  sealed under `join_key`, payload `0x91` cmd `0x07 JoinReq` = today's P2P JoinRequest body
  `product_type(1) ‖ proto_version(1) ‖ DevEUI(8, MSB-first) ‖ fw_version(4 BE)`. The CCM
  tag replaces today's CMAC tag.
- The Northbridge has no key for an unknown `src`: it **forwards the frame raw** to the
  central (no ACK; the join needs no 20 ms answer).
- The central finds the device by `addr` → DevEUI, verifies under its `join_key`, allocates
  `central_nonce`, derives `session_key`, installs it on the Northbridge (`NodeAdd{addr,
  session_key, last_seen = 0}`) and queues the **JoinAccept**.
- **JoinAccept** = TOWER Data frame, `src = net_id`, `dest = addr`, `counter = dev_nonce`
  (echo), sealed under `join_key`, payload `0x91` cmd `0x08 JoinAccept` =
  `net_id(4 LE) ‖ central_nonce(4 LE) ‖ rx_delay(1) ‖ tx_power(1) ‖ reserved(3)`, sent
  `rx_delay` after the JoinRequest (today's RX1 model).
- KAT: `tests/ccm/tower_join_kat.json` (generator `tower_join_kat.py`, pycryptodome).
- From then on the gateway address in every frame is **`net_id`** (the network, not an
  individual Northbridge, as in the current P2P).
- A stock TOWER dongle cannot open these frames (wrong key) and ignores them.
- The keyed-join extension E4 (§11) stays as the later native form of this handshake.

### 6.3.1 Scope of the Hub-held AppKey — multi-gateway

With the AppKey registry on the Hub, the **network is one Hub**. It may have several
Northbridges on that Hub's central (the central dedups and picks the best one for the
downlink), but **not several Hubs**. A node that roams to a second Hub cannot join there,
and the second Hub cannot ACK it.

**Multi-Hub (multi-gateway across Hubs) needs one of:**

1. **Keys in a shared join server (recommended):** a Portal instance above the Hubs (cloud,
   or one designated Hub) is the join server (holds the AppKeys,
   verifies JoinRequests, derives session keys) and pushes the session key + counters to
   every Hub of the network. Hubs hold only session keys, never AppKeys. New joins need the
   Portal online; joined nodes keep working offline.
2. **AppKeys on all Hubs of the network:** the Portal syncs the AppKey registry to every
   Hub. It works offline, but every Hub holds every AppKey of the network (larger exposure
   when a Hub is stolen or compromised), and the Hubs must share counters/dedup (session
   state sync between Hubs).

Either way the network needs one `net_id` shared by its Hubs, assigned by the join server
instead of today's Hub-local random value.

**Target topology (Hynek, 2026-09-28): one central Hub + client Hubs on a local network.**
This is option 1 with the join server on a designated Hub:

| Role | Holds | Does |
|---|---|---|
| **Central Hub** | Portal, AppKey registry, `net_id`, session DB, counters, downlink queues | join server + network server: verifies joins, derives session keys, dedups uplinks from all Hubs, decodes, picks the gateway for each downlink |
| **Client Hub** | its Northbridge(s) + a thin forwarder; session keys only in Northbridge RAM, no AppKeys, nothing on disk | forwards every uplink (and unknown-`src` joins raw) to the central over the LAN, ACKs locally, sends queued downlinks |

Rules this topology needs (P6, with multi-gateway):

- **One ACKer per node ("home gateway").** If every Northbridge that hears a confirmed
  frame ACKs it after the same 20 ms, the ACKs collide on air. The server (central Hub)
  chooses which gateway answers **by the signal strength each gateway reports**:
  - **A — chosen (D17, 2026-09-28): home gateway from history.** Every gateway forwards
    each frame with RSSI/SNR. The server keeps per-node statistics and assigns the best
    gateway as the node's home (`0x91` `HomeGateway`, cmd 0x06 reserved; installed on the
    gateways with `NodeAdd`/flags). Only the home ACKs, locally within the 20 ms
    turnaround; the others just forward. The home moves when another gateway is
    consistently better (hysteresis) or after repeated missed ACKs. Keeps the fast ACK and
    TOWER timing; reacts to a sudden change one frame late (the node retransmits).
  - **B — not chosen: per-frame arbitration.** Every gateway reports the frame; the server
    waits for all reports, picks the strongest and tells only that gateway to ACK. The round
    trip (Northbridge → RPi → LAN → server → back) does not fit 20 ms, so the turnaround
    would grow to ~100–200 ms (like LoRaWAN RX1). The node listens longer on every confirmed
    frame (energy) and the timing is no longer TOWER-compatible. Rejected for that reason.
  - Downlinks: the server always picks the gateway by the latest uplink (not time-critical).
- **Disjoint gateway TX counters.** All Northbridges send as `src = net_id` under the same
  per-node session key, so their counters must never overlap (nonce reuse otherwise). The
  central hands each Northbridge its own reserved counter block.
- **LAN or central outage.** Client Hubs keep ACKing already-joined nodes (session keys in
  RAM) and buffer uplinks until the central is back. New joins, rekeys and downlinks wait
  for the central. A client Hub reboot during the outage stops its ACKs until the central
  re-sends `NodeAdd` (§6.6).
- **Dedup** by `(addr, counter)` at the central, keeping the best RSSI/SNR per frame for the
  home-gateway choice.

### 6.4 Legacy TOWER pairing

- STICKER **never** uses public-key pairing (not compiled in, or behind a debug-only switch).
- The Hub **may** open a legacy window for stock TOWER nodes (`MgmtOp::PairingOpen{window,
  key}`, central-minted random key), user-initiated and documented as insecure (D11).

### 6.5 Unpairing

- Central: queues `Detach` (`0x91` cmd 0x03) so the node goes silent at once, then
  `NodeRemove` on the gateway → any further confirmed uplinks stop being ACKed →
  supervision (§7.4).

### 6.6 Key ownership and gateway power loss (D15)

| Layer | Holds | Does |
|---|---|---|
| Portal (on the Hub) | device metadata; the AppKey only passes through (`node-add`) | UI, enrollment, downlink commands |
| Hub central | registry DevEUI ↔ addr + AppKey, session keys, persisted counters, `net_id` | verifies joins, derives session keys, `NodeAdd` to the Northbridge, decodes `0x81`, answers `0x91`, builds downlinks |
| Northbridge | RAM only: addr → `node_key`, `last_seen`, downlink queue | CCM open/verify, ACK ≤ turnaround, `PENDING`, DL TX, forwards plaintext to the central |
| STICKER | `radio_appkey` + DevEUI (NFC provisioning) | derives its own `node_key` |

- The AppKey never leaves the Hub central and the STICKER; the Northbridge only ever sees session keys.
- **Northbridge power loss:** it boots with an empty registry and ACKs nothing (no key, no
  ACK). Nodes retry, count the frame as undelivered and keep it in history. The Northbridge
  announces `boot` on the Northbridge ↔ Hub link; the central re-sends `NodeAdd` for every node with
  its exact `last_seen` (the central sees every accepted frame, so there is no replay window)
  and a fresh reserved block for the Northbridge TX counter, persisted before use.
- The central keeps the registry in its local database, so the recovery needs no Portal.
- A Hub power loss takes both down; the Northbridge is up long before Linux. The gap (tens of
  seconds) is covered by node retries and history. No key copy in Northbridge flash
  (removable module, wear, consistency).

## 7. Uplink

### 7.1 Frame

`Data` frame, `dest = gw_addr`, payload = STICKER envelope (§8). `CONFIRMED` per the policy
below; the net layer does TOWER reps (byte-identical, same counter).

### 7.2 Which uplinks are confirmed (D7)

| Profile | Policy |
|---|---|
| `fsk` | **All uplinks confirmed** (an ACK costs ~16 ms; this is what a TOWER node does, and it gives every uplink a downlink opportunity). |
| `lora` | **All uplinks confirmed too** (Hynek 2026-10-06, finding F6): with unconfirmed telemetry a downlink waited for the link-check report, up to 5 × 900 s ≈ 75 min. A TOWER ACK at SF7 costs ~0.5 µA on average, and 30 nodes × 4 ACKs/h is far below the gateway's ACK budget (§3.3). Alarms still follow `radio-alarm-ack`. |

### 7.3 Retries

One `app_radio` attempt = one TOWER confirmed send with 3 net-layer reps. On failure the
common `app_radio` ladder (1..2ⁿ s, max 3) applies; a new attempt is a new send with a new
counter (TOWER semantics), so a delivered-but-unACKed frame can arrive twice — telemetry
dedups on the central by snapshot timestamp, responses by `seq` (as today).

### 7.4 Link supervision

Unchanged state machine (`app_radio`, LoRaWAN-parity): 3 failed checks → WARNING (every
report confirmed); `radio-link-check-fail-rejoin` further failures →
**keyed re-join** (rediscovers the gateway). Last resort after 24 h: `fsk` sweeps the three TOWER channels;
`lora` has no SF sweep (the SF is fixed for the whole fleet).

No ADR on P2P (decision 2026-09-28): TX power and SF are fixed per network, with no adaptive
data rate or power control. LoRaWAN ADR via ChirpStack is unaffected. The fixed values: §3.3.

### 7.5 Clock sync

Stage 1 (T5): `TimeReq` / `TimeAns` in the `0x91` control envelope (§8.2), answered by the
central through the downlink queue. Stage 2 (T6): the same command piggybacked on the
gateway's ACK (§11 N1), which also makes it precise to a few ms. Replaces the B5 Unix-time
tail of `0xFA`.

### 7.6 What is lost vs. the current P2P ACK

- Uplink **SNR** (the TOWER ACK carries RSSI only) — returned by `LinkCheckAns` in `0x91`
  (the central has RSSI/SNR of every uplink from the gateway's `Uplink` record).
- `pending_frame_len` — not needed: the downlink window is fixed per profile (§5).

## 8. Application payload

TOWER envelope = `[RADIO_SCHEMA_VERSION] ‖ postcard(NodeMsg | NodeCmd)`, ≤ 74 B. The gateway
never looks inside, but the TOWER host rejects an unknown schema byte.

**Upstream extension E3:** reserve leading bytes `0x80..0xFF` as *foreign application
envelopes* that TOWER hosts forward raw (e.g. MQTT `…/raw`) instead of rejecting. The net
layer and the gateway need no change — only the TOWER host's schema check. STICKER uses two
envelopes (T5), one per frame:

| Byte | Envelope | Body |
|---|---|---|
| **`0x81`** | STICKER **data** | `port(1) ‖ protobuf` |
| **`0x91`** | STICKER **control** | TLV list (§8.2) |

### 8.1 Data envelope `0x81`

```
0x81 ‖ port(1) ‖ LoRaWAN fPort payload (APP_PROTO_VERSION 0x01 ‖ protobuf)
port: 2 telemetry · 3 alarm · 85 response/announce · 86 command (downlink) — the current frame_type values
```

- The body after `port` is **byte-identical to the LoRaWAN fPort payload**, version byte
  included (P1 decision D-a), so the central reuses its decoder and `app_cmd_handle()` is
  unchanged. Budget: `fsk` 96 B frame → 70 B body; `lora` ≤ 100 B frame → 76 B body (D14).
  The `first_uplink` vector in `tower_frame_kat.json` predates this and carries no version
  byte; it tests the frame layer only and stays as is (sha pinned).

- 72 B per frame: responses are already paged for 64 B (#425); `app_compose` splits
  telemetry against a budget, which becomes 72 B for P2P (larger in `lora` if D14).
- Optional nicety (later): also emit a TOWER `NodeMsg::Info` (postcard) at boot so
  `tower-cli nodes` names a STICKER natively.
- `NodeCmd::Shell` is **not** supported — STICKER keeps typed commands with the M-3 field
  gate. A TOWER shell downlink is dropped and logged.

### 8.2 Control envelope `0x91`

```
0x91 ‖ { cmd(1) ‖ len(1) ‖ value(len) } × n        (little-endian values, like TOWER)
```

- **TLV with an explicit length**: a receiver skips an unknown `cmd` by `len` and processes
  the rest — forward compatible, unlike LoRaWAN MAC commands.
- ID space: `0x00–0x3F` core (future TOWER-native, T6), `0x40–0x7F` reserved,
  `0x80–0xFF` vendor.
- **Stage 1 semantics — end to end, node ↔ central.** The gateway stays a transparent TOWER
  bridge: uplink `0x91` frames reach the central as `Uplink` records, downlink ones are
  `QueuePush`ed like commands and delivered through the pending flag (§9.1). Everything the
  answer needs (RSSI/SNR, gateway count, time) the central has.
- Authenticated and encrypted by the frame's CCM like any payload.
- A `0x91` frame is sent confirmed when it asks for an answer, unconfirmed otherwise; an
  uplink may carry `0x81` **or** `0x91`, not both (piggybacking is stage 2, N1).
- The `app_radio` link-check machine counts a `LinkCheckAns` as an explicit check result; a
  plain TOWER ACK still counts as "link alive".

Stage 1 command set (IDs final — they carry over to N1 unchanged):

| ID | Command | Dir | Value | Replaces / purpose |
|---|---|---|---|---|
| 0x01 | `Capabilities` | ↑↓ | proto version(1), MTU(1), profiles(1), cmd bitmap(8), power class(1) | what each side understands; sent at boot/join announce |
| 0x02 | `Hello` | ↑ | session_id(4), reset_reason(1), fw version(4) | reboot on the link level (today: boot `Info`) |
| 0x03 | `Detach` | ↓ | reason(1) | `0xFD` |
| 0x04 | `RejoinReq` | ↓ | kind(1): rediscover / rekey | `0xFE` |
| 0x07 | `JoinReq` | ↑ | product_type, proto_version, DevEUI(8), fw_version(4) — under `join_key` (§6.3) | P2P JoinRequest `0xF0` |
| 0x08 | `JoinAccept` | ↓ | net_id(4), central_nonce(4), rx_delay(1), tx_power(1), reserved(3) — under `join_key` | P2P JoinAccept `0xF1` |
| 0x10 | `LinkCheckReq` / `Ans` | ↑↓ | Ans: rssi(i8), snr(i8), margin(i8), gw_count(1) | link check with numbers; uplink SNR |
| 0x11 | `RadioParamReq` / `Ans` | ↓↑ | tx_power(1), sf(1), channel(1), revert_after(1 uplinks); Ans: status bits | JoinAccept `reserved(4)` assignment (network-wide change, not ADR); auto-revert if no ACK |
| 0x14 | `LinkPolicy` | ↓ | confirm_every(1), warn_after(1), rejoin_after(1) | network-set supervision parameters |
| 0x15 | `Backoff` | ↓ | seconds(2) | gateway/central congestion or duty relief |
| 0x20 | `TimeReq` / `TimeAns` | ↑↓ | Ans: unix(4) ‖ fraction(1, 1/256 s) ‖ req_counter(4 LE) — the time at the end of the TimeReq uplink whose frame counter is `req_counter`; the node applies it relative to that frame's TX-done and drops an Ans with an unknown `req_counter` | B5 Unix-time tail, `clock_sync` |
| 0x21 | `Poll` | ↑ | — | "anything for me?" without data |
| 0x30 | `DevStatusReq` / `Ans` | ↓↑ | battery mV(2), battery %(1), MCU temp(i8), uptime(4), dl rssi/snr(2) | LoRaWAN DevStatus (#419 gap) |

Later (stage 2 or when needed): `RekeyReq/Conf` (0x05), `HomeGateway` (0x06),
`RxParamSetup` (0x12), `ChannelPlan` (0x13), `PendingInfo` (0x22), `PowerMode` (0x23),
`Ping` (0x31), `RadioStats` (0x32).

Not control (stays in `0x81`): configuration, sensors, alarms, history, typed commands.

## 9. Downlink

### 9.1 Flow (TOWER pending model)

1. Central builds the STICKER command (port 86, protobuf `Command` with `seq`) and does
   `MgmtOp::QueuePush{node_addr, ttl, data}` on the gateway.
2. The gateway marks the peer pending; the ACK of the node's next confirmed uplink carries
   `PENDING`.
3. The node keeps RX open for the profile's downlink window, receives a `Data` frame from
   `gw_addr` (confirmed → auto-ACK), dispatches it through `app_cmd_handle()` as
   `APP_CMD_TRANSPORT_P2P` (allow-lists and M-3 gate unchanged), queues the port-85 response.
4. The response is a confirmed uplink; its ACK re-flags `PENDING` while the queue is not
   empty → chaining.
5. Gateway reports the outcome as `RadioStat::Tx` (`DELIVERED/NOT_DELIVERED/…/EXPIRED`);
   central `seq` correlation and redelivery stay as today.

Latency: the next uplink on both profiles (all confirmed), i.e. at most one report interval. A
`0x91` answer queued after the uplink that asked for it rides the following uplink.

### 9.2 Deferred actions

`post_cmd_work_handler()` semantics unchanged (reboot/settings_save only after the response
was acknowledged).

### 9.3 Large transfers — TOWER bulk (P6)

Node-initiated pull (`BULK_ANNOUNCE` → `BulkReq(i)` → `BulkData`), streamed, constant RAM:
history readout over the radio (#260/#265), full ConfigDump without paging, later FOTA
fragments. Replaces the per-feature paging for P2P.

### 9.4 Gateway side (T3)

- **Northbridge firmware** (proximos/firmware, STM32WL55, 64 KB RAM): FSK + LoRa profile;
  TOWER gateway net layer — peer table (≫ TOWER's 16, e.g. 256), per-peer replay lanes
  and session keys in RAM only (D15, restored by the central after a boot), TX counter from
  central-assigned blocks, auto-ACK within 20 ms, pending flags, RAM downlink queue with TTL,
  joins forwarded raw, duty ledger counting ACKs, `RadioStat`. RDP on. Work breakdown: §13.2.
- **Host link (D5):** our own protocol, not part of TOWER — today's HDLC/CRC16 link on
  ttyAMA3, extended with the TOWER gateway messages (§13.2 H2). Nothing in it goes upstream.
  Driving a stock TOWER Radio Dongle from the central (fsk interop, P5) would be a separate
  adapter for the dongle's own console, if ever needed.
- **Central** (proximos-v2 `control-radio`, Rust): depends on the `tower-protocol` crate;
  registry DevEUI ↔ addr ↔ AppKey → `node_key`; `NodeAdd`/`NodeRemove` on every managed
  gateway; decodes port-2/3/85 with the existing Rust decoder; `QueuePush` for commands;
  MQTT surface unchanged.
- **Security trade-off (accepted with T3):** a compromised Northbridge leaks the session keys
  of its nodes (never the AppKeys). Mitigation: RDP, keys in RAM only, rejoin / new AppKeys
  after a Hub is lost.
- **Multi-gateway (later):** §6.3.1 — session keys on every gateway, one home gateway per node
  ACKs (D17 A), the others only forward `Uplink`s for dedup at the central.

## 10. What is removed from the current P2P

- Header `net_id(4) dev_addr(2) frame_type(1) FCtrl(1) counter(4)`, 4 B tag, direction nonce.
- JoinRequest/JoinAccept (`0xF0`/`0xF1`) with the 16 B CMAC tags, `dev_nonce`, session-key
  derivation per join, central-allocated `dev_addr`, `rx1_delay`.
- `0xFA` ACK body (`flags|rssi|snr|pending_len|unix`), `0xFD` Detach, `0xFE` RejoinRequest
  (→ app-level commands).
- `tests/ccm/p2p_join_kat.json`, `p2p_data_kat.json` → `tower_*_kat.json`.
- `app/decoder/p2p.js` frame parser → TOWER frame parser + STICKER envelope.

Kept: the `app_radio` facade and everything common in it (queues, retry ladder, duty
ledger, link supervision, announce, uplink phase, M-2, post-command executor), `app_ccm`,
the zero-key guards, the `p2p-*` config params.

P2P is pre-deployment, so this is a flag day (Nodes, Northbridge, central together); no
dual-protocol period. **Decided 2026-09-28 (Hynek): the old P2P is abandoned.** From the
first TOWER bench build on, the bench runs only TOWER-based P2P; there is no return to the
old P2P, no rollback provision for it and no legacy path to keep alive. The old wire code,
KAT and decoder paths are removed as part of P1/P2 (the P8 cleanup is folded in).

## 11. Upstream proposals (tower-protocol / tower-firmware)

| # | Change | Why | Wire impact |
|---|---|---|---|
| **U1** | `afa_send`: spend the TX counter right after sealing (as `send` does) | cancel → (key, nonce) reuse | none (bug fix) |
| **U2** | Duty governor: sliding-hour ledger instead of token bucket | bucket allows ~2 % in the worst sliding hour | none |
| **E3** | Schema bytes `0x80..0xFF` = foreign app envelopes, forwarded raw | STICKER `0x81` data + `0x91` control (stage 1) | host-side only |
| **N1** | **Native control channel** (T6): frame type `Ctrl = 8` carrying the `0x91` TLV list; ACK flag `CTRL` (bit 1) with a TLV tail after `flags`; Data flag `CTRL` (bit 4) with `ctrl_len(1) ‖ TLV` before the app envelope. Core IDs `0x00–0x3F` = the §8.2 table | gateway answers time/link check in its ACK (ms-precise time, no downlink round), piggyback saves airtime | additive: unknown frame type is dropped by stock parsers (`BadType`), ACK tail ignored (≥ 4 B rule); gated by `Capabilities`. **Part A (the answer in the ACK) is pulled into P3, §13.5** |
| **E4** | `KEYED` join (Join flag bit 1): JOIN frames under the per-node key, `JOIN_RESP` without the key | no key on air, mutual auth | additive; stock dongles ignore |

N1 replaces the earlier single-purpose proposals E1 (time bit in ACK) and E5 (SNR byte in
ACK). Moving from stage 1 to N1 is a carrier change only: same command IDs, same TLV codec,
so node and central keep one command implementation.

Owner/contact on the TOWER side and acceptance are an open point (D12). If an extension is
refused, we keep it **off by default** behind a flag bit so the core stays compatible.

## 12. Configuration surface

| Param | Values | Access | Note |
|---|---|---|---|
| `p2p-modulation` | `fsk` / `lora` | shell | new; reboot |
| `p2p-frequency` | 863–870 MHz | shell | `fsk` default 868.1 (TOWER ch0) |
| `p2p-spreading-factor` | 7..12 | shell | `lora` only; fixed SF7 (§3.3) |
| `p2p-tx-power` | 2..22 dBm (→ 2..14, §3.3) | shell | fixed 14 dBm (RFO_LP max) |
| gateway address | `net_id` from the JoinAccept | read-only, `ats radio status` | persisted with the session |
| address | derived from DevEUI | read-only, `ats radio status` | |

New proto_ids need the manual collision check (memory: proto_id collision gotcha).

## 13. Phases

| Phase | Content | Exit criterion |
|---|---|---|
| **P0 — LoRa physical verification (go/no-go, T7)** | TOWER frames + TOWER timing on SX126x LoRa, STICKER node ↔ **Northbridge** gateway (bench builds on both, §13.1) | M1–M8 pass; §5 timing table replaced by measured values |
| **P1 — Node net layer + envelopes** | rewrite `app_radio_p2p.c`: PHY shim (lora first, fsk stub), frame/CCM/nonce, counters, replay, confirmed send + reps, ACK/pending, `0x81` data + `0x91` control codec; KAT from Rust; native ztests (TESTABLE pattern, `tests/p2p_logic`); join in TOWER frames (§6.3) with the central's session key; the P0 Northbridge bench gateway grows the `0x81`/`0x91` codec | STICKER ↔ Northbridge (bench RTT bridge to the central or a host script): telemetry, alarms, responses in `0x81`; `Capabilities`/`Hello`/`LinkCheck`/`Time` in `0x91` |
| **P2 — Hub gateway** | Northbridge TOWER gateway net layer (lora) + Northbridge ↔ Hub link (D5); central registry, `NodeAdd`, `0x81` decode, `0x91` handling | STICKER lora → Hub → MQTT decoded; `TimeAns`/`LinkCheckAns` from the central |
| **P3 — Downlink & lifecycle** | pending/queue, commands/responses, chaining, supervision on `LinkCheckAns`, `RadioParamReq`, `Detach`/`RejoinReq`, `DevStatus` | Portal GetParam/SetParam E2E over P2P |
| **P4 — Upstream** (from P1 in parallel) | U1, U2, E3 now; N1, E4 drafted with the P1–P3 experience | E3 agreed; N1/E4 proposals submitted |
| **P5 — FSK profile** | FSK access on SX126x (driver decision with #408 B7), bit-exact vs TOWER Core Module / Radio Dongle (HW vs SW CRC/whitening) | STICKER fsk ↔ stock Radio Dongle via `tower-cli` `NodeAdd` |
| **P6 — Native control + join** | N1 (control in the TOWER protocol), keyed join E4, legacy pairing on Hub for TOWER nodes, bulk (history) | control piggybacked on ACK; zero-touch join; TOWER push-button on Hub |
| **P7 — Later** | 869.525 downlink channel for `lora` (D7), LBT+AFA / FHSS, multi-gateway, radio FOTA | — |
| **P8 — Cleanup** | rewrite `doc/p2p.md`, Manager-App params via NFC (if agreed); the removal of the old frames/KAT/decoder paths moved into P1/P2 (old P2P abandoned) | docs = code |

### 13.1 Phase 0 — physical verification on LoRa

**Question answered:** do TOWER frames with TOWER-style timing (20 ms turnaround, short ACK
window, pending downlink window) work on the SX126x in LoRa, and what are the real
constants per SF? Nothing is decided on the wire here; the output is measured numbers and
a go/no-go.

**Setup**

- **Node:** STICKER 0413 via J-Link 822005110 (Sticker-controller bench), debug build.
- **Gateway:** the bench **Northbridge** (STM32WL5MOC, board `stm32wl5moc`,
  `proximos/firmware`) with its own J-Link — currently the EDU **801053710** (moved to the
  Northbridge 2026-09-28, confirmed; always `-SelectEmuBySN 801053710 -NoGui 1` + timeout).
  Owners: node side = Sticker controller session, Northbridge = Hub controller session.
- **Bench constants:** LoRa 868.1 MHz, BW125, CR4/5, preamble 8, P2P sync word; gateway addr
  `0x4E420001`, node addr = low32(DevEUI), static key `000102…0f`; node TX counter advanced
  before TX (U1).
  Northbridge RTT on 127.0.0.1:19031 (Hub controller's JLinkGDBServer); 19021/19030 are the
  STICKER's rttt on 822005110.
- **Gateway slot gating:** flashing the TOWER gateway takes Hub P2P down for all nodes.
  Needed first: Nodes test schedules the slot and ends its runs, Hub radio switched off P2P
  (Portal), Hynek approves the NB flash directly in the Hub controller's chat. Rollback =
  NB 0.2.2 / 0.2.1 hex, then back to P2P.
- **Node FW:** throw-away bench branch off `feat-p2p`, layered on `debug_p2p_bench.conf` (P2P
  without LoRaWAN — RAM budget). Minimal code: TOWER frame codec + CCM nonce, static key and
  addresses, **node role** only, shell `ats tower …` (`tx <len> [c]`, `stats`, `sf`,
  `window`, `ack_drop`).
- **Gateway FW:** bench branch off `hynek/northbridge-p2p-protocol` (the HIL harness base,
  MR!3), built with `-DBENCH_RTT_BRIDGE=ON`. Minimal TOWER gateway **on the MCU**: continuous
  RX, frame decode + CCM with a static node key, **auto-ACK from the MCU** (the ≤ 20 ms
  turnaround cannot go through the host), `PENDING` + one queued downlink loaded over RTT.
  Every received frame and ACK is reported over RTT (`TWR_RX <hex> rssi snr t_rx`,
  `TWR_ACK t_tx`) so no central is needed in P0.
- **Host harness:** extend the `nb_hil.py` pattern (pylink, `--nb-sn` / `--dut-sn`, always
  SN): drives both shells, loads downlinks, collects both timestamp streams and computes
  M2–M8. No Hub/central, no MQTT in P0.
- KAT generator: a small Rust tool over `tower-radio-core` / `tower-net-core` / `frame.rs`
  emitting `tests/ccm/tower_*_kat.json` (kept for P1); the Northbridge codec tests against
  the same JSON.
- PPK2 on the node for energy; timestamps via `k_cycle_get_32()` around radio events on both
  sides (and a GPIO toggle + logic analyzer if the numbers are borderline).
- Why the Northbridge already here: the gateway turnaround and duty (ACKs at 1 %) are the
  risky numbers, and they must be measured on the real gateway radio path, not on a second
  STICKER. P2 then only adds the net layer on top of a proven PHY/ACK path.

**Agreed with the implementers (2026-09-28)**

- KAT: `tests/ccm/tower_frame_kat.json` (9 vectors, upstream `tower-radio-core` /
  `tower-net-core` @ `24259e3`, cross-checked with pycryptodome). Generator:
  `tests/ccm/tower_kat_gen`.
- Turnaround is TOWER's fixed wait before the ACK TX, settable 20–60 ms on the Northbridge;
  M2 passes when the node's RX is armed before that wait ends. Today's node path measures
  ~22 ms (radio sleeps between ops, F-P2P-2); a standby-between-ops driver change only if M2
  fails.
- M3 / M6 run on 869.525 MHz (10 %) so the runs fit in hours: N = 500 / 300 / 200 / 50 for
  SF7 / 9 / 10 / 12. M8 stays on 868.1 MHz.
- Node TX counter: the existing reserved block (+256 per boot, advanced before TX), so a
  node reboot never reuses a nonce or trips the Northbridge's replay check.
- Timestamps: MSI PLL mode (LSE-locked) in the node bench overlay, since the debug clock is
  ~1.22 % off. ToA is also measured on the Northbridge as a cross-check.
- 0413 leaves the Hub P2P set for P0; Nodes test clears the slot first.

**Node TX→RX without sleep (added 2026-09-28, P0 finding M2)**

M2 measured the node's RX armed 20.15–20.36 ms after TX-done, against the Northbridge's
exact 20.00 ms turnaround. The ACK only lands because of the 8-symbol preamble (8.2 ms at
SF7). Fix: a fork-driver call that sends and receives without sleeping the radio. The RX is
configured before TX, the fallback mode is STDBY_XOSC so the TCXO stays on, and SetRx is
issued on TxDone. The same call covers the DL window after `PENDING`. `lora_send()` /
`lora_recv()` are unchanged, and LoRaWAN is untouched. Target: arm ≲ 2 ms. Measured before
vs after: M2, M2b, M3 SF7.

**Measurements**

| # | What | Pass |
|---|---|---|
| M1 | Codec + CCM + nonce vs Rust KAT (native ztest) | byte-identical |
| M2 | Turnaround: Northbridge RX-done → ACK TX start; node TX-done → RX armed | Northbridge ≤ 20 ms; node RX armed before the ACK preamble |
| M3 | ACK success per SF 7 / 9 / 10 / 12, 500 confirmed frames each, close range; shrink the window until it fails | ≥ 99 %; minimal working window recorded → §5 |
| M4 | Frame lengths 14…96 B (and 255 B for D14) at SF7 and SF12; measured vs computed ToA | ToA within 2 % |
| M5 | Retransmit path (`ack_drop`): byte-identical resend → re-ACK, no re-delivery; counter/replay | no duplicate delivery, strict monotonic counters |
| M6 | Pending: ACK with `PENDING` → node window → 96 B downlink at SF7 / SF10 | ≥ 99 % received; window formula confirmed |
| M7 | Energy per confirmed uplink cycle (30 B + ACK) at SF7 / SF10 vs today's P2P RX1 (1 s) | reported (expected clearly lower) |
| M8 | Duty ledger on node and Northbridge incl. ACKs over a 1 h run | ≤ 1 % every sliding hour |
| M9 (opt.) | Range at 2 dBm: TOWER-over-LoRa vs current P2P | reported |

**Results so far (2026-09-28, 0413 ↔ bench Northbridge, 869.525 MHz, 52 B frames, lab: duty not enforced)**

| # | Result |
|---|---|
| M1 | 9/9 KAT vectors byte-identical on node and Northbridge |
| M2 | Northbridge gate 20.00–20.01 ms. Node TX-done → RX armed: 22.29 ms (old path, sleep between ops) → **0.658 ms** with `lora_send_recv_async` (hardwario/sticker-zephyr#2, merged) |
| M2b | turnaround 20 / 25 / 30 ms, both paths: 50/50 each |
| M3 | fast path, 0 retransmits: SF7 500/500, SF9 300/300, SF10 200/200, SF12 50/50 (after the window fix below). ACK after RX armed: 90.1 / 254.4 / 446.9 / 1729.8 ms |
| M3 window | SF7 ACK window shrink, 50 frames each: 100/95/92/91 ms 50/50; 90 ms fails (3 TO / 53). Minimal window = turnaround + ToA(ACK) + 3.3 ms |
| M5 | dropped ACKs: SF7 drop 1 ×100 → 100 OK (NB ok 100, dup 100); drop all 3 ×20 → 20 TO, NB delivered 20 once (dup 40); SF10 drop 1 ×30 → 30 OK. Resends byte-identical, counters strictly increasing, 0 replay / MIC fails |
| ToA | ACK 28 B at SF7: 66.95 ms measured vs 66.82 ms calculated (+0.2 %) |
| M6 | SF7 × 500, 96 B DL after `PENDING`: 500/500; ACK RxDone → DL RxDone 185.3 / 185.4 / 185.6 ms min/avg/max (DL ToA 164.1 ms); 0 MIC / replay / dup. SF10 × 200: 200/200, ACK RxDone → DL RxDone 1006.3 ms avg (ToA 983.8 ms = turnaround + ToA + ~2.5 ms) |
| M4 | SF7 7 lengths × 5 (23–255 B), SF12 6 lengths × 3 (23–96 B): 53/53 OK, counters strictly increasing. NB TX vs Semtech formula: SF7 +0.17/−0.01 ms, SF12 −2.2…−5.8 ms (−0.15 %, timebase). Node TX one CR block short at SF12 → **finding below** |
| M7 / M8 | not run: no PPK2 on this host; duty not enforced in the lab |
| NB (16:26–18:06Z) | RX 2688 frames: 2434 fresh + 254 dup (net-layer reps), 0 MIC / replay / CRC / header / overrun errors; RSSI −69…−56 dBm (mean −62.5), SNR 4…14 dB (mean 11.6) |
| NB turnaround | 20.001–20.017 ms at the 20 ms setting; 25.0 / 30.0 ms at the M2b settings |
| NB ToA (28 B) | measured − calculated: SF7 +0.16 ms (n = 1965), SF9 −0.08 ms (226.3 ms), SF10 −0.34 ms (411.6 ms), SF12 −2.26 ms (1.647 s) |
| NB DL | 69 × 96 B at SF7, TX 20.001–20.003 ms after the ACK TxDone, ToA +16 µs vs 164.1 ms; 69 ACKs carried `PENDING` |
| NB duty | report-only: 541 TX = 36.2 s/h = 100.6 % of the 1 % budget during the dense runs (lab, not enforced; the gateway duty needs the §17 measures in production) |

**Finding — ACK window:** TX-done → ACK RxDone = turnaround + ToA(ACK) + ~2 symbols +
~1.5 ms (RX-done latency on both sides): +3.9 ms at SF7, +8.8 at SF9, +15.9 at SF10, ~+64 at
SF12. A fixed-ms margin (`20 + ToA + 60 ms`) misses at SF12 (0/37). §5 rule for P1: **ACK
window = turnaround + ToA(ACK) + 3 symbols + fixed margin**, and the same for the DL window.

**Finding — fast path sent without the payload CRC (fixed):** `lora_send_recv_async` in the
fork (sticker-zephyr#2) called `SetRxConfig(crcOn = false)` before `Radio.Send()`; loramac-node
sx126x shares `PacketParams` between TX and RX, so every node uplink of P0 went out without
the LoRa payload CRC (node TX = ToA(no CRC) + ~6.8 ms + ~15 µs/B). The NB accepted them;
a corrupted frame died only at the CCM tag, and `crc_err` could never fire. Fix: `crcOn = true`
(one line, reception unchanged — the explicit header carries the CRC flag),
hardwario/sticker-zephyr#3. P1 builds use the fixed driver. M4 re-run with the fix: 53/53,
NB `crc_err` 0; node TX − ToA(CRC on) is a smooth ≈ 6.85 ms + 13–17 µs/B (radio wake, 5 ms
TCXO, SPI write) at SF7 and SF12 — the P1 window keeps the computed ToA, so this constant only
moves the window start later.

**P0 verdict: GO** (M1–M6 pass; M7/M8 not run, moved to P2 HIL).

**Go / no-go:** M1–M6 pass → P1. If M2 misses 20 ms, the `lora` constants just grow (timing
is not on the wire); a no-go only if confirmed delivery cannot be made reliable within a
window that keeps the energy advantage over today's RX1.

Per-step verification as usual (STICKER side): three build configs, `bash tests/run_native.sh`,
clang-format, configen pytest + decoder tests on yml/proto changes; flash/RAM baseline
re-measured (release budget `0x34000`).

### 13.2 Hub-side work (P2–P3)

TOWER replaces only the P2P **wire**. Enrollment, the registry, the decoded data, commands,
alarms and the MQTT surface stay as they are, so the Portal changes are small (H4). The work
sits in the Northbridge firmware (H1), the Northbridge ↔ Hub link (H2) and the central's `p2p` module
(H3). P2P is pre-deployment: flag day, no dual-wire period, no session migration (nodes
rejoin).

**H0 — P0 close-out.** NB `TWR_*` log summary (turnaround, ToA, duty ledger) into §13.1;
rollback to 0.2.2-rxsens when the node runs end.

**H1 — Northbridge gateway firmware** (proximos/firmware, from the P0 bench branch):

| # | Item |
|---|---|
| H1.1 | RAM peer table (≥ 256): `addr → {session_key, last_seen}`; `NodeAdd` / `NodeRemove` / `NodeList` from the central. Nothing in flash (D15) |
| H1.2 | Known `src`: CCM open, strict `>` replay; `==` → re-ACK without re-delivery; ACK after the turnaround (20 ms, settable); forward `Uplink{addr, counter, flags, plaintext, rssi, snr, t_rx}` |
| H1.3 | Unknown `src` or `dest = 0` (joins): forward the raw frame + RSSI/SNR/`t_rx`, no ACK |
| H1.4 | Timed TX for the JoinAccept: the central sends a sealed frame + TX time (`t_rx + rx_delay`); the NB never holds a `join_key` |
| H1.5 | TX counter from a central-assigned block (`CounterBlock{start, len}`); no TX when exhausted, ask for the next block early |
| H1.6 | Per-node DL queue with TTL: `QueuePush` → `PENDING` in the ACK, DL 20 ms after the ACK TxDone, outcome `DELIVERED / NOT_DELIVERED / EXPIRED` |
| H1.7 | `RadioConfig` from the central (modulation, frequency, SF, BW, CR, preamble, sync word, TX power); `lora` only, `fsk` refused until P5 |
| H1.8 | `boot` announce (FW version, capabilities, empty table) → the central restores the table and a fresh counter block (§6.6) |
| H1.9 | Duty ledger incl. ACKs, `RadioStat`, `stats`; frame ≤ ToA cap of the SX12xx 4 s TX timeout (§17) |

**H2 — Northbridge ↔ Hub link (D5, decided 2026-09-28).** Our own protocol, internal to
the Hub: today's HDLC/CRC16 framing on ttyAMA3, extended with the TOWER gateway messages
below. TOWER compatibility is required only on air, so this link has no upstream dependency
and no planned switch to the tower console. **Flag day** (Hynek 2026-09-28): the NB image
and the Hub software are flashed together — no version negotiation, no capability bits, no
legacy (proto-1) path in the central; rollback = both previous images. The whole link
(framing, messages, `GET_INFO` status, state machine, recovery, event delivery) is in
[470 - Northbridge-Hub link protocol.md](470%20-%20Northbridge-Hub%20link%20protocol.md);
the message tables live only there. The address collision check runs at `node-add`
**and** at join.

**H3 — Central** (proximos-v2 `control-radio`, `p2p/`):

| # | Item |
|---|---|
| H3.1 | `frame.rs` → TOWER frame codec; tests against `tests/ccm/tower_frame_kat.json` |
| H3.2 | `crypto.rs`: labels `HIO-TWR-JOIN` / `HIO-TWR-SES` (same construction as today); tests against `tests/ccm/tower_join_kat.json` |
| H3.3 | `session.rs`: the `dev_addr` allocator goes away, `addr = low32(DevEUI)`; collision check at `node-add` (reject with a clear error); dedup key `(addr: u32, counter)` |
| H3.4 | Join: raw frame → `addr` → DevEUI → verify → `central_nonce` → session → `NodeAdd` → sealed JoinAccept via `TxAt` |
| H3.5 | Persistence (`sessions.db`, new schema, old rows dropped): `addr`, `session_key`, `last_seen`, `dev_nonce`, per-NB counter block (persisted **before** it is handed out) |
| H3.6 | NB `boot` → re-send every `NodeAdd` with the exact `last_seen` + a new counter block |
| H3.7 | `0x81` → the existing decoder by port (2/3/85); uplink event shape unchanged plus the transport |
| H3.8 | `0x91`: `Capabilities` / `Hello` (store FW + caps), `LinkCheckAns` (margin from RSSI/SNR, gateway count), `TimeAns` (§8.2 format, time = the Northbridge `t_rx` of the TimeReq frame) — queued as DL; the answer rides `PENDING` on the node's next confirmed uplink (P1 forces up to 3 confirmed reports after a TimeReq/LinkCheckReq, no `Poll`) |
| H3.9 | Commands: port 86 → `0x81 ‖ 86 ‖ protobuf` → `QueuePush`; `seq` correlation and redelivery as today; DL payload ≤ the `lora` MTU (D14), longer → the existing paging |
| H3.10 | `P2pRadio` config: add `modulation` (`lora`), `sync_word`, `tx_power`; drop what only the old wire needed |
| H3.11 (P3) | `node-remove` → `Detach` (`0x91 03`) then `NodeRemove`; `RejoinReq` (`0x91 04`); `DevStatus` |

**H4 — Portal (minimal).** No change to the node-add form (serial, DevEUI, AppKey,
`radio = p2p`), decoded data, alarms or the command UI.

- `dev_addr` shows the 32-bit TOWER address (8 hex digits) instead of the 16-bit allocation;
  `net_id` and `session_state` unchanged.
- `node-add` can fail with an address collision → show the central's error.
- Hub radio settings: `modulation` shown (read-only `lora` until P5).

**H4 status (2026-10-07, NT, Hynek's OK): done in proximos-v2 `nodes-test/p2p-portal-h4` `b357e231`** (base `hynek/p2p-ack-ctrl` `46945a4b`, not pushed yet). Collision pre-check from the local registry on node-add and move → p2p (names both DevEUIs and the addr), the Hub's `AddressCollision` shown as `address_collision`; `modulation` read-only on the Hub System tab when the Hub runs P2P; the `undelivered` command document ends the command as its own result (a late answer still completes it). Portal tests 1297 pass, clippy + fmt clean. HW collision test **PASS** (2026-10-07, c69, Hynek's OK): `node-add --radio p2p` of 70b3d57e00005722 → exit 16 `AddressCollision` naming the holder 5876070000005722 and addr `00005722`, nothing registered; the agent path yields the same line, which `b357e231` classifies as `address_collision`. The Portal UI path is untested on HW until `b357e231` is deployed. Gateway doc amendments `eb7559c1` (§10.5, §15, §18).

**H5 — Tests and HIL.**
- Rust unit tests against both KATs.
- NB + 0413 (SC's P1 FW): join → telemetry → MQTT decoded; `TimeAns` / `LinkCheckAns`;
  replay / duplicate; NB power cycle → recovery without a rejoin (§6.6).
- P3: Portal GetParam / SetParam E2E over P2P.

**Order and dependencies.**
1. H2 message set, frozen.
2. H1.1–H1.3 + H3.1–H3.4, in parallel.
3. The integration point: SC's P1 join.
4. H1.4–H1.9 + H3.5–H3.10 → P2 exit (§13).
5. H3.11 + H4 → P3.

**Gating.**
- NB flashes need Hynek's OK in the Hub controller's chat.
- The Hub leaves P2P only via Nodes test.
- Branches are local or draft MRs; merges need Hynek's OK.

### 13.3 P2 close-out (planned 2026-10-06)

**Done:**
- the P2 exit (STICKER lora → Hub → MQTT decoded; `TimeAns` / `LinkCheckAns` from the
  central) passed on 2026-10-06, with 5722, the Northbridge 0.3.0 and the central in c64/c65;
- a Hub reboot without a rejoin.

What is left makes the gateway safe against link and service failures. The references are to
the link doc ([470 - Northbridge-Hub link protocol.md](470%20-%20Northbridge-Hub%20link%20protocol.md)).

| # | Item | Owner | Verified by |
|---|---|---|---|
| C1 | NB: event ring ≥ 32 + `TWR_EVT_ACK`, go-back-N resend, ring full → no ACK and no `last_seen` update (link §4.1) | HC | native tests — **done** 2026-10-06 (`hynek/nb-evt-ack` 0b820280, twister 98/98 incl. `tower_evq`; ring 32, resend 300 ms, first seq after BOOT = 1) |
| C2 | NB: `TWR_NODE_ADD` `KEEP_NEWER` (link §4.2) | HC | native tests — **done** 2026-10-06 (8388366e) |
| C3 | Central: `TWR_EVT_ACK` after persisting, dedup by delivery `seq` (window 128), `KEEP_NEWER` on every restore, the §6.2 / §6.3 sequences; command downlinks (port 86) sent `CONFIRMED` and re-pushed on `NOT_DELIVERED` (T2 finding: an unconfirmed DL lost on air goes unnoticed); `0x91` answers stay unconfirmed (the node re-asks); debug-only counter block size for T5 | HC | tests with a scripted NB (link §9) — **done** 2026-10-06 (central `hynek/p2p-tower-central-main` 31f60cc1 / 43853713 / df822bbf / 78ddedeb; control-radio 536 + make check 3758 green) |
| C4 | Golden link vectors (JSON shared by the NB native_sim and the central) + deframer fuzzing | HC | link §9 — **done** 2026-10-06. 117 vectors in `plan/control/radio/p2p_tower_link_vectors.txt`, byte-identical on both sides, 24 of them hand-built from the doc tables (GET_INFO checked against the §3 offsets). Seeded fuzz: central 20k + 2k noise, NB 10k + 2k noise, no panic, resync OK. Findings fixed: the encoder refuses reserved addr/net_id 0 and 0xFFFFFFFF (`be831912`); NB `P2P_MAX_ENCODED_LEN` 566 → 570 for a fully escaped 280 B body (`732725fd`, latent; must be in the NB 0.3.1 production build); `ack` 5 dropped and counted (`bad932a7`); drain log (`0f225cf8`). Central `f988320f` (569 lib tests), NB `6b6139b8` (twister 133/133) |
| C5 | Fold the link doc into proximos-v2 `p2p_tower_gateway.md`; this copy then only points there | HC | review by the planner — **done** 2026-10-06: proximos-v2 `hynek/p2p-tower-link-doc` `cbb8ac7e` (doc gaps found by the C4 vectors fixed) (`plan/control/radio/p2p_tower_gateway.md`: Part A link, Part B central), reviewed and approved by the planner; the MR waits for Hynek. The sticker copy becomes a pointer once that MR is merged |
| C6 | Integration image (NB 0.3.1 + central) deployed | HC | NB flash: Hynek's OK in the HC chat — **done** 2026-10-06 (NB flashed 12:21:46Z): c66 `5e16bacc` (c65 + C3), NB 0.3.1 HIL build `8a87edfe` with `RX_INJECT`, sha256 24d5f445 |
| C7 | T8-F1 (persistent MQTT sessions for the Hub consumers, mosquitto persistence): **out of P2**, tracked in a separate issue (Hynek 2026-10-06) | — | — |

**HIL (H5)**, with 5722 as the node. SC is on standby and swaps to the debug image when a test
needs RTT. NT watches the Portal. `interval_report` stays at its 60 s minimum (the FW and Portal
range is 60–86400). For a denser stream, SC triggers uplinks over the RTT shell on the debug
image (force-send); T3 at 60 s alone takes > 32 min.

| # | Test | Pass |
|---|---|---|
| T1 | Central down **≥ 30 s** (`systemctl stop`, then `start`) during a 10 s stream of confirmed uplinks, so that frames land in the gap and go through the NB ring | 0 frames lost, 0 duplicates northbound; no rejoin. Baseline on c65 (NB 0.3.0), 2026-10-06: two `restart`s, ~15 ms down, 62 frames (cnt 1837..1898), 0 lost, 0 dup, resync < 25 ms. No frame fell in the gap, so the result shows nothing about the ring and does not count as a PASS — **PASS 2026-10-06** (transport on c66; T1-F1 air-time recheck on c68: the ring frames appear in the Portal at 13:52:08/:28/:43, `evt_head_seq` taken at start, 0 resends) (c66 + NB 0.3.1 HIL: central down 45 s, the 5 fPort-2 frames replayed from the ring 60 ms after resync, cnt 1924..1962 contiguous, `rx_replay` 0, `acks_suppressed` 0, `evt_resent` 466) |
| T2 | NB reset (J-Link reset or power) | restore per §6.2 without a rejoin; the node's counters continue; `rx_replay` 0 — **PASS 2026-10-06** (c65, NB 0.3.0: resync 58 ms after `EVT_BOOT`, cnt 1285..1351 complete) |
| T3 | Central stopped longer than the ring: 34–36 uplinks at 10 s (≈ 6 min), i.e. 32 ring-ACKed + the rest suppressed (WARNING is allowed, the rejoin threshold of 8 failed link-checks is not reached) | **≤ 32 frames (the ring): lossless at report resolution**, processed in `seq` order after the restart, each at its air time (T1-F1). **Beyond the ring:** the NB suppresses ACKs, the node gives up those reports and the history backfill covers the outage at the sample cadence (no gap at sample resolution; the report resolution is lost, as decided 2026-09-25: history only bridges outages). No rejoin. `history-enable` stays on for 5722. Pre-run on c66 2026-10-06: FAIL (T3-F1 = blocker E). **Final on c68: PASS** — the ring part: 2696..2725 delivered in `seq` order within 1 s, every point at its air time, and only the newest `LinkCheckAns` sent (fix F). The beyond-ring part is taken from the c66 run (backfill of 6 history records, no gap at sample resolution, no rejoin); on c68 the 98 s leftover gap stayed below the Portal backfill trigger (> 2 × `interval_report`), a test artifact of the 10 s stream (T3-F5) |
| T4 | Replay / duplicate | net-layer reps counted as `rx_dup` with a re-ACK and no northbound duplicate; a replayed old frame → `rx_replay`, dropped. Use a debug-image hook if SC has one, otherwise native coverage + the natural reps — duplicate part **PASS 2026-10-06** (c66: `ack_drop 2`, NB `rx_dup` +2 with re-ACKs, `rx_replay` 0, northbound and Portal one point per frame); replay part **PASS** (NB 0.3.1 HIL `09f429fa`: frame 2080 captured with `twr rx_last`, re-injected after 2081 → `rx_replay` +1, no ACK, ring and northbound unchanged) |
| T5 | Counter block refill | with a small test block the NB sends `CTR_LOW`, the central hands out the next block, no `NO_COUNTER` gap — **PASS 2026-10-06** (c66, block 64: 3 `CTR_LOW` refills contiguous, 6/6 DLs delivered, `no_counter` 0, `link_err` 0) |
| T6 | Address collision | `node-add` of a DevEUI with the same low 32 bits as 5722 → exit 16 `address_collision`, nothing changes — **PASS 2026-10-06** (c65, CLI) |
| T8 | Broker down (c67): stop the local MQTT broker for ≥ 30 s during a 10 s stream | the outbox fills and drains after the broker is back; 0 lost, 0 duplicates northbound; a full outbox stops the ACKs (the node backfills from history) — c68 2026-10-06: central **PASS** (the outbox held 2794/2795 and drained at 14:12:00.31 on the broker start, overflow 0), **end-to-end FAIL** (T8-F1, out of P2). For P2, T8 counts on the central side: **PASS** |
| T7 | Second node (0413), optional for P2 | two nodes joined at once, independent counters and queues; needs a probe on 0413 |

**C3 deviations from the link doc (to review; fold into C5):**
1. A freshly started central learns the start `seq` during a 1 s window.
2. A gap of 2 s with no event → relearn the expected `seq`. Skipped `seq`s are counted in `evt_seq_gap` (northbridge stats, c67).
3. GET_INFO `now_ms` going backwards → treated as a missed NB reboot.
4. `evt_ring_max == 0` (NB 0.3.0) → no `EVT_ACK`, so a mixed deploy keeps working. This departs from the flag day.
5. A failed northbound publish is still ACKed. Fixed for c67: the frame goes to a bounded persistent outbox on disk before the ACK is owed. The drainer publishes at QoS 1 and deletes an entry only after its PUBACK, pending entries are replayed after a restart, and a full outbox stops the ACKs. "Don't ACK" alone would not work, because the counter is already persisted and the resend would be rejected as a replay. c66 is valid for T1–T5 while the broker is up.
6. Command DLs get up to 5 chances (`NOT_DELIVERED` / `RADIO_ERR` each use one). After that the central publishes a command document of kind `undelivered`, which the Portal reads since H4 (`b357e231`).
7. A persistent storage failure stalls the ring, so the NB suppresses ACKs and the node keeps its data in history.

**c67 (`01555825`, central `e39f01d6` D, `6150a7a7` A/B/C, `abbb8d12` outbox, `bb3f2369` docs; control-radio 726 tests green):** the outbox lives at `/data/proximos/radio/p2p/outbox/<id>.json` (fsync + rename), max 1000 messages; a separate QoS 1 drainer deletes an entry only after its PUBACK; overflow → the event is neither processed nor ACKed. New stats: `evt_relearn`, `evt_seq_gap`, `evt_seq_gap_seen`, `outbox_pending`, `outbox_overflow`. Delivery is at least once in three crash windows (PUBACK → delete, lost PUBACK, outbox append → sessions.db save), so northbound consumers dedup by DevEUI + frame counter.

**T3 pre-run on c66 (2026-10-06) — blocker E, fixed in c68:** the central restarted with a full ring (32) in the middle of an NB resend pass. In the learning window (deviation 1) it processed events in arrival order: fcnt 1997..2015 first, then dropped 1986..1996 as replays ("not above last seen 2015") and ACKed them, so **11 frames were lost**. Fix E: buffer the learning window (at least one full pass, or until a `seq` repeats), sort by `seq` with the start after the largest mod-256 gap, then process in order; the gap relearn (deviation 2) works the same way. **T3 needs c68** (= c67 + E).

**T3 pre-run findings (c66, NT):**
- T3-F1 = blocker E above.
- T3-F2 (capacity): 36 uplinks overflow the 32-entry ring by design. The criterion is restated (lossless up to the ring, history beyond it) instead of growing the ring, because the ring is shared by all nodes and no fixed size covers every outage.
- T3-F3 (DL waste): the ring replay queued 3 stale `LinkCheckAns` plus the live one, which cost 4 consecutive `0x91` DLs. Fix (central): keep at most one pending `LinkCheckAns` per peer, the answer to the newest request; drop answers to older replayed requests.
- T3-F4 (resolved, no loss): fcnt 2044 was never ACKed; its retry 3 went out on a fresh counter as 2046 and was ACKed at 12:50:09Z (2045 is the link-check). The NB still suppressed at 12:50:00–01 because the restarted central was in its 1 s learning window and had not ACKed the full ring yet, which is correct backpressure. `evt_head_seq` (c68 + NB 0.3.1 final) removes that window.
- T3-F5 (Portal): the backfill fires only on a gap > 2 × `interval_report`. A single given-up report leaves a gap of exactly 2 × interval, so it is never backfilled. **Dropped (Hynek 2026-10-07):** the backfill stays as on LoRaWAN (gap > 2 × interval, batched); a single given-up report is not backfilled.
- **T8-F1 (end-to-end loss):** the outbox covers only central → broker. The broker PUBACKed the drained 2794/2795 at 14:12:00.31, the Portal reconnected 2 s later (30 s backoff) with `clean_session = true`, and mosquitto has no persistence, so nobody was subscribed and both uplinks are lost for the Portal. This is not TOWER-specific: any broker restart loses northbound data for every radio (LoRaWAN too). Out of P2, tracked in a separate issue (Hynek 2026-10-06).

**C3 bugs found on HIL (T1), fixed in c67 with the outbox and `evt_seq_gap`:**
- A: the first ring event after `EVT_BOOT` is ACKed ~1 s late.
- B: the `KEEP_NEWER` flag sent to NB 0.3.0 gets `BAD_PARAM` in a loop, which breaks deviation 4. Fix: send it only when `evt_ring_max > 0`.
- C: the steady-state ACK latency sometimes exceeds 300 ms → 2 needless resends (NT T1-F2).
- **T1-F1 (major):** a ring-replayed uplink is stamped with the replay time, not the air time (`received_at = now_unix`). The 5 frames from the T1 gap (air 12:30:03–:44Z) all carry 12:30:45Z, and the Portal chart shows one point instead of five. Fix: derive `received_at` from the `EVT_TWR_UPLINK` rx `t_ms` through the same `t_ms` → wall mapping `TimeAns` uses. T1 passes fully only when this is fixed; T3 runs on c67, otherwise ~32 readings collapse onto one timestamp.

**Order:**
1. C1–C4 in parallel.
2. C6.
3. T1–T6. T1 and T3 need C1–C3, while T2, T5 and T6 can run on today's image first.
4. C5.
5. The P2 result goes into the #470 body.

**P2 is done when:** C1–C6, T1–T6 and T8 have passed. T8 counts on the central side; the broker → consumer hop (T8-F1) is a separate issue. T7 moves to P3 if 0413 has no probe yet.

**P2 result (2026-10-06): DONE.** C1–C6 and T1–T6 + T8 (central side) passed on c66/c68 + NB 0.3.1 HIL. Left for Hynek: the proximos-v2 MRs (central `hynek/p2p-tower-central-main`, NB `hynek/nb-evt-ack`, doc `hynek/p2p-tower-link-doc`) and the NB 0.3.1 production build with `732725fd`. T7 moves to P3; T8-F1 is a separate issue.

### 13.4 P3 command checks run early (c66, 2026-10-06, NT)

Each command DL went `PENDING` on the next confirmed uplink, and the fPort-85 answer cleared the queue. No WARNING, no join and no given-up report during the run.

| Check | Result |
|---|---|
| force-send (Portal) | **PASS**: DL on 2104, forced report 2106 |
| GetSettings (Portal refresh-config) | **PASS**: 57 B answer |
| SetParam `interval_report` 60 → 120 → 60 (CLI) | **PASS**: 29 B answers; save + reboot → `Hello` (reset 3) + `Capabilities`, `TimeReq` → RTC synced; counter jumps from the NVS reserve |
| GetConfig paging | **PASS**: 6 pages of 47–64 B, ~2 s apart, all ACKed, the Hub config is complete |
| clock-sync | **PASS**: `TimeReq` → `TimeAns` on the next uplink, RTC synced |

**c68 rerun (2026-10-06, NT): PASS.** All 6 command DLs (force-send, GetSettings, SetParam 120 and back to 60, GetConfig paging, clock-sync) were delivered and confirmed; `not_delivered` 0, `expired` 0, outbox 0. The node ran at its 60 s interval, so the DL latency is one report interval.

- P3-F1 (latency, not loss): a pending `0x91` answer takes the single DL slot ahead of a queued command, so force-send and SetParam each waited one extra interval (70 s and 66 s at 60 s). **Kept as is:** a `LinkCheckAns` or `TimeAns` pushed behind a command would count as a missed link-check on the node (supervision) or leave the RTC unsynced. Fixed by §13.5 (N1 part A pulled into P3: the answer rides in the ACK).

Still open in P3: node `RadioParamReq` / `DevStatus`, central H3.11 (Detach, DevStatus), HW tests of Detach/RejoinReq, supervision (outage → WARNING → rejoin) and history replay over the radio.

### 13.5 P3: `LinkCheckAns` / `TimeAns` in the ACK (N1 part A, Hynek 2026-10-06)

**Why.** Today these answers are queued as `0x91` DLs. Each one costs a whole DL frame (~40 ms at SF7) plus the node's ACK of it, and it takes the single DL slot ahead of a queued command (P3-F1: the command waits one extra interval). As a 6–17 B tail of the ACK the answer costs ~9–25 ms of airtime only on the uplinks that asked, and the DL slot stays free for commands.

**Wire (flag day, no capability gate).**
- ACK plaintext: `acked_counter(4) ‖ rssi(i8) ‖ flags(1) [‖ TLV…]`. Flags bit 1 `CTRL` = a TLV tail follows up to the end of the plaintext, in the `0x91` codec (`id(1) ‖ len(1) ‖ body`, §8.2 IDs). Bit 0 `PENDING` is unchanged; both bits may be set.
- Receivers that do not know `CTRL` ignore the tail (the ≥ 4 B ACK rule), so upstream TOWER parsers stay compatible.
- An ACK without a request carries no tail: **a normal uplink's ACK and window are unchanged.**

**Northbridge (gateway).** For a fresh confirmed `0x91` uplink that contains `LinkCheckReq` (0x10) and/or `TimeReq` (0x20), the NB builds the answers itself inside the 20 ms turnaround and appends them to the ACK:
- `LinkCheckAns` 0x10: `rssi i8, snr i8 (dB, floored), margin i8 (floored), gw_count u8 = 1`. The margin uses the same formula the central uses today (SNR minus the SF demodulation floor), moved to the NB. `gw_count` = 1 because only the home gateway ACKs (P7 limit, documented).
- `TimeAns` 0x20: `unix(4) ‖ fraction(1/256 s) ‖ req_counter(4)` = the wall time at the end of **this** received frame, from the time anchor (link `TWR_TIME_SYNC`). Without an anchor the NB omits the `TimeAns` TLV; the node re-asks later.
- Worst-case ACK (both answers + `PENDING`): 45 B, i.e. +17 B: SF7 92.4 ms (+25.6), SF8 164.4 ms (+41.0), SF12 2138 ms (+491.5). The tail is sealed at RX, before the turnaround gate (+2 AES blocks, < 1 ms).
- No anchor: a `TimeReq` gets no `TimeAns` (with an LC only `0x10`; alone a plain 6 B ACK). An unconfirmed request gets no tail. A time outside 1970..2106 counts as no anchor. `TimeAns` has ms resolution, `fraction` floored.
- `TWR_TIME_SYNC`: RSP status only; `BAD_PARAM` if the length is not 16 or `now_ms` is ahead of the NB clock; a new anchor replaces the old one; the anchor is lost at an NB reset (the restore sequences resend it).
- `EVT_TWR_UPLINK` `ack` bit 7 = a `CTRL` tail went out with this ACK, bit 6 = the tail holds a `TimeAns`; the low bits keep their meaning. A request with `TimeReq` but bit 6 clear means the NB has no anchor. The central counts and publishes "answered in ACK" per uplink from it.
- Counters: `ack_ctrl_sent` = ACKs with a tail on air (late ones too); `ack_time_no_anchor` = `TimeReq`s sealed without an anchor.
- The tail order is fixed: `0x10`, then `0x20`. A request whose TLV list is truncated gets no tail. `TimeAns` is the time of the uplink's RxDone.
- A net-layer repetition (`== last_seen`) is re-ACKed with a freshly built tail; `TimeAns` then refers to the end of that repetition.
- Other TLVs of the same `0x91` uplink (`Capabilities`, `Hello`, …) still go to the central in `EVT_TWR_UPLINK`, which carries the whole plaintext as today.

**Central.** It no longer queues `LinkCheckAns` / `TimeAns`, and it drops fix F's answer handling for them. It sends `TWR_TIME_SYNC` after every `GET_INFO` (the restore sequences and the 60 s reconcile probe, so in practice every 60 s), and at least every 10 min. It still decodes the requests for statistics.

**Node (STICKER).** After a confirmed uplink that carried `LinkCheckReq` or `TimeReq`, the ACK window grows by ToA(17 B), to turnaround + ToA(28 + 17 B) + 60 ms. It parses a `CTRL` tail and feeds it to the same handlers as a `0x91` DL answer (link-check machine, RTC). The `TimeAns` is applied relative to the TX-done of the transmission this ACK answered. A `0x91` DL answer is still accepted (harmless).

**Tests.**
- native: ACK tail codec (with and without `PENDING`, unknown TLV skipped), node window, NB answer builder incl. the margin formula and no anchor → no `TimeAns`;
- golden link vector for `TWR_TIME_SYNC` + ACK-tail KAT frames shared by node and NB (single source: proximos-v2 `plan/control/radio/p2p_tower_ack_ctrl_kat.txt`, `hynek/nb-ack-ctrl` `483d3a65`);
- HIL: LC every 5th report answered in the ACK (no `0x91` DL on air), clock-sync: the node RTC (whole seconds) within ±1 s of the Hub, rounded correctly (the `fraction` is used for rounding; a sub-second RTC set is not needed, since every application timestamp is in seconds), P3-F1 re-run (a command plus an LC in the same exchange → the command arrives on that uplink).

**Owners:** NB + central → Hub controller; node → Sticker controller. P3-F1 closes with this.

**HIL result (2026-10-06 16:17–16:37Z, SC + HC): PASS.** 5722 on p2pb `d6d0a969`, NB 0.3.2 (`hynek/nb-ack-ctrl` `17af0c83`), central c69 `bd2b8db0`.

| Check | Result |
|---|---|
| Uplinks | 144/144 ACKed; no failed link check, no WARNING |
| LC answered in the ACK | **PASS**: all 22 `LinkCheckAns` arrived in the ACK tail, none as a `0x91` DL |
| `TimeAns` in the ACK | **PASS**: all 4 in the tail; a clock-sync `TimeReq` + LC (uplink 3632) got one shared tail |
| Clock | **PASS**: RTC offset at set −0.30 / +0.41 / +0.20 / −0.48 s (criterion ±1 s) |
| NB counters | `ack_ctrl_sent` 26 (LC 23, time 4, 1 shared), `acks_late` 0, `ack_time_no_anchor` 0; the anchor refreshed every 60 s |
| Commands (NT) | force-send, clock-sync, SetParam 120 / 60 with reboots, GetConfig: all received, ACKed and answered |
| P3-F1 re-run (command + LC in one exchange) | not seen on air: the `LinkCheckReq` goes 5 s after the report, so commands ride the report ACKs and no longer queue behind an answer. The combined case is covered natively; an air run needs NT to queue a DL inside that 5 s gap. P3-F1 is closed. |

NT confirmation (Portal side, same run): 0 LC answers pushed by the central, the only `0x91` DLs were 2 × `Capabilities` after node reboots; `TimeAns` 124–134 ms after TX-done; command DL latency pushed → delivered 0.3–6.7 s at a 10 s stream; a central restart at 16:39:00 sent `TWR_TIME_SYNC` with the `GET_INFO` 175 ms after SIGTERM, started at head seq 159 without learning and resynced the peer.

Node lists: SC `~/Documents/claude/tower-p1/ack_node.txt`.

## 14. Test plan (outline)

- **KAT**: generated from the Rust crates (`tower-radio-core` CCM, `tower-net-core` nonce/ACK/
  replay/pairing kernels, `frame.rs`) — node, Northbridge and central test against the same
  JSON.
- **Interop matrix (HIL)**:

  | Node | Gateway | Profile |
  |---|---|---|
  | STICKER | stock TOWER Radio Dongle + `tower-cli` | fsk |
  | STICKER | Hub Northbridge | fsk, lora SF7/SF10 |
  | TOWER `radio_push_button` | Hub Northbridge | fsk (legacy pairing) |
  | STICKER 0413 | bench Northbridge (P0, MCU auto-ACK, RTT) | lora SF7–SF12 |

- **Power** (PPK2): energy per uplink cycle `fsk` vs `lora` SF7/SF10 incl. ACK/downlink window,
  against the 92 µA idle baseline.
- **Range**: `fsk` vs `lora` on the 5722 range setup.
- **Duty**: 24 h simulation on both ends including gateway ACKs.
- **Negative**: forged/replayed frames, stock-dongle behaviour on `KEYED` join, counter
  saturation, reboot mid-send.

## 15. Open decisions

| # | Question | Recommendation |
|---|---|---|
| D4 | Default `p2p-modulation` | **Decided 2026-09-28: `lora`**; `fsk` for TOWER-mixed sites |
| D5 | Northbridge ↔ Hub link | **Decided 2026-09-28:** our own protocol (today's HDLC link extended, §13.2 H2); TOWER compatibility only on air, no tower console |
| D6 | Address derivation | low 32 bits of DevEUI if unique across the HARDWARIO range, else FNV-1a-32 |
| D7 | `lora`: gateway ACK/downlink on 869.525 MHz (10 %) | yes, P6; sparse confirmation from P2. §3.3 proposes the uplink on 869.525 MHz too (one channel, from P2) |
| D8 | Envelope `0x81 ‖ port ‖ protobuf` | yes, pending E3 |
| D9 | `fsk` frequency: free `p2p-frequency` or TOWER channel index 0..2 | keep frequency, validate against the TOWER plan in `fsk` |
| D10 | TOWER `NodeCmd::Shell` on STICKER | drop and log |
| D11 | Legacy public-key pairing on the Hub for stock TOWER nodes | allowed, user-initiated, short window, documented as insecure |
| D12 | TOWER-side owner for the upstream work | **Decided 2026-09-28:** we first verify each proposal on our side (P0–P3), then prepare the PRs to `hardwario/tower` ourselves |
| D13 | PR: reuse #410 or a new PR | **Decided: new PR #470**; #410 closed as superseded |
| D14 | MTU per profile | **Decided 2026-09-28:** `fsk` 96 B frame (TOWER); `lora` ≤ ~100 B frame (SF12 fits the SX12xx 4 s TX timeout, §17) |
| D15 | Key ownership and gateway power loss | **Decided 2026-09-28:** keys only on the Hub (§6.6); the Northbridge holds derived keys in RAM, no flash copy |
| D17 | Which gateway ACKs in a multi-Hub network | **Decided 2026-09-28: A** — the server assigns a home gateway per node from the reported RSSI/SNR history; the home ACKs locally. B (per-frame server arbitration, ~100–200 ms turnaround) documented as not chosen (§6.3.1) |
| D16 | Fast TX→RX fork call (hardwario/sticker-zephyr#2) | **Decided 2026-09-28:** merge |

## 16. Cross-repo impact

| Repo | Work |
|---|---|
| sticker-firmware (`feat-p2p`) | `app_radio_p2p.c` rewrite, config params, KAT, decoder, docs |
| sticker Zephyr fork (`v4.3.0-sticker2`) | FSK access / sx126x sync-CRC-whitening (with #408 B7) |
| proximos/firmware (Northbridge) | P0 bench gateway (MCU auto-ACK, RTT report) off `hynek/northbridge-p2p-protocol`; then TOWER gateway net layer, FSK, Northbridge ↔ Hub link messages |
| proximos-v2 (central) | `tower-protocol` dependency, registry/key derivation, gateway mgmt, decode |
| tower-firmware / tower-protocol | U1, U2, E3 (stage 1); N1, E4 (native stage) |
| Manager-App | later: P2P params over NFC; decoder of the new frame |

## 17. Risks

- **Zephyr SX12xx 4 s TX timeout** (found in P0 by the Northbridge build): `sx12xx_lora_config()`
  hard-codes 4000 ms; a longer frame is cut mid-air and `lora_send()` blocks forever. Caps the
  `lora` MTU at ~100 B frame on SF12 and ~198 B on SF11 until the fork driver derives the
  timeout from ToA (input to D14; same fork change as #408 B7).
- **Gateway TX counter across power cycles**: the P0 Northbridge restarts it at 1 (nonce
  reuse with a static key). P2 needs a reserved-block counter on the Northbridge, persisted
  by the central (§9.4).
- **PHY bit-exactness** (whitening coverage, CRC seed/order, length byte) — P0 gate; SW
  CRC/whitening fallback.
- **Upstream latency or refusal** — extensions stay flag-gated; core compatibility holds.
- **Gateway duty in `lora`** — sparse confirmation + 869.525 MHz; sizes fleet per gateway.
- **Keys on the Hub** (T3) — RDP, rejoin / new AppKey after a Hub loss, documented threat model; multi-Hub needs §6.3.1.
- **72 B payload** — compose/paging budgets must be re-checked for every message type
  (cf. the 11 B tier audit).
- **Flash** — net layer + FSK path + bulk against the release budget; measure in P2.
- **Hub turnaround** — 20 ms `fsk` ACK needs the ACK decision on the Northbridge MCU; measure
  in P3.
