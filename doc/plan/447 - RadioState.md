# 447 — RadioState: radio link state and diagnostics (NFC Info + GetRadioState)

Issue: #446 · PR: #447 (into `feat-p2p`) · Related: #409 A2, #423, #439 (transport
layer), #442/#443 (P2P ADR), #445 (24 h announce)

## Why

The link information a node exposes today is spread over three places, and all of them
are LoRaWAN-flavoured:

- `Info.lrw_state` (field 12): the link state, now used for P2P as well;
- `Info.last_dl_rssi` / `last_dl_snr` / `last_dl_age_s` (16–18, #423);
- the `ats lrw status` / `ats radio status` shell dumps, which carry the rest (DevAddr,
  FCnt, DR, TX power, LinkCheck margin, fail streaks) but only on a debug build with RTT.

An installer with a phone, or support looking at a node that goes quiet, cannot see why.
For example, after the P2P HIL on 2026-09-26 (PP8, Northbridge halted for 5 min) the node
had made 5 unacknowledged cycles of 4 transmissions each. None of that was visible
anywhere once the link came back. The network sees only the frames that arrived. What
the node measured and did while the link was down exists only on the node.

Goals:

1. **One message, one name.** A single `RadioState` for LoRaWAN and P2P, replacing
   `lrw_state` and the `last_dl_*` fields. It uses the same shape and the same meaning
   on both radios, in line with the transport-layer parity goal (#439).
2. **Enough for analysis.** State, radio parameters, the quality of both link
   directions, session identity, failure streaks, duty cycle and counters since boot.
3. **NFC always, radio on request only.** The phone gets it in every Info. Over the
   radio it goes out only when a host asks with a new downlink command, never in the
   boot or 24 h announce (#445). Airtime stays spent on data.
4. **Paging like every other answer** (#425).

## Decisions

| | |
|---|---|
| Name | `radio_state` (message `Response.RadioState`); the link-state enum is nested as `RadioState.State` |
| NFC | `Info.radio_state` = field **19**, NFC-only, like `dev_eui` / `claim_token`; the LoRaWAN/P2P Info omits it |
| Radio | new command `get_radio_state` = `Command` field **32**, body `GetRadioState { optional uint32 page = 1; }`; answer `Response.radio_state` = oneof field **12** (11 is reserved, was `info_lite`) |
| Transports | all (LoRaWAN, P2P, NFC, vendor, shell); read-only, no secrets (DevAddr and FCnt travel in clear on every frame anyway) |
| Retired | `Info.lrw_state` (12) and `Info.last_dl_rssi/snr/age_s` (16–18) become `reserved`. 12 shipped in v1.4.0; 16–18 exist only in unreleased v1.5.0, but they are reserved as well so that no app build decodes them as something else |
| Paging, radio | #425 envelope. Units are single fields, except the groups that must travel together (below). A unit too big for the budget on its own is left out (physical floor, as in Info). The pages are streamed from a snapshot, so every page describes the same moment |
| Paging, NFC/vendor/shell | host-driven, `GetRadioState.page` (as `GetInfo.page`). Normally one page |
| Values | a snapshot at request time; absent = unknown / not applicable (every field is `optional`) |

### Fields

`RadioState`, all `optional`. "Both" means LoRaWAN and P2P fill the field with the same
meaning.

| # | Field | Type | Radio | Meaning |
|---|---|---|---|---|
| 1 | `state` | `State` | both | IDLE / JOINING / HEALTHY / WARNING / RECONNECT / DISABLED (`enum app_radio_state`) |
| 2 | `sf` | uint32 | both | spreading factor of the uplinks now (7..12) |
| 3 | `datarate` | uint32 | LoRaWAN | DR index now |
| 4 | `tx_power_dbm` | sint32 | both | conducted uplink TX power now, capped by the PA (14 dBm on RFO_LP) |
| 5 | `dl_rssi` | sint32 | both | last downlink as measured by the node: RSSI, dBm |
| 6 | `dl_snr` | sint32 | both | … SNR, dB |
| 7 | `dl_age_s` | uint32 | both | … seconds since it |
| 8 | `dl_unix_time` | uint32 | both | … its wall-clock time (omitted while the RTC is unsynced) |
| 9 | `ul_rssi` | sint32 | P2P | last acknowledged uplink as heard by the Hub: RSSI from the Ack, dBm |
| 10 | `ul_snr` | sint32 | P2P | … SNR from the Ack, dB |
| 11 | `ul_margin` | uint32 | LoRaWAN | last LinkCheckAns: demodulation margin, dB |
| 12 | `ul_gw_count` | uint32 | LoRaWAN | … number of gateways |
| 13 | `dev_addr` | uint32 | both | LoRaWAN DevAddr / P2P node address (16 bit); omitted when not joined |
| 14 | `fcnt_up` | uint32 | both | next uplink frame counter |
| 15 | `fail_streak` | uint32 | both | consecutive uplinks without confirmation (LoRaWAN: LinkCheck failures, P2P: unacked cycles); drives WARNING / RECONNECT |
| 16 | `join_attempts` | uint32 | both | attempts in the current (re)join episode (backoff step) |
| 17 | `duty_blocked_s` | uint32 | both | how long sends have been held by the duty cycle; absent = not held |
| 18 | `airtime_hour_ms` | uint32 | P2P | airtime used in the sliding hour (duty ledger; EU868 budget 36 000 ms) |
| 19 | `uptime_s` | uint32 | both | time base of the counters below (same as `Info.uptime_s`) |
| 20 | `tx_count` | uint32 | both | since boot: uplink transmissions, retransmissions included, joins excluded |
| 21 | `rx_count` | uint32 | both | … downlinks received (Acks, commands, MAC answers) |
| 22 | `retry_count` | uint32 | both | … retransmissions (P2P: Ack retries; LoRaWAN: a frame re-sent after a failed send) |
| 23 | `fail_count` | uint32 | both | … uplinks never confirmed (P2P: unacked after all retries; LoRaWAN: LinkCheck timeouts) |
| 24 | `tx_err_count` | uint32 | both | … sends refused by the MAC or failed in the radio (LoRaWAN includes duty-cycle refusals; P2P waits for its ledger instead of refusing) |
| 25 | `join_count` | uint32 | both | … join attempts (JoinRequests / `lorawan_join()`) |

Paging groups over the radio:

- `{dl_rssi, dl_snr, dl_age_s}`: RSSI/SNR never travel without their age;
- `{ul_rssi, ul_snr}`;
- `{ul_margin, ul_gw_count}`;
- every other field is its own unit.

At the 11 B tier (US915 DR0) the groups do not fit and are left out. From EU868 DR0
(51 B) up, the whole message is 2–3 pages; from DR3 it is one frame.

### Counters

The counters live in the common layer (`app_radio.c`, #439 direction). The backends
report events through `app_radio_count(APP_RADIO_CNT_*)`:

- 6 × `atomic_t`, zeroed at boot and never persisted;
- they wrap at 2³²;
- they count the active radio only (a radio-mode switch reboots anyway).

## Implementation

- **Proto** (`app_config.proto`):
  - `Response.RadioState` message and nested `State` enum;
  - `Info.radio_state = 19`, `reserved 12, 16, 17, 18`;
  - `Response.radio_state = 12`;
  - `Command.GetRadioState`;
  - `app_config.yml` entry `get_radio_state` (proto_id 32), from which configen
    regenerates the oneof, the dispatch case and the `ttn.js` command map.
- **Common layer**:
  - `struct app_radio_status` + `app_radio_get_status()` in `app_radio.c`. It fills the
    state, the last downlink (+ unix time from the RTC), the counters and the PA cap,
    then asks the backend: `app_radio_lrw_fill_status()` / `app_radio_p2p_fill_status()`.
  - LoRaWAN reads DR / TX power / DevAddr / FCntUp from the MIB under `lorawan_mac_lock()`
    (guarded by `m_mac_started`). It maps DR→SF and the TXPower index → dBm per region
    (RP002 tables, integer math).
- **app_cmd**:
  - `fill_radio_state()` shared by Info (NFC) and `get_radio_state`;
  - a field-mask page builder that clears the `has_` flags through nanopb's field
    iterator, so no per-field switch;
  - greedy layout as in Info;
  - `PAGE_STREAM_RADIO` with a snapshot for the radio;
  - `GetRadioState.page` host paging.
- **Shell**: `ats device info` prints the state, signal and parameters lines.
- **Decoder** (`ttn.js`): `radio_state` in Info and as a Response body;
  `get_radio_state` in `encodeDownlink`; the retired fields removed.

## Verification

- Native `tests/cmd`:
  - NFC Info carries `radio_state`, the LoRaWAN Info does not;
  - `get_radio_state` over NFC is one frame;
  - over LoRaWAN at 51 B it is paged, with seq and a consistent snapshot on every page;
  - at 11 B it is paged with the groups left out;
  - host paging out of range → `OUT_OF_RANGE`.
- Native `tests/p2p_logic`: the counters (tx / retry / fail / rx / join) across an
  unacked cycle and a re-join.
- `ttn.test.js`: `get_radio_state` encoder vector, answer decode, Info field 19 decode.
- HIL on 0413:
  - NFC Info and `get_radio_state` over P2P and LoRaWAN;
  - Hub-halt test: `fail_count` / `retry_count` / `fail_streak` match the RTT log.

## Compatibility

- **Manager-App**: reads `Info.radio_state` (19) instead of `lrw_state` (12) and the
  `last_dl_*` fields (16–18). Older app builds simply see those fields absent.
- **Hub / Portal (proximos-v2)**: new optional command. Nothing changes unless it is used.
