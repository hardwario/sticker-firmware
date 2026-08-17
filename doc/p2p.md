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

Status: **design document**. No firmware/gateway code implements this yet;
mainline `radio-mode p2p` is reserved and falls back to radio-off with a
warning (`app_lrw.c`, #271).

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
behind an `app_transport` facade (shape carried over from PR #228; mainline
has ~40 direct `app_lrw_*` call sites that the facade must absorb).

Build: dual-stack image gated by `CONFIG_APP_LORA_P2P` (default `y` on
release; `n` on the flash-tight debug overlay). `CONFIG_LORA=y` (Zephyr raw
LoRa driver) is already present in the release configuration today, so no new
driver Kconfig is needed. Flash headroom against the current `0x34000` budget
must be re-measured before implementation — PR #228 fought for every byte
against the old 160 KB budget.

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
| `0x56` | down | command (fPort 86 mirror) |
| `0xF0` | up | **JoinRequest** (link control, §5) |
| `0xF1` | down | **JoinAccept** |
| `0xFA` | down | **Ack** |
| `0xFD` | down | **Detach** (authenticated) |
| `0xFE` | down | **RejoinRequest** (network asks the node to rejoin) |

`0x02`–`0x56` mirror fPorts so the off-device decoder reuses the LoRaWAN codec
(`ttn.js` via `p2p.js`); `0xF0`–`0xFF` is reserved for link control and never
collides with an fPort mirror.

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
| `p2p_join_key` (16 B, derived) | device (derives on demand), phone app (derives after claim), **central (registered by the app)** | `join_key = AES128-ECB(secret_key, "HIO-P2P-JOIN" ‖ serial_number(4 BE))` — a one-block PRF over the hardware AES already in flash. The app derives it after claiming the device and registers `{product_type, serial, join_key}` with the central over its management API (TLS, operator-authenticated). One-time per device lifetime. |
| `p2p_session_key` (16 B, per pairing) | device + central | Derived during the join handshake: `session_key = AES128-ECB(join_key, 0x01 ‖ dev_nonce(4) ‖ central_nonce(4) ‖ serial(4) ‖ zeros(3))`. Rotates on every re-join. |

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

- Magnet hold (the TOWER "pairing button" equivalent),
- NFC command `p2p_join` (staged like any command — works battery-off, applied
  at next boot),
- automatically at boot when `radio-mode p2p` and no valid pairing state in
  NVS,
- automatically as self-healing after persistent ACK loss (§7).

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
  seen `dev_nonce` per device and rejects non-increasing values.
- `product_type` + `fw_version` mirror TOWER's "firmware name + version in the
  pairing request" so the central can auto-name and manage the node with zero
  configuration (§9), and make the join generic across future products
  (identity envelope per `claiming_process.md` §11).

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
  `dev_addr`, `session_key`, counter bases), central → session DB. Gateways
  persist nothing.
- The `reserved` field is the v2 hook for assigning data-channel radio
  parameters (§11).

A failed window (no JoinAccept) retries with jittered exponential backoff,
duty-cycle-aware.

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
  This is the replay protection TOWER never had.
- **Downlink commands** (`0x56`): flagged in the ACK, delivered in the RX1
  window of the *next* uplink (Class-A downlink queue on the central). Same
  body format as LoRaWAN fPort 86, so `app_cmd` ingest is transport-agnostic.
- **Dedup across gateways**: every gateway that hears a frame forwards it;
  the central keys dedup on `(dev_addr, counter)` and records per-gateway
  RSSI/SNR (which also feeds ACK routing).
- Power impact: one RX1 per report against the 92 µA idle baseline
  (`doc/power-consumption.md`) must be measured on HW; the RX window is the
  main new consumer vs. the TX-only draft.

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
| `factory_reset` | Pairing state is wiped (`persistent:` tiering: P2P pairing state **survives `device_reset`**, is **cleared by `factory_reset`**, like the LoRaWAN session). Node returns to unpaired boot behavior. |
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

- **KDF construction review** — the one-block AES-ECB PRF (§4) is simple and
  uses only primitives already in flash; confirm with a crypto review that the
  label/serial domain separation is sufficient (vs. e.g. AES-CMAC).
- **ACK economics at scale** — at what node count per network does confirmed-
  uplink eat the gateways' 1 % duty cycle (§6/§10)? Options if it binds:
  ACK-every-Nth, or sink-driven TDMA (Piyare et al., §10 related work).
- **Multi-channel** — should JoinAccept's reserved field assign per-device
  data channels (frequency/SF) to spread load? v1 says single shared channel.
- **Multiple *networks* in RF range** — `net_id` filtering handles it, but the
  join channel is shared; JoinRequests are answered only by the central that
  knows the serial, which resolves ownership implicitly. Confirm this is
  acceptable (a device enrolled at two centrals joins whichever answers
  first).
- **Northbridge scheduled-TX precision** — RX1 hit accuracy over
  UART+MQTT+LAN needs a prototype measurement before freezing `rx1_delay`
  at 1 s.
- **Flash budget** — re-measure the dual-stack image against `0x34000`;
  decide whether debug keeps `CONFIG_APP_LORA_P2P=n`.

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
   (`radio_mode`, `app_ccm`, `app_transport` facade), still unpaired/unACKed
   behind a build flag; salvage `app_p2p.c` logic, `p2p.js` + tests.
2. **FW join + ACK state machine** — §5–§7 (RX1, retries, NVS pairing state,
   join triggers, self-healing rejoin).
3. **Northbridge RX-stream + scheduled TX** — UART protocol extension.
4. **Central service** — enrollment API, session DB, dedup/ACK routing,
   management MQTT, northbound decode.
5. **App-side** — join-key derivation + central registration (Manager-App
   coupling issue to be filed when phase 4 starts).
6. **HW validation** — range, RX1 timing, power delta vs. the 92 µA baseline,
   multi-gateway dedup/roaming.
