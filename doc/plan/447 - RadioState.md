# 447 — RadioState: radio link state and diagnostics (GetRadioState)

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
3. **Its own command, not part of Info.** One new command, `get_radio_state`, on every
   transport. The phone sends it over NFC next to `get_info`. Over the radio it goes
   out only when a host asks, never in the boot or 24 h announce (#445), so airtime
   stays spent on data. (A first draft put it in the NFC Info; decided on 2026-09-26
   to keep Info free of it.)
4. **Paging like every other answer** (#425).

## Decisions

| | |
|---|---|
| Name | `radio_state` (message `Response.RadioState`); the link-state enum is nested as `RadioState.State` |
| Info | carries no link state any more; the phone reads it with `get_radio_state` over NFC |
| Radio | new command `get_radio_state` = `Command` field **32**, body `GetRadioState { optional uint32 page = 1; }`; answer `Response.radio_state` = oneof field **14** (11 is reserved, was `info_lite`; 12/13 are `page_index`/`page_count`) |
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
| 18 | `airtime_hour_ms` | uint32 | both | airtime used in the sliding hour (common duty ledger since #460 T2d; budget per EU868 sub-band, 36 000 ms at 1 %) |
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
  - `Info`: `reserved 12, 16, 17, 18`;
  - `Response.radio_state = 14`;
  - `Command.GetRadioState`;
  - `app_config.yml` entry `get_radio_state` (proto_id 32), from which configen
    regenerates the oneof, the dispatch case and the `ttn.js` command map.
- **Common layer**:
  - `app_radio` owns the data (push model). Each backend reports the facts as they
    happen, through `app_radio_note_downlink()`, `app_radio_set_params()`,
    `app_radio_set_uplink_rssi()` / `_margin()`, `app_radio_set_session()`,
    `app_radio_set_fail_streak()` / `_join_attempts()`, `app_radio_set_duty_held()`
    and `app_radio_count()`. The airtime comes from app_radio's own duty ledger
    (`app_radio_duty_charge()`, #460 T2d; before that, `app_radio_set_airtime()`
    from P2P only).
  - Readers take a spinlock-consistent snapshot with `app_radio_get_status()`, which
    adds the state, the ages, the downlink's wall-clock time and the counters. Nobody
    reads a backend directly.
  - LoRaWAN pushes DR / TX power / DevAddr / FCntUp from the MIB after every sent
    uplink and on entering HEALTHY (under `lorawan_mac_lock()`). It maps DR→SF and the
    TXPower index → dBm per region (RP002 tables, integer math).
  - P2P pushes SF / TX power / session after every transmission.
- **app_cmd**:
  - `fill_radio_state()` for the `get_radio_state` answer;
  - a field-mask page builder that clears the `has_` flags through nanopb's field
    iterator, so no per-field switch;
  - greedy layout as in Info;
  - `PAGE_STREAM_RADIO` with a snapshot for the radio;
  - `GetRadioState.page` host paging.
- **Shell**: `ats device info` prints the state, signal and parameters lines.
- **Decoder** (`ttn.js`): `radio_state` as a Response body (field 14);
  `get_radio_state` in `encodeDownlink`; the retired Info fields removed.

## Verification

- Native `tests/cmd`:
  - the last-downlink group appears only after a downlink;
  - `get_radio_state` over NFC is one frame;
  - over LoRaWAN at 24 B it is paged: seq on every page, every one of the 25 fields
    exactly once, the downlink group never split, pages == the NFC frame;
  - at 11 B it is paged with the groups left out;
  - host paging out of range → `OUT_OF_RANGE`.
- Native `tests/p2p_logic`: the backend pushes the downlink quality to app_radio.
- `ttn.test.js`: `get_radio_state` encoder vectors (empty / page), paged answer decode,
  state names.
- HIL on 0413:
  - `get_radio_state` over NFC, P2P and LoRaWAN;
  - Hub-halt test: `fail_count` / `retry_count` / `fail_streak` match the RTT log.

## Compatibility

- **Manager-App**: sends `get_radio_state` over NFC instead of reading `lrw_state` (12)
  and the `last_dl_*` fields (16–18) from Info. Older app builds simply see those
  fields absent. Told on 2026-09-26.
- **Hub / Portal (proximos-v2)**: new optional command. Nothing changes unless it is used.
