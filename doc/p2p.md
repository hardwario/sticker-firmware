# STICKER — LoRa P2P (raw LoRa) transport

> Tracking: epic **#118** (design **#119**, scaffolding **#120**, transport
> **#121**, config **#122**, receiver **#123**, tests/docs **#124**).

An alternative to the LoRaWAN transport for deployments with no LoRaWAN
infrastructure: the STICKER transmits its telemetry / alarm frames directly over
**raw LoRa** (no network server, no OTAA/ABP join) to a paired receiver. Useful
for point-to-point links, private short-range networks and factory / agriculture
setups.

The SX126x sub-GHz radio is shared between LoRaMac and raw LoRa, so the two
**cannot run at the same time**. Both stacks are linked into one image; the
active one is chosen at boot from a config parameter.

---

## Selecting the transport

`transport` is a boot-time config parameter (`application` group):

```
config application transport p2p       # or: lorawan (default)
settings save                          # persists + reboots
```

On boot `app_transport_init()` reads it and brings up **only** the selected
stack (`app_lrw` or `app_p2p`). Switching transport requires changing the
parameter and rebooting — there is no live switch (it would mean tearing down
and re-initialising the shared radio).

The payload layer is shared unchanged: `app_compose` builds the same protobuf
`Telemetry` snapshot and `app_report` owns the `interval_report` cadence for both
transports. The difference is purely how a frame reaches the air (`app_transport`
facade → `app_lrw` *or* `app_p2p`).

---

## Build

P2P is compiled in by default (`CONFIG_APP_LORA_P2P=y`). No special build command
is needed — the standard release/debug builds carry both stacks:

```bash
west build -b sticker sticker/app                          # release (both stacks)
west build -b sticker sticker/app -- -DEXTRA_CONF_FILE=debug.conf   # debug
```

`CONFIG_APP_LORA_P2P` only controls whether the P2P code is *present* — it does
**not** exclude `CONFIG_LORAWAN`. Set it to `n` to drop P2P and reclaim flash on a
LoRaWAN-only build.

**Flash budget:** the release image carries LoRaMac **and** the P2P transport and
sits at ~99.9 % of the 160 KB code partition. The dead PSA crypto layer was
dropped (AES-CCM now goes through the legacy `mbedtls_ccm` API directly, the same
path the NFC channel uses) to make room. Bandwidth and coding rate are fixed in
v1 (see below) for the same reason. If the image ever overflows, the first lever
is `CONFIG_APP_LORA_P2P=n` on the flash-tight build.

---

## Wire frame

```
+----------+-----------+-------------+-------------+----------------------+--------+
| net_id   | dev_addr  | frame_type  | counter     | AES-CCM ciphertext   | tag    |
| 4 B  BE  | 2 B  BE   | 1 B         | 4 B  BE     | = plaintext length   | 4 B    |
+----------+-----------+-------------+-------------+----------------------+--------+
\------------------ 11 B cleartext header (AAD) -------------/
```

- **Header** is cleartext: a receiver filters foreign traffic by `net_id` and
  routes by `dev_addr` / `frame_type` *before* spending a decrypt. It is fed to
  AES-CCM as additional authenticated data (AAD), so it is covered by the tag.
- **`frame_type`** replaces the LoRaWAN fPort and reuses its values:
  `2` telemetry, `3` alarm, `85` response, `86` inbound command.
- **Body** is the exact payload LoRaWAN would carry on that fPort (version byte +
  protobuf), so the off-device decoder reuses the LoRaWAN codec.
- **`counter`** is a strictly increasing 32-bit frame counter (see security).

`app_compose` bin-packs telemetry against a fixed budget of **240 B**
(255 B LoRa MTU − 11 B header − 4 B tag); larger snapshots split into multiple
frames.

---

## Link security (AES-CCM)

Raw LoRa has no MIC or encryption of its own, so the body is **AES-CCM**
(AES-128, 4-byte tag) encrypted *and* authenticated with the shared `p2p_key`
(provision via `config p2p-key <32 hex>`). This is the same `mbedtls_ccm`
primitive (`app_ccm.c`) the NFC channel uses.

The CCM nonce is `counter(4 BE) || dev_addr(2 BE) || frame_type(1) ||
direction(1) || zeros(5)` (13 B). The direction byte (0 = device→receiver) keeps
any future downlink keystream separate from the uplink.

**Nonce uniqueness across reboots:** reusing a `(key, nonce)` pair would leak
plaintext, so `counter` must never repeat for a given key. It is persisted with a
reservation window (NVS key `p2pfc/base`): on boot the firmware resumes from the
last persisted high-water and immediately reserves the next block of 256, so a
crash or power loss can never re-issue a counter that may already have been on the
air. NVS is written once per 256 frames.

> If `p2p_key` is all-zero the firmware logs a boot warning — the link is then
> effectively unprotected. Provision a real key before deployment.

---

## Duty cycle

Raw LoRa bypasses LoRaMac's duty-cycle enforcement, so `app_p2p` enforces the
**EU868 1 %** limit itself: after each frame it computes the time-on-air
(integer LoRa airtime formula, Semtech AN1200.13) and blocks further TX for
`airtime × 99`. A telemetry send refused by the duty-cycle gate is simply skipped
(the next `interval_report` cycle retries), mirroring the LoRaWAN `-EAGAIN` path.

---

## Configuration parameters (`p2p` group)

| Parameter            | Default      | Notes                                            |
|----------------------|--------------|--------------------------------------------------|
| `p2p_frequency`      | `868100000`  | Carrier frequency in Hz.                         |
| `p2p_spreading_factor` | `10`       | SF 6–12 (higher = longer range, lower rate).     |
| `p2p_tx_power`       | `14`         | dBm; the sticker RFO-LP caps at +14.             |
| `p2p_network_id`     | `0`          | 32-bit network id (header / receiver filter).    |
| `p2p_device_addr`    | `0`          | 16-bit device address (header).                  |
| `p2p_key`            | (unset)      | 16-byte AES-CCM link key; readable shell + NFC.  |

**Fixed in v1** (kept out of config to fit the dual-stack flash budget; they are
the common P2P defaults and rarely retuned in the field): **bandwidth 125 kHz**,
**coding rate 4/5**, **preamble 8**, **private sync word**, **explicit header**,
**IQ not inverted**. The paired receiver must match all radio parameters exactly.

```
config p2p-frequency 868100000
config p2p-spreading-factor 10
config p2p-tx-power 14
config p2p-network-id 1
config p2p-device-addr 42
config p2p-key 000102030405060708090a0b0c0d0e0f
config application transport p2p
settings save
```

---

## Receiver

There is no network server in P2P, so a receiver is needed to validate frames.

**Reference decoder** — `app/decoder/p2p.js` (Node ≥ 18, zero dependencies):

```js
const p2p = require("./app/decoder/p2p.js");
const out = p2p.decodeP2pFrame(frameBuffer, "000102…0e0f" /* p2p_key */);
// → { netId, devAddr, frameType, frameTypeName, counter, body, data }
```

It parses the header, verifies + AES-CCM-decrypts the body and decodes the
payload by reusing the LoRaWAN codec in `ttn.js`. `encodeP2pFrame()` is the
inverse (for tests / a software sender). Tested in `app/decoder/p2p.test.js`
(`node --test`).

**Capturing frames** — two options:

1. **A second STICKER in RX mode.** Flash a debug build (shell), set the same
   radio parameters + `p2p_key` + `p2p_network_id`, and run `p2p listen` on its
   RTT shell. Each received frame is validated, decrypted and logged with RSSI /
   SNR. (`p2p listen off` returns it to TX.)
2. **A Python / SX126x dev board** configured to 868.1 MHz / SF10 / BW125 / CR4-5,
   feeding captured frames to `p2p.js`.

---

## Limitations (v1)

- **Fire-and-forget**, unconfirmed: no ACK, no retransmission, no RX windows on a
  sending node.
- **TX-only sticker**: a deployed node only transmits; the RX / `p2p listen` path
  exists for the reference receiver and diagnostics (debug/shell builds only).
- **Bandwidth / coding rate fixed** at 125 kHz / 4-5 (flash budget).
- **No history replay / link check / clock sync** over P2P — those are
  LoRaWAN-specific.
- Hardware range / throughput validation and a native_sim ztest for the frame
  pack/unpack + duty-cycle accounting are follow-ups (the wire contract is
  currently pinned by the `p2p.js` round-trip tests).
