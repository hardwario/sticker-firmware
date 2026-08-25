# STICKER — LoRa P2P transport, pairing & network design

> Tracking: epic **#118**. Supersedes the design half of draft PR **#228**
> (`feat/lora-p2p`) — the transport layer below is carried over from it, the
> pairing/enrollment and gateway/central architecture are new. PR #228 remains
> the implementation reference for the TX path; its config surface (`transport`
> enum) is replaced by the mainline `radio-mode` parameter (#271), and its
> crypto layer by the mainline `app_ccm` (#261).

An alternative to the LoRaWAN transport for deployments with no LoRaWAN
infrastructure: the STICKER transmits telemetry / alarm frames over **raw
LoRa** (no network server, no OTAA/ABP join) to a private receiver network.
Unlike the v1 draft in PR #228 — which was fire-and-forget to a hand-configured
sniffer — this design adds **on-air pairing**, **acknowledged uplinks** and a
**gateway/central architecture** so a device can be enrolled into a system the
way HARDWARIO TOWER nodes pair to a Radio Dongle, but with real cryptography
(TOWER's sub-GHz link has none).

Status: **design document**, phase 1 (§13) implemented and HIL-validated on a
two-STICKER bench (§14.1) — `radio-mode p2p` now routes through a real
`app_p2p`/`app_radio` facade instead of the `app_lrw.c` (#271) fallback
warning; phases 2+ (join/ACK, northbridge, central) remain unimplemented.
**Targeting v1.5.0** (#118): the unified `ats radio ...` shell surface —
covering both LoRaWAN and P2P, including the new `unjoin`/`rx1_delay`/
`ack_drop`/universal `compose` bench helpers below — ships in v1.5.0, not
v1.4.0.

---

## 1. Architecture overview

The single most important structural decision, borrowed from LoRaWAN rather
than TOWER: **gateways are stateless, keyless packet forwarders; all protocol
state lives in one central service.**

```
 STICKER (battery, STM32WLE5)          gateways (mains, FIBER v2)         central (service)
+---------------------------+       +---------------------------+      +---------------------------+
| app_p2p transport          |  RF   | STM32WL5MOC "northbridge" | LAN  | join-key registry (app)   |
| session key + counters NVS | <---> | dumb modem, continuous RX | <--> | session DB, dev_addr alloc|
| TX + RX1 ack window        |       | + RPi forwarder daemon    | MQTT | dedup, ACK routing        |
+---------------------------+       |   (no keys, no state)     |      | decrypt, northbound MQTT  |
                                    +---------------------------+      | mgmt API, pairing window  |
                                      ... N gateways, hot-add ...      +---------------------------+
```

- A **node** pairs to the *network* (identified by `net_id` = the central),
  never to an individual gateway. It does not know or care which gateway hears
  it.
- A **gateway** is a FIBER v2 class device: an STM32WL5MOC module
  (STM32WL55xx, the same SX126x-family sub-GHz radio as the STICKER's
  STM32WLE5 — fully PHY-compatible) acting as a dumb modem over UART, plus a
  thin forwarder daemon on the Raspberry Pi that bridges radio frames to
  MQTT/IP with RSSI/SNR/timestamp attached. It holds **no keys and no device
  table**; a stolen or compromised gateway leaks nothing.
- The **central** is plain software (reference deployment: the RPi of one
  "central FIBER", but it can run anywhere). It is the only component that
  holds key material: the join-key registry (populated by the phone app at
  enrollment), the session table, the frame-counter high-waters, the alias
  map. It deduplicates uplinks heard by several gateways, picks the
  best-RSSI/SNR gateway for each downlink/ACK, and republishes decoded
  payloads northbound over MQTT (reusing the existing decoder stack).
- **Multiple gateways are macro-diversity for free**: coverage is extended by
  adding gateways, devices roam between them invisibly, and no handover
  concept exists anywhere in the protocol. The only constraint is that
  gateway↔central round-trip must fit inside the RX1 window (§6), i.e. a
  LAN-grade link.

### What is inherited from TOWER, and what is deliberately not

| TOWER concept | Adopted? | Notes |
|---|---|---|
| Pairing window + peer whitelist | **Yes**, moved to the central and **bounded** (default 120 s auto-close; TOWER's window never times out) |
| Node sends ID + firmware + version in the pairing request | **Yes**, extended with `product_type` and a protocol version (TOWER lacks both) |
| Gateway returns its own ID inside the pairing ACK | **Yes**, one level up: the JoinAccept carries the *network* id, not a gateway id |
| Peer list persisted on both sides | **Yes** (node NVS / central DB); gateways stay stateless |
| Over-the-air attach/detach | **Yes**, but authenticated (TOWER's `NODE_DETACH` is unauthenticated — anyone can unpair anyone) |
| Alias layer (`<firmware>:<n>`), MQTT management surface | **Yes**, on the central (§9) |
| ACK + bounded retries with jittered backoff | **Yes**, re-sized for LoRa time-on-air and duty cycle (§6) |
| **No encryption, no authentication, no replay protection** | **No.** This is TOWER's one fatal flaw and the reason the key hierarchy in §4 exists. |

---

## 2. Selecting the transport

P2P is selected by the existing mainline parameter (#271):

```
config radio-mode p2p        # off / lorawan / p2p
settings save                # persists + reboots
```

On boot the firmware brings up **only** the selected stack — the SX126x radio
is shared between LoRaMac and raw LoRa, so there is no live switch. The
payload layer is unchanged: `app_compose` builds the same protobuf snapshots
and `app_report` owns the `interval_report` cadence for both transports,
behind an `app_radio` facade (shape carried over from PR #228; mainline
has ~40 direct `app_lrw_*` call sites that the facade must absorb).

Build: dual-stack image gated by `CONFIG_APP_LORA_P2P` (default `y` on
release; `n` on the flash-tight debug overlay). `CONFIG_LORA=y` (Zephyr raw
LoRa driver) is already present in the release configuration today, so no new
driver Kconfig is needed. Flash headroom against the current `0x34000` budget
was re-measured — see §11 — release has room, debug does not and keeps
`CONFIG_APP_LORA_P2P=n`.

---

## 3. Wire format

### 3.1 Data frame (carried over from PR #228)

```
+----------+-----------+-------------+-------------+----------------------+--------+
| net_id   | dev_addr  | frame_type  | counter     | AES-CCM ciphertext   | tag    |
| 4 B  BE  | 2 B  BE   | 1 B         | 4 B  BE     | = plaintext length   | 4 B    |
+----------+-----------+-------------+-------------+----------------------+--------+
\------------------ 11 B cleartext header (AAD) --------------/
```

- The header is cleartext so receivers can filter (`net_id`) and route
  (`dev_addr`, `frame_type`) before spending a decrypt; it is fed to AES-CCM
  as AAD, so the tag covers it.
- Payload budget: **240 B** (255 − 11 − 4); larger snapshots split across
  frames.
- AES-CCM = AES-128, 4-byte tag, via the mainline `app_ccm` primitive
  (`app_ccm_encrypt_and_tag` / `app_ccm_auth_decrypt`, RFC 3610 over the
  STM32WL hardware AES peripheral, #261) — **not** mbedTLS, which is gone from
  the build. `app_ccm`'s parameter envelope (nonce 7–13 B, even tag 4–16 B,
  AAD ≥ 1 B) covers everything this design needs.
- CCM nonce: `counter(4 BE) || dev_addr(2 BE) || frame_type(1) ||
  direction(1) || zeros(5)` (13 B). Direction: `0` uplink (device→network),
  `1` downlink (network→device) — separate keystreams by construction.
- Counter persistence: NVS reservation window (`p2pfc/base`, +256 per boot) so
  a `(key, nonce)` pair can never repeat across a crash — carried over
  unchanged from PR #228.

### 3.2 `frame_type` allocation

| Value | Direction | Meaning |
|---|---|---|
| `0x02` | up | telemetry (mirrors LoRaWAN fPort 2, body byte-identical) |
| `0x03` | up | alarm (fPort 3 mirror) |
| `0x55` | up | command response (fPort 85 mirror) |
| `0x56` | down | command (same protobuf Command shape as fPort 85's Command oneof; P2P-only surface, no real LoRaWAN fPort counterpart -- see §6) |
| `0xF0` | up | **JoinRequest** (link control, §5) |
| `0xF1` | down | **JoinAccept** |
| `0xFA` | down | **Ack** |
| `0xFD` | down | **Detach** (authenticated) |
| `0xFE` | down | **RejoinRequest** (network asks the node to rejoin) |

`0x02`, `0x03`, `0x55` mirror fPorts byte-for-byte so the off-device decoder
reuses the LoRaWAN codec (`ttn.js` via `p2p.js`); `0x56` reuses the same
protobuf shape but has no fPort of its own (§6); `0xF0`–`0xFF` is reserved
for link control and never collides with an fPort mirror.

### 3.3 Radio parameters

Join and data traffic share one well-known channel in v1: **868.1 MHz, SF10,
BW 125 kHz, CR 4/5, preamble 8, private sync word, explicit header, IQ not
inverted** (device-side defaults `p2p-frequency` / `p2p-spreading-factor` /
`p2p-tx-power` remain config parameters; BW/CR stay fixed). The JoinAccept
reserves room to assign data-channel parameters later (multi-channel is a v2
option, §11).

---

## 4. Key hierarchy & enrollment

TOWER's whitelist is the *only* authorization gate on its radio — an
eavesdropper can spoof any enrolled ID. Here every hop is keyed, and the keys
form a strict one-way hierarchy rooted in the per-unit factory secret:

| Key | Who holds it | Origin / distribution |
|---|---|---|
| `secret_key` (16 B, existing) | device, ATELOS/inventory, phone app after claim | Set at the tester; the NFC command channel key. **Never leaves the NFC/claim world — the central and gateways never see it.** |
| `p2p_join_key` (16 B, derived) | device (derives on demand), phone app (derives after claim), **central (registered by the app)** | `join_key = AES128-CMAC(secret_key, "HIO-P2P-JOIN" ‖ serial_number(4 BE))` (NIST SP 800-38B / RFC 4493) — CMAC needs only the AES-ECB forward primitive already in flash (subkey generation + one CBC-MAC pass over the single 16 B block), so this is a library/driver change, not a new crypto dependency (§11's KDF review, resolved #118 phase 2; phase 1 shipped a bare one-block AES-ECB PRF, breaking change accepted pre-ship). The app derives it after claiming the device and registers `{product_type, serial, join_key}` with the central over its management API (TLS, operator-authenticated). One-time per device lifetime. |
| `p2p_session_key` (16 B, per pairing) | device + central | Derived during the join handshake: `session_key = AES128-CMAC(join_key, 0x01 ‖ dev_nonce(4) ‖ central_nonce(4) ‖ serial(4) ‖ zeros(3))` — same CMAC construction, keyed under `join_key`. Rotates on every re-join. |

Properties this buys:

- **Compromised gateway leaks nothing** (stateless, keyless).
- **Compromised central cannot speak NFC** to any device (it knows `join_key`,
  not `secret_key`) — bounded blast radius.
- **Enrollment is one-time; pairing is repeatable without the app.** The
  device can re-derive `join_key` from `secret_key` at any moment, and the
  central keeps it registered — so any re-join (§7) is fully automatic, no
  phone, no NFC tap.
- Rotating `secret_key` (`set_secret_key`, #299) implicitly rotates
  `join_key`; the app must re-register the new derivation and the device must
  re-join (§7).
- There is **no `p2p_key` config parameter** any more (PR #228 had one). Link
  identity (`net_id`, `dev_addr`) and keys are assigned/derived, not
  hand-typed — the whole "operator must configure matching values on both
  ends" model from the epic is gone.

---

## 5. Pairing (on-air join)

Modeled on TOWER's pairing-request/ACK exchange, hardened to OTAA-grade.

### 5.1 Preconditions

1. Device enrolled at the central (its `join_key` registered by the app, §4).
2. Central pairing window **open** — bounded, default 120 s, auto-close,
   opened via the management API/MQTT (§9). Gateways forward JoinRequests at
   all times; the *central* ignores them outside the window (except automatic
   re-joins of already-known devices, §7, which are accepted any time).

### 5.2 Join triggers (node side)

Deliberately narrower than TOWER's magnet/button gesture: **only** an
explicit NFC command, or a bounded window right after boot — no
gesture-based trigger (same reasoning as #252's removal of the boot-time
dual-magnet calibration entry: a magnet is an easy *accidental* trigger, and
here it would also mean an unbounded radio-retry loop for a device that
never finds a gateway, see below).

- NFC command `p2p_join` (staged like any command — works battery-off, applied
  at next boot). Battery-cheapest and timing-safest path: the join burst is a
  single bounded attempt at the node's own next boot, so there is no window to
  miss.
- Automatically at boot when `radio-mode p2p` and no valid pairing state in
  NVS, but **only for 120 s after boot** (mirrors the central's own pairing
  window default, §5.1) — after that the node stops retrying entirely and
  goes back to sleep/idle until the next boot or an NFC `p2p_join`. Without
  this cap, a device that is provisioned but never brought within range of an
  open pairing window (in transit, in a warehouse, deployed before the
  central side is set up) would otherwise keep transmitting JoinRequest +
  listening RX1 on backoff for its *entire remaining battery life* — the
  120 s window turns an unbounded worst-case drain into a fixed, small,
  once-per-boot cost. (No HW power numbers yet — see §13 phase 6, "power
  delta vs. the 92 µA baseline".)
- Automatically as self-healing after persistent ACK loss (§7) — unaffected
  by the above: that is a *re-join* of an already-paired device recovering
  from lost sync, already bounded by "N consecutive failed uplink cycles"
  (default 8), not the open-ended never-paired case this cap targets.

### 5.3 Handshake

**JoinRequest** (`frame_type 0xF0`, uplink):

```
header:  net_id = 0, dev_addr = 0, counter = dev_nonce
body:    product_type(1) | proto_version(1) | serial_number(4 BE) | fw_version(4)
tag:     CCM tag under join_key (body sent as AAD, empty plaintext)
```

- `serial_number` must be cleartext — it is the central's lookup handle into
  the join-key registry.
- `dev_nonce` is a **monotonic persisted counter** in NVS (LoRaWAN 1.0.4
  style), giving JoinRequest replay protection: the central stores the last
  seen `dev_nonce` per device and rejects non-increasing values. That accept
  rule must itself be **bounded, not a bare `>`** — same fix as #266/PR #268
  on the NFC command channel's `nonce_counter` (`NFC_NONCE_MAX_SKIP`, capped
  to `(current, current+1024]`): an unbounded jump lets a single forged or
  buggy JoinRequest (still requires `join_key`, so a compromised/misbehaving
  enrollment tool rather than an outside attacker) push the central's stored
  high-water to near `UINT32_MAX`, after which no legitimate `dev_nonce` can
  ever be `>` it again — and because `join_key` doesn't change on
  `factory_reset` (§4, §7), this is a *permanent* per-device join lock the
  device itself cannot recover from; only manual central-side DB surgery
  fixes it. Apply the same capped-skip window here.
- `product_type` + `fw_version` mirror TOWER's "firmware name + version in the
  pairing request" so the central can auto-name and manage the node with zero
  configuration (§9), and make the join generic across future products
  (identity envelope per `claiming_process.md` §11).
  - **`product_type` registry** (no central schema exists yet, so this is a
    placeholder pending one, #118 phase 2 HIL): `1` = STICKER, the only value
    in use today (`P2P_PRODUCT_TYPE_STICKER`, app_p2p.c). HW-confirmed on the
    wire (decoded correctly by an independent central-side decoder).
  - `fw_version(4)` is packed as `fw_major(1) | fw_minor(1) | fw_patch(1) |
    reserved(1)`, matching the existing `Info` command's version fields
    (app_cmd.c) — not itself broken down further above since it is a plain
    byte layout, not a registry. HW-confirmed (`1.4.0` decoded correctly).

**JoinAccept** (`frame_type 0xF1`, downlink, sent in the node's RX1 window):

```
header:  net_id = 0, dev_addr = 0, counter = dev_nonce (echo)
body:    net_id(4) | dev_addr(2) | central_nonce(4) | rx1_delay(1) | reserved(4)
crypto:  AES-CCM under join_key (encrypted + tagged; nonce bound to dev_nonce,
         direction = 1)
```

- The central allocates `dev_addr` (unique per network — it is the global
  allocator, so cross-gateway collisions cannot happen) and answers through
  the gateway that heard the JoinRequest best.
- Both sides derive `session_key` (§4) and persist: node → NVS (`net_id`,
  `dev_addr`, `session_key`, `rx1_delay`, counter bases), central → session
  DB. Gateways persist nothing. `rx1_delay` must be persisted alongside the
  rest — a re-join isn't triggered by every reboot (§7: the session survives
  RF outages and normal power cycles), so a node computing RX1 timing from a
  forgotten/default value after reboot would drift from what the central
  actually assigned.
- The `reserved` field is the v2 hook for assigning data-channel radio
  parameters (§11).

A failed attempt (no JoinAccept) retries with jittered, duty-cycle-aware
backoff — but only for the trigger's own bounded lifetime: within the 120 s
boot window (§5.2) for an auto-boot join, or a single attempt for an
NFC-staged `p2p_join` (no standing retry loop once that boot's window/attempt
is spent — the node goes back to idle and waits for the next trigger).

### 5.4 Detach

`Detach` (`0xFD`) is a downlink authenticated under the session key; the node
wipes its pairing state and returns to the unpaired boot behavior. The
management API's `nodes/remove` issues it (best-effort — a sleeping node picks
it up in its next RX1, exactly TOWER's caveat). TOWER's unauthenticated
detach-by-anyone is explicitly not reproduced.

---

## 6. Data plane — acknowledged uplinks

v1 is **confirmed-uplink**: after every data TX the node opens one RX window
(Class-A pattern).

- **RX1**: opens `rx1_delay` seconds (default 1 s, assigned in JoinAccept)
  after uplink end, same channel/SF. The ACK (`0xFA`) is a header + a 1-byte
  flags body (bit 0: downlink pending) + tag under the session key, `counter`
  echoing the uplink counter. At SF10/125 kHz an ACK is ≈ 250 ms
  time-on-air — the gateway→central→gateway decision must fit inside the
  ~1 s budget, hence the LAN requirement.
- **Retries**: unacknowledged uplinks retransmit **the same counter value**
  (byte-identical frame) up to 3 times with randomized backoff. The central
  treats `counter == high-water` as a duplicate: re-ACK, don't re-process.
  Retry/timeout budgets must be sized against the 1 % duty cycle on *both*
  ends — see the GLOBECOM 2017 finding in §10 (100 % ACK traffic collapses
  goodput); the ACK's small size and the central's per-gateway duty-cycle
  accounting are what keep this affordable.
- **Anti-replay**: the central keeps a per-device counter high-water;
  anything ≤ high-water (except the retransmission case above) is dropped.
  This is the replay protection TOWER never had. Same capped-skip window as
  the JoinRequest `dev_nonce` (§5.3) applies here too, for consistency — lower
  blast radius (`session_key` rotates on every re-join, so a jammed high-water
  self-heals via `Detach`/`RejoinRequest` rather than needing central DB
  surgery), but no reason to leave a second unbounded `>` check in the same
  design.
- **Downlink commands** (`0x56`): flagged in the ACK, delivered in the RX1
  window of the *next* uplink (Class-A downlink queue on the central). Same
  protobuf Command shape as LoRaWAN fPort 85's Command oneof; `frame_type
  0x56` is a P2P-only surface with no real LoRaWAN fPort counterpart (the
  directional `0x55`/`0x56` split exists because P2P lacks ChirpStack's
  topic-based direction separation) -- `app_cmd` ingest is still
  transport-agnostic, just not a literal fPort mirror on the downlink side.
- **Dedup across gateways**: every gateway that hears a frame forwards it;
  the central keys dedup on `(dev_addr, counter)` and records per-gateway
  RSSI/SNR (which also feeds ACK routing).
- Power impact: one RX1 per report against the 92 µA idle baseline
  (`doc/power-consumption.md`) must be measured on HW; the RX window is the
  main new consumer vs. the TX-only draft.
- **Bench testing (v1.5.0)**: `ats radio ack_drop <count>` (CONFIG_SHELL) makes the
  next `count` Acks appear dropped without a real RF outage, exercising the
  retry/backoff path above deterministically -- the P2P analogue of `ats
  radio lc fail` for the LoRaWAN link-check FSM.

---

## 7. Lifecycle: link loss, re-join, resets

**RF outage never triggers a re-join.** The session is persisted on both
sides; a node that loses coverage keeps transmitting on schedule (retries,
then gives up per-uplink), and any gateway that hears it again resumes the
session untouched. Data heard by *any* gateway during the outage still flows.

Re-join happens only on genuine state loss, and — because the node re-derives
`join_key` from `secret_key` on demand and the central keeps it registered —
it is always automatic, requiring no app interaction:

| Trigger | Behavior |
|---|---|
| `secret_key` rotation (#299) | `join_key` changes ⇒ app re-registers the new derivation with the central, node re-joins on its next boot (rotation already forces a reboot, #322). |
| `factory_reset` | **Does NOT clear P2P pairing** (doc/code mismatch found 2026-08-24: the `p2pjoin/*` settings subtree is registered entirely inside `app_p2p.c` and is never referenced by `app_settings_factory_reset()`, unlike the LoRaWAN NVM it was assumed to mirror). The debug shell's `ats radio join` (v1.5.0) forces a fresh join live, no reboot needed (mirrors LoRaWAN's `join`, which always rejoins unconditionally); `ats radio unjoin` (v1.5.0; clears `p2pjoin/state`, leaves the `dev_nonce` anti-replay counter untouched, reboot required) additionally simulates a cold, never-paired boot. Otherwise only a whole-NVS `settings erase` clears it. |
| Central DB loss/restore | Node's uplinks stop being ACKed (or ACK under an unknown session fails MIC). Self-healing: after **N consecutive fully-failed uplink cycles** (default 8) the node starts re-join attempts with exponential backoff. Known devices' re-joins are accepted outside the pairing window. |
| Explicit `Detach` / `RejoinRequest` downlink | Authenticated; immediate. `RejoinRequest` is the network-initiated rekey lever (counter hygiene, key rotation policy). |
| Counter approaching 32-bit wrap | Practically unreachable; policy is a network-initiated `RejoinRequest` rekey long before wrap. |

---

## 8. Gateway & central (FIBER v2 pattern)

The FIBER v2 (`proximos` / `fiber-northbridge`) split is adopted as-is and
extended one level:

- **Northbridge (STM32WL5MOC)**: dumb modem. The existing CRC16-framed UART
  protocol (`TX_PACKET` / `SET_CONFIG` / `ENTER_RX` / `GET_STATUS`) gains an
  RX-stream message carrying `frame ‖ RSSI ‖ SNR ‖ timestamp`, and a
  scheduled-TX message (`transmit frame F at time T`) for hitting RX1
  windows. Continuous RX is already its default posture. No crypto, no
  addressing logic on the modem — matching its current design philosophy.
- **Forwarder daemon (RPi)**: a thin radio↔MQTT bridge. Subscribes
  `p2p/gw/<gw_id>/tx`, publishes `p2p/gw/<gw_id>/rx`. Stateless; a gateway is
  fully described by its MQTT identity.
- **Central service**: join-key registry + enrollment API (`POST` of
  `{product_type, serial, join_key}` from the phone app; TLS + operator
  auth), session DB, `dev_addr` allocation, pairing window state, dedup, ACK
  routing (best RSSI/SNR gateway, per-gateway duty-cycle bookkeeping),
  payload decrypt + decode (reusing `p2p.js`/`ttn.js`), northbound publish
  `p2p/net/<net_id>/device/<serial-or-alias>/...` — the same back-office
  shape the existing sticker-monitor consumes.

Scaling: 1 central + N gateways; gateways hot-add with zero device-side or
protocol-level ceremony (they are just more ears and mouths for the same
central).

---

## 9. Management surface (TOWER-inherited)

The TOWER gateway's MQTT surface maps almost 1:1 onto the central:

| Operation | TOWER equivalent | Notes |
|---|---|---|
| `pairing-mode/start` / `stop` | same | bounded window (120 s auto-close), state on the central |
| `nodes/list` | `/nodes/get` | serial, alias, product, fw version, last seen, per-gateway RSSI |
| `nodes/remove` | `/nodes/remove` + `NODE_DETACH` | issues the authenticated `Detach` downlink |
| `nodes/purge` | same | |
| `alias/set` / `alias/remove` | same | auto-assigned `<product-or-fw>:<n>` on first join, TOWER-style; aliases live in the central DB, northbound topics subscribe both id and alias |
| `scan/start` / `stop` | same | passive: report unknown `serial`/`dev_addr` traffic without enrolling |

(Manual `nodes/add` from TOWER is intentionally absent — enrollment goes
through the app + join-key registration, never by unauthenticated fiat.)

---

## 10. End-to-end enrollment flow

```
 tester            ATELOS/inventory        phone app                central              device
   |  secret_key,        |                     |                       |                    |
   |  claim_token  ----> |                     |                       |                    |
   |                     |   claim (NFC clm    |                       |                    |
   |                     | <-- record + token) |                       |                    |
   |                     |  secret_key ------> |                       |                    |
   |                     |                     | derive join_key       |                    |
   |                     |                     | register {product,    |                    |
   |                     |                     |  serial, join_key} -> |                    |
   |                     |                     |                       |  pairing window    |
   |                     |                     |                       | <--- JoinRequest --|
   |                     |                     |                       | ---- JoinAccept -->|
   |                     |                     |                       |   session up       |
   |                     |                     |                       | <== data + ACKs ==>|
```

Steps 1–3 are the **existing, shipped** claiming flow (`hio.stck:clm`,
`claiming_process.md`) — nothing new. Step 4 (derive + register) is the only
new app-side obligation, one-time per device. Step 5+ never needs the app
again.

---

## 11. Open questions

- **KDF construction review** — **Resolved (#118 phase 2):** switched `§4`'s
  `join_key`/`session_key` derivation from the bare one-block AES-ECB PRF to
  AES-CMAC (NIST SP 800-38B / RFC 4493), still built on only the AES-ECB
  forward primitive already in flash (`app_ccm_cmac()`), so no new crypto
  backend. Breaking change vs. phase 1's `join_key`, accepted pre-ship (phase
  1 was bench-only, no central to migrate).
- **ACK economics at scale** — at what node count per network does confirmed-
  uplink eat the gateways' 1 % duty cycle (§6/§10)? Options if it binds:
  ACK-every-Nth, or sink-driven TDMA (Piyare et al., §10 related work).
- **Multi-channel** — should JoinAccept's reserved field assign per-device
  data channels (frequency/SF) to spread load? v1 says single shared channel.
  **Resolved: yes** — keep building for it. The `reserved` field (§5.3) stays
  the assignment hook; §13's v1/v2 split is unaffected (single shared channel
  ships first, per-device assignment is the v2 consumer of a field v1 already
  reserves), this just confirms the direction is worth the reserved bytes.
- **Multiple *networks* in RF range** — `net_id` filtering handles it, but the
  join channel is shared; JoinRequests are answered only by the central that
  knows the serial, which resolves ownership implicitly. Confirm this is
  acceptable (a device enrolled at two centrals joins whichever answers
  first). **Resolved: yes** — accepted as designed, no change needed.
- **Northbridge scheduled-TX precision** — RX1 hit accuracy over
  UART+MQTT+LAN needs a prototype measurement before freezing `rx1_delay`
  at 1 s. **Resolved + implemented (v1.5.0):** `rx1_delay` is already per-device and
  central-assigned (JoinAccept, §5.3), but that alone means changing it
  needs a live central + a registered device — too slow for bench tuning.
  `ats radio rx1_delay <seconds>` (CONFIG_SHELL-gated, same idiom as the
  existing `ats radio lc ...` debug helper) overrides the live value
  in-RAM only — not persisted, restored by the next real JoinAccept — so it
  can be swept without a full re-join while measuring northbridge
  scheduled-TX precision during phase 3 (§13).
- **Flash budget** — re-measure the dual-stack image against `0x34000`;
  decide whether debug keeps `CONFIG_APP_LORA_P2P=n`. **Measured 2026-08-17**
  (current `v1.4.0` tip, no P2P code yet — this is the baseline the dual-stack
  addition must fit into): release `153116 B / 212992 B (0x34000) = 71.89%`,
  **~58.5 KB (28.1 %) free**; debug `240900 B / 245760 B = 98.02%`, only
  **~4.75 KB (1.98 %) free**. Release has comfortable headroom; debug does
  not — confirms debug keeps `CONFIG_APP_LORA_P2P=n` (same lever already used
  to drop AU915/US915 from debug.conf, see the debug build RAM+flash coupling
  fix from issue #340) unless debug frees up budget elsewhere first.
- **Device-side duty-cycle enforcement** — §6/§8 only describe duty-cycle
  *bookkeeping* on the central/gateway side; LoRaWAN's own EU868 1 % limit is
  enforced inside LoRaMac today, which P2P mode bypasses entirely (raw LoRa
  driver, no network server, §1). Nothing in this design yet gives the node
  itself an equivalent tracker for its own uplink/retry/re-join traffic.
  **Deferred to phase 2** (§13, FW join + ACK state machine) — must land
  before this ships, not before the design is accepted.
- **RX2-equivalent downlink fallback** — v1 is RX1-only (§6): a missed
  ~1 s gateway↔central round trip (LAN/MQTT jitter) just counts as an
  unacknowledged uplink and falls back to the normal retry path, unlike
  LoRaWAN Class-A's RX1+RX2 pair. **Deferred to a later phase** — ship
  RX1-only for v1 (already implied by §13's Class-A-equivalent scope) and
  revisit only if phase 6 HW validation shows LAN-jitter-driven RX1 misses
  are frequent enough to matter.

---

## 12. Related work — how other LoRa P2P implementations compare

A literature/industry review (Jul 2026) looked at how other raw-LoRa P2P
designs handle the same problem — a battery-powered, duty-cycled TX node
talking to an always-on, mains-powered receiver — to sanity-check the
choices above. **Caveat up front:** public documentation from named
commercial asset trackers (Digital Matter, Abeeway, Sensoneo, Milesight,
Dragino, Browan, Adeunis) does not go into enough protocol detail to confirm
how their non-LoRaWAN P2P modes actually schedule TX, ACK, or encrypt — the
evidence below is mostly academic MAC-protocol research plus one open-source
raw-radio stack (RadioHead), not confirmed vendor internals.

**Power / duty-cycle scheduling.** A fire-and-forget scheme is, in MAC
terms, pure (unslotted) ALOHA, which has a hard theoretical efficiency
ceiling of **18.4 %** (vs. 36.8 % for slotted ALOHA) — a ceiling that even
standard LoRaWAN uplink inherits, since LoRaWAN's own MAC is Pure-ALOHA too
([Optimizing Pure ALOHA for LoRa-Based ESL](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC8348389/);
[LoRaWAN CARA](https://pmc.ncbi.nlm.nih.gov/articles/PMC7915080/)). One
academic design inverts our model instead of tuning it: the **always-on
sink initiates** data collection with a wake-up beacon rather than nodes
pushing unprompted, and paired with TDMA scheduling reached **100 % packet
delivery** vs. 83–91 % for carrier-sense/Listen-Before-Talk under the same
load ([Piyare et al., 2018](https://arxiv.org/pdf/1809.04142)) — directly
relevant to our sticker→receiver topology, since the receiver side already
has the power budget to listen continuously or drive a schedule.

**Reliability / ACK / retry.** The open-source RadioHead stack shows a
clean way to add reliability without adopting a LoRaWAN-style MAC: the base
raw-radio driver sends unaddressed, unacknowledged datagrams, and a
separate optional layer (`RHReliableDatagram`) adds addressing + auto-ACK +
bounded retransmission (3 retries, ~200 ms timeout with jitter) on top
([RadioHead docs](https://www.airspayce.com/mikem/arduino/RadioHead/classRHReliableDatagram.html)) —
architecturally compatible with our 11-byte header without a frame-format
redesign. ACK is not a free win, though: a LoRaWAN simulation study found
that when 100 % of uplinks request an ACK, network goodput collapses to
roughly **15 % of the no-ACK baseline** under EU868 duty-cycle limits
([Van den Abeele et al., IEEE GLOBECOM 2017](https://arxiv.org/pdf/1704.04174)) —
this design adopts confirmed uplink anyway (§6) because STICKER report
cadences are minutes-scale, not seconds-scale, but the retry/timeout budget
is sized against the 1 % duty-cycle allowance for exactly this reason.

**Security.** The only encryption-architecture claim that held up under
verification is again from RadioHead: `RHEncryptedDriver` wraps *any*
transport driver with a pluggable cipher (AES or the lightweight ASCON)
at the transport layer, rather than the cipher being baked into a fixed
frame format the way our AES-CCM header/body/tag layout is
([RadioHead docs](http://www.airspayce.com/mikem/arduino/RadioHead/)). No
confirmed detail on named competitors' key provisioning or rolling-code
schemes surfaced.

**Topology / scaling.** In place of our simple `net_id`/`dev_addr` header
filtering, published multi-node schemes without a central network server
include reservation-based MACs (a short RTS control packet sent before
data, decoded only by same-channel neighbours who then back off) and CANL,
an asynchronous neighbour-listening collision-avoidance scheme that
avoids the known unreliability of classic Channel Activity Detection
([RTS scheme, MDPI Sensors 2022](https://www.mdpi.com/1424-8220/22/14/5260);
[CANL](https://www.researchgate.net/publication/373472490_CANL_LoRa_Collision_Avoidance_by_Neighbor_Listening_for_Dense_LoRa_Networks)).
Quantified benefits (PDR improvement, max nodes per gateway) for either
scheme did not survive adversarial re-checking, so treat the mechanisms as
real design options and the specific numbers as unconfirmed.

**Internal reference — HARDWARIO FIBER v2 (`proximos`).** The `proximos`
GitLab group (renamed from `fiber-v2`) hosts the in-house concentrator this
design's gateway is based on: a mains-powered STM32WL5MOC radio MCU
(`fiber-northbridge`) paired with a Raspberry Pi, plus a Python test tool
(`tools/mesh_node.py`) that talks to it over UART:

- **Radio MCU is a dumb modem, not a protocol stack.** `fiber-northbridge`
  (bare-metal STM32 HAL) only exposes `TX_PACKET` / `SET_CONFIG` /
  `ENTER_RX` / `GET_STATUS` over a CRC16-framed UART protocol; addressing,
  ACK and crypto live host-side. This design keeps that split for the
  gateway and moves the host-side logic one level further up, into the
  central (§8).
- **Its 9-byte mesh header** (`version | pkt_type | src_id(2) | dst_id(2) |
  seq(2) | ttl(1)`, with a `PKT_ACK` type) is a placeholder for a future
  mesh, with no retry and no forwarding logic implemented — this design's
  ACK layer (§6) is specified where FIBER's is aspirational.
- **No encryption at all** in the FIBER prototype (plain UTF-8 payloads);
  §4 is strictly ahead of it.
- **Confirms the always-on continuous-RX concentrator pattern** and the
  self-enforced duty-cycle approach; its default radio parameters
  (SF7/BW125/CR4-5, 14 dBm, private sync word, 869.525 MHz) differ only in
  frequency and SF from §3.3.

---

## 13. Phasing & limitations

Deliberately out of scope for v1:

- **No mesh / multi-hop** — star topology only (gateways are not repeaters).
- **No history replay / link check / clock sync** over P2P (LoRaWAN-specific
  today; clock sync over P2P is a natural v2 since the central owns time).
- **BW / CR fixed** at 125 kHz / 4-5.
- **Single shared channel** (multi-channel via JoinAccept reserved field is
  the v2 hook).
- Downlink to a node exists **only** in its post-uplink RX1 window (Class-A
  equivalent); no Class-C-style always-listening node mode.

Implementation phasing (each independently reviewable):

1. **FW transport port** — re-implement PR #228's TX path on mainline
   (`radio_mode`, `app_ccm`, `app_radio` facade), still unpaired/unACKed
   behind a build flag; salvage `app_p2p.c` logic, `p2p.js` + tests. Bench
   HIL from day one via the two-STICKER rig (§14) — no need to wait for
   phase 3/4's real gateway/central to start validating the wire format.
2. **FW join + ACK state machine** — §5–§7 (RX1, retries, NVS pairing state,
   join triggers, self-healing rejoin), including the device-side EU868
   duty-cycle tracker (§11) LoRaMac would otherwise have provided for free.
   Also HIL-testable on the two-STICKER rig (§14): join handshake, ACK/retry,
   anti-replay.
3. **Northbridge RX-stream + scheduled TX** — UART protocol extension.
4. **Central service** — enrollment API, session DB, dedup/ACK routing,
   management MQTT, northbound decode.
5. **App-side** — join-key derivation + central registration (Manager-App
   coupling issue to be filed when phase 4 starts).
6. **HW validation** — range, RX1 timing, power delta vs. the 92 µA baseline,
   multi-gateway dedup/roaming. This is where the two-STICKER rig's
   optimistic RX1 timing must be re-validated against the real
   northbridge+RPi+central round trip (§14).

---

## 14. Bench validation strategy: two-STICKER rig (pre-hardware)

FIBER v2 gateway hardware and the central service don't exist yet for this
project, and phases 3–5 (§13) build them. That would otherwise block *any*
HIL testing of phases 1–2's firmware work (wire format, crypto, join/ACK
state machine) until they land. It doesn't have to: the gateway's
STM32WL5MOC (STM32WL55xx) and the STICKER's STM32WLE5 are the **same SX126x
sub-GHz radio family, fully PHY-compatible** (§1) — a second STICKER can
stand in for "the network side" on the radio link well enough to validate
everything except real round-trip timing.

### Setup

- **Device A ("node" / DUT)** — mainline firmware, `radio-mode p2p`, behaves
  exactly like a deployed unit.
- **Device B ("gw-sim")** — **a separate, standalone firmware**
  (`sticker/tests/p2p/`, `west build -b sticker tests/p2p`), *not* a debug
  build of the mainline app with extra `ats p2p ...` shell commands as
  originally planned here. That plan turned out to be impractical in
  practice: mainline `debug.conf` already sits at ~99%+ RAM even before
  adding P2P (§11, and see "First HIL session" below — this margin turned out to
  be *unsafe*, not just tight), so bolting a second debug-shell
  surface onto it wasn't viable. The standalone firmware shares only the
  crypto primitive (`app_ccm.c`, via a `KCONFIG_ROOT` pointing at the main
  app's `Kconfig` — same idiom as `tests/nfc_hw`) and reimplements the wire
  format locally (kept in sync by hand, documented in its own README) — no
  sensors/NFC/LoRaWAN/history/protobuf, ~24% flash/RAM instead of fighting
  for the last few hundred bytes. Its RTT shell:
  - `p2p listen on|off` — continuous RX on the P2P channel params (§3.3),
    decrypts and logs every received frame (raw hex + parsed
    `net_id`/`dev_addr`/`frame_type`/`counter` header + RSSI/SNR + decrypted
    body).
  - `p2p tx <frame_type> <hex_body> [counter] [dir]` — encrypts and
    transmits a hand-built frame, so a human (or a bench script) can inject
    a JoinAccept/Ack/Detach/RejoinRequest in response to what `listen` just
    logged, without a real central, once phase 2 lands.
  - `p2p key <secret_key> <serial_number>` — derives `join_key` (§4) to
    match device A's identity; `p2p radio`/`p2p status` for the rest.
    Nothing persists across reboot — re-enter every bench session.
- Both devices need independent J-Link/RTT access (two probes) — same bench
  pattern as any other two-unit HIL session. See "First HIL session" below
  for what a first real session on this rig found.

### What this validates

- Wire format (§3): header framing, AES-CCM tag verification, payload
  budget.
- `dev_nonce`/uplink-counter anti-replay and its capped-skip window (§5.3,
  §6).
- JoinRequest/JoinAccept handshake (§5.3) and `session_key` derivation
  matching on both sides.
- ACK (§6) and the retry/backoff state machine, including the 120 s
  boot-window / NFC-`p2p_join` trigger cap (§5.2).
- `Detach`/`RejoinRequest` (§5.4, §7).

Test frames for device B's `p2p tx` (JoinAccept, Ack, etc., once phase 2
lands) can be hand-crafted offline the same way `sticker_nfc_frame.py`
hand-crafts NFC AES-CCM frames for phone-free NFC testing — same primitive
(`app_ccm_encrypt_and_tag`/`app_ccm_auth_decrypt`), different frame layout.

### First HIL session (2026-08-18)

First run of this rig, against the phase-1 transport-port implementation
(#118). DUT = STICKER 2162199999 (`radio-mode p2p`, `secret_key`
`dddd...dddd`), gw-sim = a second STICKER on `tests/p2p`. Findings:

**Wire protocol: validated.** A DUT `send` (RTT shell) produced a frame the
gw-sim correctly received and decrypted end-to-end on real silicon (not
just the native_sim ztest suite):

```
RX 44 B, RSSI -23 dBm, SNR 9 dB
net_id=0 dev_addr=0 frame_type=3 counter=257
body (29 B): 01088af490d40610021a07080238ff0148041a07080338ff0148042001
```

`frame_type=3` is **alarm** (§3.2, fPort 3 mirror) — the DUT had an active
`alarm-no-data` condition, so the transport-ready callback's first report
was an alarm frame, not telemetry (`0x02`). This is a useful confirmation
in itself: it means `app_radio_send_alarm()` and
`app_radio_send_telemetry()` are *both* independently exercised by
ordinary device state, not just the explicit `send` command. Radio config
(868.1 MHz/SF10/BW125/CR4-5), the 11 B AAD header, and the `join_key` KDF
all matched between the two independently-flashed devices, confirming §3–§4
end to end.

**Frame-counter persistence: confirmed.** The counter kept climbing across
multiple DUT resets (crash-forced and deliberate) instead of resetting to
0 — consistent with the NVS reservation-window design (§3.1, `p2pfc/base`,
+256 per boot). The visible jumps between reads (e.g. 257 → 7453 → 7710 →
8224 → 8481 → 8738, deltas clustering around 257/514) are exactly what a
+256-per-boot reservation predicts across several reboots between
observations — not a bug, just the reservation window being consumed by
reboot count rather than frame count.

**Duty-cycle enforcement: inconclusive on this bench, code looks correct.**
Tried to force back-to-back `send`s to observe the `-EAGAIN`/block-until
gate (§3.1, `app_p2p.c`'s 1‰-configured EU868 duty-cycle math — at SF10 a
44 B wire frame airtime is ≈535 ms, so the block window is
≈53 s, not the ≈25 s a rougher airtime guess would suggest — **recompute
this from `frame_toa_ms()`, don't estimate**, next time). Never got a clean
isolated repro of "duty-cycle blocked an otherwise-pending frame": once the
DUT's reportable state (alarm/telemetry deltas) settled, `app_compose_budget`
legitimately had nothing new to send, which is silent at the same log level
as the duty-cycle gate itself (both effectively invisible without a
temporary `LOG_WRN` — the existing `LOG_WRN("TX duty-cycle blocked...")` at
`app_p2p.c` line ~316 is real and did not fire during this session, which is
consistent with "nothing to send" rather than "actively blocked", but the
two are hard to tell apart from outside without forcing a genuine sensor
delta — a hall/input toggle or similar physical stimulus this session
couldn't provide unattended). Re-run with a way to force fresh reportable
data (physical sensor trigger, or a temporary shell command that force-marks
a report-worthy delta) to get a clean before/after duty-cycle trace.

**gw-sim firmware bugs found + fixed** (both real stack overflows on real
HW, both fixed by moving large scratch buffers to `static` plus a modest
explicit stack-size bump — see `tests/p2p/prj.conf`'s comments for the
exact sizing):
- `rx_work_handler()` (system work queue, default 1024 B stack) overflowed
  decrypting the very first real RX frame — >1.5 KB of stack-local scratch
  (hex dump + plaintext + body-hex buffers) plus AES-CCM call depth on a
  1024 B stack. Fixed: buffers moved to `static`,
  `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048`.
- `cmd_p2p_tx()` (shell's own thread, default 2048 B stack) overflowed the
  first real `p2p tx` call — same class of bug, smaller margin violation.
  Fixed the same way, `CONFIG_SHELL_STACK_SIZE=3072`.

**Separate, pre-existing bug found — NOT specific to P2P, filed as
issue #394.** While chasing what looked like TX silence, the DUT's shell
went completely unresponsive a few seconds after boot. Turned out to be a
reproducible BusFault/UsageFault from the *mainline* `debug.conf`'s system
heap writing past the physical end of RAM (`0x20010000` on this 64 KB
part) — reproduced identically with `CONFIG_APP_LORA_P2P` entirely
disabled, so this is a `debug.conf` RAM-budget problem, not a P2P bug. The
bench overlay (`app/debug_p2p_bench.conf`) has its own local fix (trimmed
history/log/RTT buffers + `CONFIG_MAIN_STACK_SIZE`, verified via
`k_thread_stack_space_get()` to leave >2 KB of the 3 KB main stack free on
real HW) so P2P bench sessions aren't blocked by it, but the underlying
`debug.conf` issue is unresolved upstream — see issue #394 before relying
on any other debug-build HIL session's RAM margin at face value.

### What this does NOT validate — still needs real FIBER v2 hardware (phase 6)

- **RX1 timing margin.** Two STICKERs sitting next to each other respond
  far faster than the real gateway→central→gateway round trip over
  UART+MQTT+LAN (§6, ~1 s budget) — this rig's timing is optimistic and must
  not be mistaken for a validated margin.
- **Multi-gateway dedup, central DB, MQTT management surface** (§8/§9) —
  there is no second radio "gateway" role in this rig, and no central
  service at all.
- **Northbridge UART protocol** (§8) — irrelevant here; device B is a
  STICKER, not a STM32WL5MOC northbridge.

### Turning this into a tracked test plan

Once phase 1–2 code actually lands, the scenarios above become concrete,
numbered HIL test cases in `doc/manual-test-plan.md` (same X1-style format
as the PR #358 regression scenarios) rather than the prose method described
here.
