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
| **L5 gateway ↔ host** | Recommended: the Northbridge speaks the tower-protocol console (COBS + CRC, postcard `MgmtRequest`/`Uplink`/`RadioStat`) so the central can drive a Radio Dongle and a Northbridge the same way. | §9, decision D5 |

## 3. PHY profiles (T4)

| | `fsk` (TOWER-native) | `lora` |
|---|---|---|
| Modulation | GFSK 19 200 bps, deviation 20 kHz, BT 1.0 | LoRa, BW 125 kHz, CR 4/5 |
| Rate knob | fixed | `p2p-spreading-factor` 7..12 (default 7, decision #22) |
| RX bandwidth | SX126x 234.3 kHz (nearest to SPIRIT1's ~216 kHz; must pass TOWER's ±40 ppm crystal offset, ~69 kHz) | 125 kHz |
| Preamble | 4 B `0xAA…` | 8 symbols |
| Sync | 4 B `0xDB624715` | private LoRa sync word (as today) |
| Packet | variable length (1 B length), CRC-16 poly `0x1021`, PN9 whitening | explicit header, CRC on |
| Max frame | **96 B** (SPIRIT1 FIFO) | **96 B** as well — one MTU for both profiles, so a frame never depends on the PHY |
| Channels (EU868) | TOWER ch0/1/2 = 868.1 / 868.3 / 868.5 MHz | `p2p-frequency` (default 868.1) |
| TX power | ≤ 14 dBm ERP (g1); TOWER itself runs +11.6 dBm | 2..22 dBm config, ≤ 14 dBm ERP |
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
  - Downlink replay lane on the node is persisted on **every** accept (downlinks are rare;
    TOWER's lazy `P = 32` leaves a replay window after a reboot).
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
  announces `boot` on the console link; the central re-sends `NodeAdd` for every node with
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
| `lora` | Decision #22 policy unchanged: link check every N-th report, answers/announce/history confirmed, alarms per `radio-alarm-ack`, other telemetry unconfirmed. |

### 7.3 Retries

One `app_radio` attempt = one TOWER confirmed send with 3 net-layer reps. On failure the
common `app_radio` ladder (1..2ⁿ s, max 3) applies; a new attempt is a new send with a new
counter (TOWER semantics), so a delivered-but-unACKed frame can arrive twice — telemetry
dedups on the central by snapshot timestamp, responses by `seq` (as today).

### 7.4 Link supervision

Unchanged state machine (`app_radio`, LoRaWAN-parity): 3 failed checks → WARNING (every
report confirmed, TX power steps up); `radio-link-check-fail-rejoin` further failures →
**keyed re-join** (rediscovers the gateway). Last resort after 24 h: `lora` sweeps SF
(as today), `fsk` sweeps the three TOWER channels.

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
| 0x11 | `RadioParamReq` / `Ans` | ↓↑ | tx_power(1), sf(1), channel(1), revert_after(1 uplinks); Ans: status bits | JoinAccept `reserved(4)` assignment, adaptive power (#443); auto-revert if no ACK |
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

Latency: `fsk` — next uplink (all confirmed). `lora` — next confirmed uplink; the central can
ask for `force_send`-style polling via the link-check interval if needed.

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
- **Host link (D5, recommended):** replace the CRC16 UART protocol (`TX_PACKET`/`ENTER_RX`/…)
  with the tower-protocol console subset (`Hello`, `MgmtRequest/Response`, `Uplink`,
  `RadioStat`). Then a TOWER Radio Dongle on USB and a Northbridge on UART are the same
  kind of device for the central, and `tower-cli` can debug a Northbridge.
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
dual-protocol period.

## 11. Upstream proposals (tower-protocol / tower-firmware)

| # | Change | Why | Wire impact |
|---|---|---|---|
| **U1** | `afa_send`: spend the TX counter right after sealing (as `send` does) | cancel → (key, nonce) reuse | none (bug fix) |
| **U2** | Duty governor: sliding-hour ledger instead of token bucket | bucket allows ~2 % in the worst sliding hour | none |
| **E3** | Schema bytes `0x80..0xFF` = foreign app envelopes, forwarded raw | STICKER `0x81` data + `0x91` control (stage 1) | host-side only |
| **N1** | **Native control channel** (T6): frame type `Ctrl = 8` carrying the `0x91` TLV list; ACK flag `CTRL` (bit 1) with a TLV tail after `flags`; Data flag `CTRL` (bit 4) with `ctrl_len(1) ‖ TLV` before the app envelope. Core IDs `0x00–0x3F` = the §8.2 table | gateway answers time/link check in its ACK (ms-precise time, no downlink round), piggyback saves airtime | additive: unknown frame type is dropped by stock parsers (`BadType`), ACK tail ignored (≥ 4 B rule); gated by `Capabilities` |
| **E4** | `KEYED` join (Join flag bit 1): JOIN frames under the per-node key, `JOIN_RESP` without the key | no key on air, mutual auth | additive; stock dongles ignore |

N1 replaces the earlier single-purpose proposals E1 (time bit in ACK) and E5 (SNR byte in
ACK). Moving from stage 1 to N1 is a carrier change only: same command IDs, same TLV codec,
so node and central keep one command implementation.
| **D5** | Gateway console spoken by non-TOWER gateways (Northbridge) | interchangeable gateways | none (reuse) |

Owner/contact on the TOWER side and acceptance are an open point (D12). If an extension is
refused, we keep it **off by default** behind a flag bit so the core stays compatible.

## 12. Configuration surface

| Param | Values | Access | Note |
|---|---|---|---|
| `p2p-modulation` | `fsk` / `lora` | shell | new; reboot |
| `p2p-frequency` | 863–870 MHz | shell | `fsk` default 868.1 (TOWER ch0) |
| `p2p-spreading-factor` | 7..12 | shell | `lora` only |
| `p2p-tx-power` | 2..22 dBm | shell | ≤ 14 dBm ERP |
| gateway address | `net_id` from the JoinAccept | read-only, `ats radio status` | persisted with the session |
| address | derived from DevEUI | read-only, `ats radio status` | |

New proto_ids need the manual collision check (memory: proto_id collision gotcha).

## 13. Phases

| Phase | Content | Exit criterion |
|---|---|---|
| **P0 — LoRa physical verification (go/no-go, T7)** | TOWER frames + TOWER timing on SX126x LoRa, STICKER node ↔ **Northbridge** gateway (bench builds on both, §13.1) | M1–M8 pass; §5 timing table replaced by measured values |
| **P1 — Node net layer + envelopes** | rewrite `app_radio_p2p.c`: PHY shim (lora first, fsk stub), frame/CCM/nonce, counters, replay, confirmed send + reps, ACK/pending, `0x81` data + `0x91` control codec; KAT from Rust; native ztests (TESTABLE pattern, `tests/p2p_logic`); join in TOWER frames (§6.3) with the central's session key; the P0 Northbridge bench gateway grows the `0x81`/`0x91` codec | STICKER ↔ Northbridge (bench RTT bridge to the central or a host script): telemetry, alarms, responses in `0x81`; `Capabilities`/`Hello`/`LinkCheck`/`Time` in `0x91` |
| **P2 — Hub gateway** | Northbridge TOWER gateway net layer (lora) + console link (D5); central registry, `NodeAdd`, `0x81` decode, `0x91` handling | STICKER lora → Hub → MQTT decoded; `TimeAns`/`LinkCheckAns` from the central |
| **P3 — Downlink & lifecycle** | pending/queue, commands/responses, chaining, supervision on `LinkCheckAns`, `RadioParamReq`, `Detach`/`RejoinReq`, `DevStatus` | Portal GetParam/SetParam E2E over P2P |
| **P4 — Upstream** (from P1 in parallel) | U1, U2, E3 now; N1, E4 drafted with the P1–P3 experience | E3 agreed; N1/E4 proposals submitted |
| **P5 — FSK profile** | FSK access on SX126x (driver decision with #408 B7), bit-exact vs TOWER Core Module / Radio Dongle (HW vs SW CRC/whitening) | STICKER fsk ↔ stock Radio Dongle via `tower-cli` `NodeAdd` |
| **P6 — Native control + join** | N1 (control in the TOWER protocol), keyed join E4, legacy pairing on Hub for TOWER nodes, bulk (history) | control piggybacked on ACK; zero-touch join; TOWER push-button on Hub |
| **P7 — Later** | 869.525 downlink channel for `lora` (D7), LBT+AFA / FHSS, multi-gateway, radio FOTA | — |
| **P8 — Cleanup** | remove old frames/KAT/decoder paths, rewrite `doc/p2p.md`, Manager-App params via NFC (if agreed) | docs = code |

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
| M6 | SF7 × 500, 96 B DL after `PENDING`: 500/500; ACK RxDone → DL RxDone 185.3 / 185.4 / 185.6 ms min/avg/max (DL ToA 164.1 ms); 0 MIC / replay / dup. SF10 × 200 pending |
| NB (16:26–18:06Z) | RX 2688 frames: 2434 fresh + 254 dup (net-layer reps), 0 MIC / replay / CRC / header / overrun errors; RSSI −69…−56 dBm (mean −62.5), SNR 4…14 dB (mean 11.6) |
| NB turnaround | 20.001–20.017 ms at the 20 ms setting; 25.0 / 30.0 ms at the M2b settings |
| NB ToA (28 B) | measured − calculated: SF7 +0.16 ms (n = 1965), SF9 −0.08 ms (226.3 ms), SF10 −0.34 ms (411.6 ms), SF12 −2.26 ms (1.647 s) |
| NB DL | 69 × 96 B at SF7, TX 20.001–20.003 ms after the ACK TxDone, ToA +16 µs vs 164.1 ms; 69 ACKs carried `PENDING` |
| NB duty | report-only: 541 TX = 36.2 s/h = 100.6 % of the 1 % budget during the dense runs (lab, not enforced; the gateway duty needs the §17 measures in production) |

**Finding — ACK window:** TX-done → ACK RxDone = turnaround + ToA(ACK) + ~2 symbols +
~1.5 ms (RX-done latency on both sides): +3.9 ms at SF7, +8.8 at SF9, +15.9 at SF10, ~+64 at
SF12. A fixed-ms margin (`20 + ToA + 60 ms`) misses at SF12 (0/37). §5 rule for P1: **ACK
window = turnaround + ToA(ACK) + 3 symbols + fixed margin**, and the same for the DL window.

**Go / no-go:** M1–M6 pass → P1. If M2 misses 20 ms, the `lora` constants just grow (timing
is not on the wire); a no-go only if confirmed delivery cannot be made reliable within a
window that keeps the energy advantage over today's RX1.

Per-step verification as usual (STICKER side): three build configs, `bash tests/run_native.sh`,
clang-format, configen pytest + decoder tests on yml/proto changes; flash/RAM baseline
re-measured (release budget `0x34000`).

### 13.2 Hub-side work (P2–P3)

TOWER replaces only the P2P **wire**. Enrollment, the registry, the decoded data, commands,
alarms and the MQTT surface stay as they are, so the Portal changes are small (H4). The work
sits in the Northbridge firmware (H1), the console link (H2) and the central's `p2p` module
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

**H2 — Console link (D5, decided 2026-09-28).** Today's HDLC/CRC16 framing on ttyAMA3
with a TOWER-console-shaped message set; the tower console framing comes later as a codec
swap, because tower-protocol v3 has no raw-frame forward, no `last_seen` in `NodeAdd`, no
counter blocks and no SNR in `Uplink` (an upstream wire v4 first, D12). The NB announces
`proto_version = 2` in `EVT_BOOT` / `GET_INFO`; the central picks the TOWER adapter by it.
Frozen message set (full text: proximos-v2 `plan/control/radio/p2p_tower_gateway.md` §2),
LE, response = `0x40 | cmd` + status (new: 6 `NO_SPACE`, 7 `NOT_FOUND`):

| Dir | Cmd | Message |
|---|---|---|
| → NB | 0x02 | `SET_RADIO_CONFIG` (existing; `lora` only) |
| → NB | 0x05 | `TX_SCHEDULE` (existing; JoinAccept only, sealed by the central) |
| → NB | 0x10 | `TWR_START {net_id u32, turnaround_ms u8, dl_gap_ms u8, flags}` |
| → NB | 0x11 | `TWR_NODE_ADD {addr u32, session_key[16], last_seen u32, flags: bit0 HOME}` |
| → NB | 0x12 | `TWR_NODE_REMOVE {addr}` |
| → NB | 0x13 | `TWR_CTR_BLOCK {first u32, last u32}` |
| → NB | 0x14 | `TWR_QUEUE_PUSH {addr, item u16, ttl_s u16, flags: bit0 CONFIRMED, len, plaintext ≤ 78}` |
| → NB | 0x15 | `TWR_QUEUE_DROP {addr, item \| 0}` |
| → NB | 0x16 | `TWR_GET_STATS` (u32 list, append-only) |
| → NB | 0x17 | `TWR_NODE_LIST {start u16}` → paged `{addr, last_seen, flags}`, no keys |
| NB → | 0x80 | `EVT_BOOT` (existing; proto 2, empty table → the central restores) |
| NB → | 0x81 | `EVT_RX` (existing raw frame: `dest = 0` or unknown `src`; `t_ms, rssi, snr_q, frame`) |
| NB → | 0x84 | `EVT_TWR_UPLINK {t_ms u64, rssi i16, snr_q i8, frame_flags, addr, counter, ack (0 none / 1 sent / 2 +PENDING / 3 late / 4 no counter), len, plaintext}` — fresh frames only |
| NB → | 0x85 | `EVT_TWR_TX {addr, item, outcome (DELIVERED / SENT / NOT_DELIVERED / EXPIRED / RADIO_ERR / NO_COUNTER), gw_counter, node_ack_counter, ack_rssi}` |
| NB → | 0x86 | `EVT_TWR_CTR_LOW {next, last}` below 25 % left; fail-closed when exhausted |

The legacy proto-1 central path stays (selected by the NB image, TOWER sessions stored
apart) until P8, so the bench can still roll back to the legacy NB. The address collision
check runs at `node-add` **and** at join.

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
| D5 | Northbridge ↔ RPi link = tower-protocol console | **Decided 2026-09-28:** today's HDLC framing + TOWER-console-shaped messages (§13.2 H2); console framing later as a codec swap after an upstream wire v4 |
| D6 | Address derivation | low 32 bits of DevEUI if unique across the HARDWARIO range, else FNV-1a-32 |
| D7 | `lora`: gateway ACK/downlink on 869.525 MHz (10 %) | yes, P6; sparse confirmation from P2 |
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
| proximos/firmware (Northbridge) | P0 bench gateway (MCU auto-ACK, RTT report) off `hynek/northbridge-p2p-protocol`; then TOWER gateway net layer, FSK, console link |
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
