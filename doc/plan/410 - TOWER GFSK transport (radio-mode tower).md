# HARDWARIO STICKER — TOWER GFSK Transport (`radio-mode tower`)

A fourth radio transport in which STICKER speaks the HARDWARIO TOWER network protocol
natively over GFSK. Split out of
`doc/plan/408 - LoRa improvements - P2P hardening.md` (PR #408) so that the P2P hardening
and the TOWER transport are separate, independently reviewable streams; the LoRaWAN side is
PR #409. This is the "next phase" work — it starts after #408's P2P steps land, and shares
one prerequisite with them (the radio-driver extension, see Step 1).

> Decision (2026-08-27): **no protocol merge.** STICKER P2P stays its own protocol and only
> absorbs TOWER's good ideas (that is PR #408, Track B). Talking to a TOWER network is this
> separate, later feature.

**Status:** this document is the plan for this PR; code lands on top of it. The TOWER
protocol survey and the 16-row TOWER-vs-P2P comparison live in the #408 plan (§1.1, §4) and
are not repeated here. Base branch is `feat-p2p`, because the transport plugs into the
`app_radio` facade that lives there.

---

## 1. What it is

- A fourth value in the `radio_mode` enum {off, lorawan, p2p, **tower**}, selected at boot —
  consistent with the current design (one radio, no concurrent modes). The decoder's enum
  map needs the new value.
- **A new `app_tower` transport module**, structured like `app_p2p` and plugged into the
  existing `app_radio` facade: frame codec (96 B cap — the SPIRIT1 FIFO limit), AES-128-CCM
  with an 8-byte tag (our `app_ccm` hardware engine is the same primitive, only the nonce
  layout differs: `src ‖ counter ‖ bulk_index ‖ zeros`), replay lanes, the 3-way JOIN
  pairing under the public `PAIRING_KEY`, and a duty governor.
- **The PHY is reachable — verified.** TOWER is GFSK 19.2 kbps / ±20 kHz deviation /
  ~216 kHz RX bandwidth / 4-byte sync word `0xDB624715` / CRC-16 `0x1021` / PN9 whitening
  (`tower-firmware`, `src/radio/config.rs`). The STM32WLE5's SX126x has a full GFSK modem
  and the in-tree loramac-node HAL already drives it: the `MODEM_FSK` paths in
  `modules/lib/loramac-node/src/radio/sx126x/radio.c`, sync words up to 8 bytes
  (`SX126xSetSyncWord`, `:692`), whitening (`RADIO_DC_FREEWHITENING` +
  `SX126xSetWhiteningSeed`, `:686`, `:693`), and configurable CRC. The nearest RX bandwidth
  is 234.3 kHz.
- **Radio access is the hard prerequisite**: Zephyr's `lora.h` is LoRa-only and has no FSK,
  so this needs either direct `Radio.*` HAL calls or an extension to the in-workspace Zephyr
  driver — the **same driver work as B7 (CAD) in #408**, so the two must be designed as one
  driver change, not two.

## 2. Open risks

- **Bit-level PHY interop** must be validated on the bench against a real TOWER dongle
  before any protocol work is worth doing. The classic traps: the PN9 whitening
  implementation, bit order, length-field placement in the SPIRIT1 Basic packet format
  versus the SX126x packet engine, and CRC-versus-whitening ordering.
- **`tower-firmware` is under active development** — the port must be pinned to a specific
  commit (`b7f3f4a` surveyed) and re-validated on any protocol-affecting change upstream.
- **96 B frames** constrain payloads relative to P2P's 255 B; STICKER's compose/budget
  machinery already handles splitting, but responses near the cap need checking.
- **Product overlap**: TOWER's network and STICKER P2P solve similar problems (pairing, CCM,
  ACK + pending flag, duty). Whether a deployment wants STICKERs on a TOWER network or on
  their own P2P network is a product decision that should be settled before Step 3.

## 3. Tracking

- [ ] Step 1 — radio-driver FSK access (designed together with #408 B7/CAD)
- [ ] Step 2 — GFSK PHY profile + bench bit-level interop against a real TOWER dongle
- [ ] Step 3 — frame codec + CCM nonce layout, host-testable
- [ ] Step 4 — replay lanes + counter persistence
- [ ] Step 5 — 3-way JOIN pairing
- [ ] Step 6 — `app_radio` integration, `radio-mode tower`, config/decoder plumbing
- [ ] Step 7 — duty governor + HIL end-to-end against a TOWER gateway

## 4. Implementation steps

Standard verification per step as in #408: the three build configs, `bash
tests/run_native.sh`, `clang-format --dry-run --Werror`; configen pytest + decoder tests
when yml/proto change.

### Step 1 — radio-driver FSK access *(shared with #408 B7)*

Decide and build the one driver change both features need: expose FSK TX/RX (this PR) and
CAD (#408 B7) from the in-workspace Zephyr LoRa driver, or bypass it with direct
`Radio.*` HAL calls. Design note first, since it decides where all later code lives; the
driver owns a static `RadioEvents_t`, so events (CadDone, FSK RX) need a registration path.

### Step 2 — GFSK PHY profile and bench interop

Program the TOWER PHY (19.2 kbps, ±20 kHz, sync `0xDB624715`, CRC-16 `0x1021`, PN9,
234.3 kHz RX BW) and prove **bit-level** frame exchange against a real TOWER dongle in both
directions. This is the go/no-go gate for the whole feature — everything after it is
software; this step is where the PN9/bit-order/length-field/CRC-ordering traps live.

### Step 3 — frame codec and crypto, host-testable

Port the TOWER frame codec and the CCM nonce layout (`src ‖ counter ‖ bulk_index`, 8 B tag)
into `app_tower`, reusing `app_ccm`. Applying TOWER's own lesson (host-testable decision
kernels — #408 §4): pure functions, covered by a native ztest suite from day one, with
golden vectors captured from the real dongle in Step 2.

### Step 4 — replay lanes and counter persistence

TOWER's receiver-side replay lanes and reserve-ahead TX counter (fail-closed, saturating) —
same NVS pattern as P2P's `p2pfc`, separate subtree. Native tests.

### Step 5 — 3-way JOIN pairing

JoinRequest/challenge/JoinAccept under the public `PAIRING_KEY`, host-minted per-node key
stored in NVS. Bench-tested against a TOWER gateway running its normal pairing window.

### Step 6 — integration and plumbing

`radio_mode` gains `tower` (yml enum + regenerated config + decoder map + `_LRW_ENUM`),
`app_radio_init()` dispatches to `app_tower_init()`, LED/state mapping like the P2P
mapping, `ats radio status` grows a TOWER branch.

### Step 7 — duty governor and HIL

Reuse #408's token-bucket duty governor (B2) parametrised for the TOWER channel plan; full
HIL end-to-end: pair, uplink with ACK, downlink via pending flag, reboot persistence,
against a real TOWER gateway.

## 5. Explicit non-goals

- **Protocol merge or convergence** — decided against; P2P stays its own protocol.
- **Concurrent modes** — one radio, one transport at boot, same as today.
- **TOWER FHSS/AFA modes** — v1 targets the plain single-channel EU868 TOWER network; the
  US FHSS mode is out of scope until a product need exists.

## 6. References

- `doc/plan/408 - LoRa improvements - P2P hardening.md` — the TOWER survey (§1.1), the
  TOWER-vs-P2P comparison (§4), and B7/CAD (the shared driver work).
- `hardwario/tower-firmware` @ `b7f3f4a` — `src/radio/` (SPIRIT1 stack), `docs/radio.md`
  (the 456-line radio guide), `crates/tower-net-core` (replay/counter kernels),
  `docs/gateway.md`.
- `modules/lib/loramac-node/src/radio/sx126x/radio.c` — the in-tree `MODEM_FSK` paths.
