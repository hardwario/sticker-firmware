# HARDWARIO STICKER — Universal Response Paging

One paging scheme for every answer the device sends over a radio: when a response does not
fit one frame, the device splits it into **pages** — each a complete, independently decodable
message carrying "page *i* of *N*" — and sends all of them by itself. The same behaviour for
Info, configuration dumps, parameter reads, 1-Wire scans, history frames and alarm batches, on
LoRaWAN today and on the P2P transport once it lands.

> Requested 2026-09-23: "stránkování by mělo být univerzální pro všechny příkazy, i pro čtení
> historie, deviceinfo apod." — then: "místo fragmentace zvol stránkování jako máme… pokud se
> nevejde tak bude pages: 1/5 a vím že mám 1. zprávu z 5 nutných", "jednotné pole pro všechny
> typy odpovědí, ať je to shodné chování napříč druhem zpráv", "mysli i na možnost použít v P2P".

**Status:** plan for this PR, which implements it. Stacked on #409 (it builds on #409's payload
budget helper, the 3c deferred announce and the 3d/3e GetConfig/GetParam page stream); the PR
base is `hynek/lrw-improvements-plan` and is retargeted to `v1.5.0` when #409 merges —
**retarget before #409's branch is deleted**, or GitHub closes this PR for good (see #382 →
#400).

---

## 1. Why

#409 made every uplink type survive a small payload budget, but each in its own way:

| Response | #409 behaviour at a small budget |
|---|---|
| Telemetry (fPort 2) | split by sensor group, every frame self-contained |
| ConfigDump (GetConfig / GetParam) | `ConfigDump.page_index/page_count`, device streams all pages (3d/3e) |
| HistoryFrame (req_history) | `HistoryFrame.frame_index/frame_count`, device streams frames |
| AlarmReport (fPort 3) | several reports sharing `base_time` / `total`, no page numbers |
| Info | `InfoLite` (firmware version only), full Info re-sent later |
| settings-info, W1Scan | not split — skipped / `BUDGET_TOO_SMALL` |

Three different paging vocabularies, two message types that cannot split at all, and a special
reduced message (`InfoLite`). A host has to know per message type how to tell "this is part 2
of 5". This PR replaces all of it with one rule.

**Rejected alternative — byte fragmentation** (split the encoded bytes, 2 B header,
reassemble at the receiver): universal and works at 11 B, but a fragment is not decodable on
its own (stateless LNS formatters such as TTN's cannot use it), and one lost fragment loses the
whole message. Decided against on 2026-09-23 in favour of self-contained pages.

## 2. The rule

1. **One pair of fields, in the envelope.** `Response` gains
   `uint32 page_index = 12; uint32 page_count = 13;` (numbers < 16 → one-byte tags, 2 B per
   field). Every response type uses them; no body type has its own paging fields any more.
2. **Absent = single frame.** A response that fits carries neither field (proto3 omits 0),
   so an unpaged answer is byte-identical to today's. `page_count >= 2` means "page
   `page_index` (0-based) of `page_count`".
3. **Every page is a complete message — decodable with no memory.** Same `seq`, same body
   type, a subset of the body, plus everything needed to interpret that subset (e.g. every
   history page carries its own `t0_unix` / `interval_s` / `present`, every alarm page its own
   `base_time` / `total` / `time_synced`). The TTN payload formatter and the ChirpStack codec
   are stateless — they see one uplink at a time and keep nothing between invocations — so
   the decoder never waits for, stores or merges pages: each uplink yields a correct,
   meaningful object on its own, labelled `pages: "i/N"`. Merging pages (disjoint field sets /
   list ranges, no overlap, no conflict) is optional and is the job of the application
   behind the LNS, keyed by `seq`.
4. **Device-driven on the radio, host-driven on NFC.** Over a radio transport the device
   answers the requested page (0 unless a `page` was given) and then sends the remaining pages
   by itself. Over NFC the phone keeps asking for a page (NFC pages are ~450 B, so only big
   config dumps page at all).
5. **Layout locked at the start.** Pages are laid out for the payload budget at the moment
   the answer is built; the same layout serves every page of that answer (`page_count` never
   changes mid-stream). If the budget later drops so a page no longer fits, #409's 3g
   recovery answers `Error BUDGET_TOO_SMALL` with the `seq` and the stream stops.
6. **Physical floor.** A single unit that does not fit even alone (an alarm rule is 17 B, one
   history record or 1-Wire ROM ~10 B) cannot be sent at that budget → `Error
   BUDGET_TOO_SMALL`. Everything made of small fields (Info, identity, status) pages down
   to the 11 B tier; GetConfig / GetParam need ≥ ~13 B (see "As built" below).

`AlarmReport` (fPort 3) is not a `Response`, so it gets the same two fields under the same
names: `uint32 page_index = 5; uint32 page_count = 6;`.

### Wire changes

| Message | Change |
|---|---|
| `Response` | + `page_index = 12`, `page_count = 13`; `reserved 11` (was `info_lite` in #409) |
| `Response.InfoLite` | **removed** (never released) — Info pages replace it |
| `ConfigDump.page_index/page_count` (1, 2) | **deprecated, no longer set** — kept declared so older firmware still decodes |
| `HistoryFrame.frame_index/frame_count` (1, 2) | **deprecated, no longer set** — same |
| `AlarmReport` | + `page_index = 5`, `page_count = 6` |

Field numbers checked free on `v1.5.0`, `feat-p2p`, #414, #407, #423 (2026-09-23).

## 3. Page units per response type

A page is built from **units**; the device packs units greedily into pages, measuring each
candidate page by really encoding it (with `page_index`/`page_count` at their worst-case
one-byte values, so the final numbers never make a page grow).

| Response | Unit | Notes |
|---|---|---|
| Info | each scalar field (fw_major, fw_minor, fw_patch, build_type, debug, serial, uptime, unix_time, battery, reset_cause, device_status), then each active alarm | per-field units let page 0 carry the firmware version at 11 B; built from a snapshot so all pages describe one moment |
| ConfigDump — GetConfig / GetParam | each config field | existing greedy page packing, page budget derived from the payload budget instead of the fixed 30 B |
| ConfigDump — settings-info (#412) | each of the 3 application fields, 9 capability flags, the `w1_slot_type` block | today one fixed 40 B page |
| W1Scan | each ROM | scan result kept, later pages do not rescan the bus |
| HistoryFrame | records (as today) | only the frame numbering moves to the envelope |
| AlarmReport | each event | today's greedy split, plus page numbers |
| Error, Ack | — | always fit |
| Telemetry (fPort 2) | — | unchanged, see §7 |

## 4. Architecture — transport-neutral core, per-transport driver

The paging logic must not know whether the frame goes out over LoRaWAN or P2P.

**Core (`app_cmd.c`):**
- `set_page(resp, index, count)` — the only place that writes the envelope fields.
- One page-stream state (a single radio is active at a time, `radio-mode`): kind (REQUEST,
  SETTINGS, W1SCAN, INFO, HISTORY), `seq`, the budget the layout was made for, next page,
  page count, and a kind-specific snapshot in a union (raw request ≤ 64 B, W1 scan result,
  Info scalars + alarm triples).
- `app_cmd_handle()` gets the transport and the frame budget as today (`out_cap`); for a
  radio transport it pages instead of failing and returns `APP_CMD_ACTION_PAGE_STREAM`.
- `app_cmd_stream_next(out, cap, &len)` → next page, `-ENODATA` when done;
  `app_cmd_stream_cancel()`.
- The autonomous builders (`app_cmd_build_info()`, `app_cmd_build_config_status()`) return
  page 0 plus a `more` flag.

**Driver (per transport, behind the `app_radio` facade on `feat-p2p`):**
- LoRaWAN (`app_lrw.c`): today's `m_page_stream_work` — one page per run while the response
  queue keeps a slot free, paced by the send path and the duty cycle, cancelled on rejoin.
- P2P (`app_p2p.c`): the same loop over its own queue / `send_confirmed()`, paced by the P2P
  duty governor (B2), cancelled on detach/rejoin. The budget is `app_p2p_get_max_payload()`
  (240 B body today) — so P2P pages rarely, but the rule and the wire format are identical,
  and a future P2P mode with a smaller MTU (SF/BW changes, the regulatory question in #408 B6)
  needs no new code.
- History replay moves onto the same stream: the LoRaWAN (`app_lrw_history_replay_start()`)
  and P2P (`app_p2p_start_history_replay()`, feat-p2p B8) replays today duplicate the frame
  loop; with the core owning the page/frame layout both become thin drivers.
- A new transport only supplies "budget", "queue a frame" and "call me when there is room".

`APP_CMD_TRANSPORT_P2P` already reuses the LoRaWAN command gating (feat-p2p B4), so a P2P
command answer takes exactly the radio path above.

## 5. Host side

**Hard constraint: the LNS decoders are stateless.** `ttn.js` runs as the TTN payload
formatter and as the ChirpStack codec (same `decodeUplink()`), one uplink per call, with no
storage between calls. The design must never need cross-uplink memory in the decoder:

- **`ttn.js`:** read `page_index`/`page_count` from the envelope (and from `AlarmReport`); add
  a display field `pages: "1/5"` (1-based) whenever `page_count >= 2`; decode the page's
  content exactly like an unpaged message of that type, but emit only the fields actually
  present (no default zeros — an Info page without `fw_major` must not read as firmware
  0.0.0). No buffering, no reassembly, no global state. Keep decoding the deprecated
  `ConfigDump`/`HistoryFrame` numbering for older firmware. Drop `info_lite`.
  `AlarmReport.truncated` is only meaningful when unpaged. Tests: every paged vector decodes
  on its own; decoding page 2 before page 1 (or without page 1) gives the same result.
- **Application side (optional merge):** a consumer that wants the whole answer — the Hub
  (proximos-v2, Rust decoder behind ChirpStack), a TTN integration, a backend — merges the
  decoded pages by (DevEUI, fPort, `seq`), for `AlarmReport` by `base_time`; complete when
  `page_count` distinct indices arrived. Nothing breaks for a consumer that ignores `pages`:
  it just sees several partial answers. Hub side tracked in proximos-v2#96 (LoRaWAN merge
  by `seq` / `base_time` with timeout + re-request of a missing `page`, P2P several 0x55
  per `seq`, Rust decoder parity incl. error code 9; decoder parity: proximos-v2#90).
- **Manager-App (NFC):** GetConfig/GetParam page count now comes from the envelope
  (`Response.page_count`, absent = 1) instead of `ConfigDump.page_count`; W1Scan / Info never
  page over NFC in practice. Coordinate with apps/manager before this lands.

## 6. Interaction with #409

| #409 piece | After this PR |
|---|---|
| 3a payload-cap helper, compact LoRaWAN Error, MAC-flood defer | kept |
| 3b alarm split + telemetry alarm bits | kept; reports gain page numbers |
| 3c `InfoLite` | **removed** — replaced by Info pages |
| 3c deferred boot announce | kept, but only needed when not even one Info / settings unit fits |
| 3c `BUDGET_TOO_SMALL` | kept (physical floor, §2.6) |
| 3d/3e GetConfig/GetParam stream | generalised into the page stream; numbering moves to the envelope |
| 3f history floor | kept; numbering moves to the envelope |
| 3g DR-drop recovery | kept; a page that no longer fits → `BUDGET_TOO_SMALL` + stream stop |

## 7. Open questions — decided 2026-09-23 (all as proposed)

1. **Telemetry (fPort 2)** is split by sensor group and every frame is already a complete
   snapshot slice. Adding `page_index/page_count` would make it uniform too, but `Telemetry`
   field numbers < 16 are all taken, so each field would cost 3 B (6 B per frame) — a lot at
   11 B. **Decided: telemetry stays out.**
2. **NFC history** (`req_history_page`) is cursor-paged by the phone (`next_ord` /
   `has_more`), because records are addressed by ordinal. Proposal: keep the cursor; the
   envelope fields stay absent there (documented exception). **Decided: keep the cursor.**
3. **Page loss.** No retransmission in v1 — a host missing a page re-sends the request with
   `page = <missing index>` (the stream then sends from that page to the end). **Decided.**

## 8. Implementation steps

**Progress:** step 1 ✅ `0290f44` · steps 2+3 ✅ `b746288` (landed together: the Info
stream and the generic stream share one code path) · step 5 ✅ `c6972bf` · step 6 ✅
(folded into each step: envelope + `pages`, Info-page pruning, AlarmReport) · step 8 ✅
· **step 4:** numbering in the envelope done in step 1; both history replays build their
frames with `app_cmd_build_history_frame()`, so they carry it without a driver change —
no further unification needed · **step 7 (P2P driver)** ✅ in PR #426 into `feat-p2p`
(`app_p2p.c` lives there): paging enabled for both radio transports, `m_page_stream_work`
queues one page per run over the P2P TX queue (P2P responses are ≤ 64 B, so GetConfig /
Info with alarms / W1Scan do page there). The central must accept several 0x55 with one
`seq` (proximos-v2 MR!30).

**As built — physical floor refined:** a unit that does not fit even alone is left out
(not `BUDGET_TOO_SMALL` for the whole answer); zero-valued Info fields are skipped;
`BUDGET_TOO_SMALL` only when nothing fits. At the 11 B tier the Info then carries the
small fields (firmware version, battery, reset cause, status) but not the serial,
unix time or alarm entries.

**As built — GetConfig / GetParam over LoRaWAN** keep the #409 3d/3e layout: fixed 30 B
field pages (DR0-sized, conservative, room for FOpts), the same at every DR ≥ 51 B (e.g. a
default config = 6 pages of 29–41 B, HW-seen). They do **not** page down to the 11 B tier:
a paged ConfigDump frame needs about 13 B (version, `seq`, `page_index`, `page_count`,
`config_dump` + section wrapper, one field), so there the answer is `BUDGET_TOO_SMALL`
(native-tested). No LoRaWAN region has a DR between 11 B and 51 B, so a budget-aware
layout would only save frames at higher DRs — a possible follow-up, not a gap.


One step per commit; each: Release + `debug.conf` builds (sticker2 Zephyr, #419 patch), native
suites, decoder tests, configen pytest, clang-format.

1. **Proto + `set_page()`** — envelope fields, AlarmReport fields, `InfoLite` removal,
   deprecation comments; ConfigDump handlers and settings-info write the envelope.
2. **Core page stream** — generalise #409's stream (kinds, union snapshot), settings-info
   units, W1Scan units.
3. **Info paging** — snapshot, per-field + per-alarm units, `app_cmd_build_info(…, more)`,
   LoRaWAN GetInfo; remove `InfoLite` paths and the 3c "full Info later" special case.
4. **History on the stream** — numbering in the envelope; LoRaWAN replay as a driver.
5. **AlarmReport pages** — two-pass split in `alarm_batch_flush()` with real page numbers.
6. **Decoder** — envelope + `pages` field, partial-page rules, AlarmReport, back-compat.
7. **P2P driver** (on the `feat-p2p` merge, or as a follow-up there) — stream driver in
   `app_p2p.c` behind `app_radio`, P2P history replay onto the stream.
8. **Docs** — `doc/version 1.5.md`, host-side notes, proximos-v2 / apps/manager issues.

**Verify beyond the standard set:** native tests per response type at 11 B, 51 B and 242 B
(page count, disjoint union = full answer, `seq` constant, every page ≤ budget, decodes on its
own); HIL on the EU868 bench for the ≥ 51 B tiers (GetConfig / GetInfo / history streams, an
alarm batch > 1 frame); the 11 B tier needs a US915/AU915 gateway (as #409 A5b).

## 9. Cost estimate

About +1 KB flash (layout + stream kinds), +~150 B RAM (the stream snapshot union). Debug RAM is
at 94 % after #409 — measure early; the snapshot union is the lever if it gets tight.

## 10. HIL record 2026-09-23

Bench: STICKER DevEUI `5876070000000413`, J-Link `822005110`, EU868, ADR off; ChirpStack v4 on the ProXimos Hub.
Frames were read from the raw capture and decoded with `ttn.js`; from Hub combined10 on, pages were also assembled
on the Hub (proximos-v2 !99). Debug images carry the #419 `loramac-node` patch. The run was done by the Sticker
controller session.

**First run, head `9ef98b0`:**

| Test | Result |
|---|---|
| P1 GetConfig at DR0 | PASS — 6/6 pages, same `seq` |
| P2a / P2b GetParam | PASS — one key unpaged; P2b unpaged |
| P3 GetConfig `page=3` | PASS — pages 3..5 byte-identical to the full stream |
| P4 new GetConfig cancels a running stream | PASS — one page of the old `seq` was already queued and still went out |
| P5a / P6 boot Info / settings-info | PASS — unpaged |
| **P5b GetInfo with 6 active alarms at DR0** | **FAIL** — MPU fault "Stack overflow on CPU 0" in `m_work_q` (guard of `m_work_stack` hit, PC in nanopb `load_descriptor_values`) → watchdog reset |

A/B on `v1.5.0` @ `8c23cc5` (without this PR), same scenario: no crash; the unpaged 50 B Info trims
`active_alarms` to 4/6. So P5b was a **regression of this PR**. `app_cmd_handle()` kept the `Command` + `Response`
and the paged-Info layout with two more `Response`s on the 4 KB `m_work_q` stack at the same time.

**Fix `5503d54`:** `Command` decode and dispatch moved into a `__noinline` `decode_and_dispatch()`, and the
dead response reused as scratch.

**Re-run, heads `5503d54` + `4c7b10a` (decoder only):**

| Test | Result |
|---|---|
| P5b | PASS — 2 pages (3 + 3 alarms), no reset |
| P2b paged | PASS — one key per page, fixed 30 B layout (#409) |
| P7 AlarmReport pages | PASS |
| P8 history, 30 records | PASS — 4 frames, envelope numbering (`4c7b10a` omits the legacy HistoryFrame numbering when absent) |
| P10 at DR5 | PASS — Info 7 + 1 and AlarmReport 5 + 3 (64 B TX slot, a design limit); GetConfig with 8 rules = 14 pages |
| Release image | PASS — Info in one frame with 8 alarms, settings-info + `w1_slot_type`, GetConfig 14/14 assembled on the Hub |
| D1–D3 with #424 | PASS — boot, a page stream, and a link-recovery rung DR5 → DR4 + `lc ok` → HEALTHY with the same DevAddr; the stream is unaffected |

**Not HW-tested:** the 11 B tier (US915 DR0, AU915/AS923 DR2 dwell), AU915, the P2P driver (#426). Native tests only.

**Observations (open, not blockers):**
- EU868 duty cycle at DR0: LoRaMac's credits reset only per hour, so ~36 DR0 frames can block every uplink for
  ~50 min. Host guidance is in `doc/version 1.5.md` §12; the Hub limits requests (proximos-v2#96 / !99).
- Priority: an AlarmReport page waited ~37 s behind a 14-page GetConfig stream, because responses drain before
  alarms. "Alarms first" vs. leaving it to the host is an open decision.
- A rejoin does not cancel pages already queued. They go out in the new session, possibly out of order. Consumers
  merge by `seq` and page index, so this is tolerated.

