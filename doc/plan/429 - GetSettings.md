# 429 — GetSettings: the boot settings-info ConfigDump on request

Issue: #428 · PR: #429 (stacked on #425, retarget to `v1.5.0` when #425 merges) ·
Hub side: proximos-v2 !91

## Why

The ProXimos Portal "Load from device" needs the device's key operating settings. Over
LoRaWAN the only read-back was `GetConfig`: 38 keys in 6 pages, plus one page per alarm
rule (14 pages with 8 rules), which is ~13–30 s of SF12 airtime at EU868 DR0. That is a
large share of the 36 s/h duty-cycle budget (#425 host guidance). The #412 boot dump
already has the right subset in one frame, but a host could not ask for it.

## Decisions

| | |
|---|---|
| Command | `get_settings` = `Command` field **31**, empty body `GetSettings {}` |
| Numbering | 29/30 are claimed by #414 (`get_claim_info` / `get_basic_info`); 15 is free on the wire but was `req_alarm_rules` (removed without `reserved`), so it is not reused |
| Content | exactly the #412 boot settings-info: application 2/3/4, sensors 1..9, runtime `w1_slot_type` |
| `seq` | the command's `seq` (the boot dump keeps 0), so the Hub pairs it and can tell it apart |
| Values | staged config, like `GetConfig` / `GetParam` (same `app_config()` source) |
| Paging | the #425 envelope over LoRaWAN when the budget is smaller (`PAGE_STREAM_SETTINGS` with the seq); one frame at EU868 DR0 and up |
| Transports | all (LoRaWAN, NFC, shell); P2P via the #426 driver on `feat-p2p` |
| Decoder | unchanged (normal `config_dump`); `ttn.js` learns the name for `encodeDownlink` |

## Implementation

- `app_config.yml` entry, then configen regenerates the proto oneof, the dispatch case
  and the `ttn.js` command map. The body message is hand-written in `app_config.proto`.
- `app_cmd_handle_get_settings()` fills every settings-info item with the command's
  seq. If that does not fit over LoRaWAN, `app_cmd_handle()` calls `settings_paged()`,
  which re-lays out the items for the budget and arms the stream.
- `config_status_fill()` / `_layout()` / `_page()` take the `seq`, since it counts in
  the measured size, and a caller-provided scratch `Response`. The command path already
  holds `app_cmd_handle()`'s `Response` on `m_work_q`, and a second ~600 B one was what
  overflowed the 4 KB stack in #425 HIL P5b.

## Verification

- Native `tests/cmd`:
  - one frame, byte-compared with the boot dump apart from the seq;
  - 16 B budget, paged, seq on every page, all 12 settings;
  - NFC, one frame.
- `ttn.test.js`: encoder vector `0807fa0100` and answer decode.
- HIL 2026-09-23: seq 7 → one 34 B frame at DR5, `config_dump` byte-identical to the
  boot settings-info (manual test **L4c**).
