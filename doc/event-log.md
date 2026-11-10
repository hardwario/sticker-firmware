# Event log (PoC)

The event log keeps **every alarm edge** (activate / deactivate of any alarm
rule or built-in watchdog) in a flash ring and uploads them in batches on
**LoRaWAN fPort 4**. Unlike the fPort-3 alarm detail (sent per alarm window),
nothing is lost between uplinks: every event carries a **sequence number
(seq)**, so a receiver sees gaps and can ask the device to send missing events
again (`req_events`). The same log can be read on site over **NFC**
(`req_events_page`).

Status: proof of concept on branch `hynek/event-log-poc`. LoRaWAN only (P2P
does not upload the log).

## Configuration

All parameters are in the `alarms` group (shell `config <name>`, NFC, downlink
`set_param`).

| id | name | default | meaning |
|---|---|---|---|
| 22 | `event_log_enable` | false | Record alarm edges into the log |
| 23 | `alarm_send_detail` | true | Keep sending the fPort-3 alarm detail (set false when the log replaces it) |
| 24 | `alarm_send_report` | true | Keep the alarm state in the fPort-2 report |
| 25 | `event_flush_count` | 20 | Upload after this many new events (0 = off) |
| 26 | `event_flush_interval` | 3600 | Upload at least every N seconds when events wait (0 = off) |
| 27 | `log_split` | 0 | Percent of the history partition given to the event log: 0 / 25 / 50 / 75 / 100 |
| 28 | `event_format` | full | fPort-4 format: `full`, `compact`, `raw` |

Upload happens on whichever trigger fires first (count or interval).

**`log_split` must be at least 25** for the log to work. 0 (the default)
leaves the whole partition to history and the event log has no storage.
Changing `log_split` **erases the whole partition** (history and event log) on
the next boot, and the **seq restarts at 1**: a receiver that sees a seq lower
than the last one received treats it as a new log (compare `oldest_seq`).

Upgrade from firmware without the event log keeps the history: `log_split`
defaults to 0, nothing is erased and history mounts its pages as before
(verified on HW: main -> this branch, old records read back by `req_history`).
A downgrade to such firmware followed by a settings save drops the event log
parameters; back on this firmware they are at their defaults (log off).

### Capacity

The history partition has 32 KB = 16 flash pages of 2 KB. One page holds
**126 events** (16 B record).

| `log_split` | event log | history |
|---|---|---|
| 0 % | none | 16 pages |
| 25 % | 4 pages = 504 events | 12 pages |
| 50 % | 8 pages = 1008 events | 8 pages |
| 75 % | 12 pages = 1512 events | 4 pages |
| 100 % | 16 pages = 2016 events | none |

History holds `1757 B / sample size` records per page (for example 439 records
of 4 B).

Every boot starts a new page (in history too): records of an unsynced boot are
stamped with that boot's uptime, and one page can carry only one uptime-to-unix
offset. Each reboot therefore uses one page, and with a full ring it evicts the
oldest page. Choose `log_split` with this in mind: few pages + frequent reboots =
short retention. 50 % is a reasonable default for motion logging.

## Time

Events carry unix time once the clock is synced. The RTC keeps the time over a
reset, so only a power loss (battery change) starts a boot without time. Such a
boot stamps its events with uptime: they go out with `time_synced = false` and
`base_time` = seconds since that boot. When the clock syncs in the same boot the
page gets a fix-up (stored in flash) and its events go out in unix time; a boot
that never synced keeps uptime for good and is sent in its own frames, never
mixed with synced events.

## Sequence numbers

* Every event gets the next number of a global counter starting at 1. The
  counter never restarts: after a reboot it continues after the newest event
  in flash (no extra flash write: the page header stores the seq of its first
  event, the following events count on).
* An edge that could not be stored (pending queue full during a burst of more
  than 32 edges) keeps its seq **unused**: the receiver sees a gap.
* A page evicted from the full ring takes its events with it. They are counted
  in `lost` (RAM counter) and the oldest retained seq (`oldest_seq`) moves on.
* Known blind spot: edges still waiting in RAM (milliseconds after the edge)
  when the device resets give their seqs to the next events; that loss leaves
  no gap.

## fPort 4 formats

Every batch is self-contained; a large batch is split into several frames,
each with its own `seq_first`. A frame never exceeds the current DR budget
(min of 64 B and the LoRaWAN max payload, e.g. 51 B at EU868 DR0-2). If the DR
drops after the batch was laid out and the radio refuses a frame, its events are
re-sent as a replay sized for the new budget. If not even one event fits
(US915 DR0, 11 B) the upload waits for the next flush and nothing is marked sent.

### raw (`event_format raw`, first byte 0x03)

| offset | size | field |
|---|---|---|
| 0 | 1 | version `0x03` |
| 1 | 4 | `base_time` u32 LE (unix s when synced, else uptime s) |
| 5 | 1 | flags: bit0 time synced, bit1 replay, bit2 per-event seq delta |
| 6 | 4 | `seq_first` u32 LE: seq of the first event |
| 10 | 4 | `oldest_seq` u32 LE, **only when replay** |
| … | 3 or 4 | per event |

Per event:

| size | field |
|---|---|
| 2 | `rel_s` u16 LE, seconds after `base_time` |
| 1 | `edge << 7 \| rule` (rule 0..15, 126 = low battery, 127 = watchdog) |
| 1 | seq delta from the previous event, **only with flag bit2** (replay of non-consecutive events) |

Header 10 B (14 B for a replay), 3 B per event: 18 events fit 64 B (17 in a replay).
A raw batch spans at most 65535 s (`rel_s` is 16 bits); a longer one is split.

### full / compact (first byte 0x01 / 0x02)

Protobuf `AlarmReport` (the fPort-3 message) after the version byte:

| field | id | meaning |
|---|---|---|
| `base_time`, `total`, `events`, `time_synced`, `page_index`, `page_count` | 1–6 | as on fPort 3 |
| `seq_first` | 7 | seq of the first event in this frame |
| `replay` | 8 | answer to `req_events` |
| `oldest_seq` | 9 | oldest retained seq (replay) |
| `AlarmEvent.seq` | 12 | the event's own seq, only where it is not previous + 1 |

`full` carries the whole event (slot, channel, type, value). `compact` (version
byte 0x02) carries only `rule`, `edge`, `rel_s`: the rule index identifies the
rest. 8 events per frame.

### Decoder output

```json
{ "format": "raw", "base_time": 1794254190, "time_synced": true,
  "seq_first": 81, "total": 3,
  "events": [ { "seq": 81, "rule": 2, "event": "activate",   "time": 1794254190 },
              { "seq": 82, "rule": 3, "event": "activate",   "time": 1794254192 },
              { "seq": 83, "rule": 0, "event": "deactivate", "time": 1794254192 } ] }
```

A replay adds `"replay": true, "oldest_seq": N`.

### Size comparison (motion every 60 s = 2 edges/min, upload every 15 min = 30 events)

| format | frames | bytes |
|---|---|---|
| full | 4 | ~200 B |
| compact | 4 | ~160 B |
| raw | 2 | 10 + 30 × 3 = 100 B |

## Re-sending events: `req_events` (downlink, fPort 85)

The device re-sends retained events as fPort-4 frames with the replay flag.
One selector per request:

| selector | fields | use |
|---|---|---|
| time window | `from_unix`, `to_unix` | audit of a period (unsynced events always pass) |
| seq ranges | `ranges`: pairs `[seq_from, count]`, max 8 | missing whole frames |
| single seqs | `seqs`: ascending, max 20; on the wire first absolute, then deltas | scattered missing events |

```json
{ "seq": 41, "command": "req_events", "req_events": { "ranges": [[20, 3], [77, 6]] } }
{ "seq": 42, "command": "req_events", "req_events": { "seqs": [13, 50, 100, 131] } }
{ "seq": 43, "command": "req_events", "req_events": { "from_unix": 1794224900, "to_unix": 1794225100 } }
```

Both fit one 51 B downlink (DR0). The answer:

* selected events still in flash, in seq order, `replay = true`, `oldest_seq`;
* events that never existed (dropped seqs, seqs not reached yet) are skipped;
* nothing selected: one **empty replay frame** with `oldest_seq` and
  `seq_first` = the next seq the device will assign.

Errors (fPort 85): `BAD_REQUEST` for an invalid selector, `NOT_READY` outside
LoRaWAN.

### Recovering lost events (receiver side)

1. Remember the highest seq received per device.
2. A frame whose first seq is higher than expected = gap. Ask for the missing
   seqs (`ranges` for runs, `seqs` for single ones). One request per gap is
   enough, the device answers within the duty-cycle budget.
3. Seqs below `oldest_seq` of the answer are gone for good (evicted).
4. Seqs not returned while `>= oldest_seq` never existed (dropped in a burst).
5. Duplicates (replays, repeated uplinks) are identified by seq.

Example (verified on HW): frames with seq 2433..2437 were lost while the device
was disabled on the network server.

```text
uplink  fPort 4  seq_first 2430, 3 events        -> expect 2433 next
uplink  fPort 4  seq_first 2438, 3 events        -> gap 2433..2437 (5)
downlink fPort 85 08f7038a02051a03811305
         = { "seq": 503, "command": "req_events", "req_events": { "ranges": [[2433, 5]] } }
uplink  fPort 4  seq_first 2441, 1 event         (downlink rides on this uplink's RX window)
uplink  fPort 4  replay, seq_first 2433, 5 events -> gap filled
```

Receiver sketch (JavaScript, `decodeUplink` from `ttn.js`):

```js
// state per device: { next: null, missing: [[from, count], ...] }
function onEventFrame(st, d) {            // d = decodeUplink(...).data, fPort 4
  if (d.replay) {
    d.events.forEach(e => fill(st, e.seq));                    // remove from st.missing
    st.missing = st.missing.filter(([f, n]) => f + n > d.oldest_seq); // evicted = gone
    return;
  }
  if (st.next !== null && d.seq_first > st.next)
    st.missing.push([st.next, d.seq_first - st.next]);         // lost frame(s)
  else if (st.next !== null && d.seq_first < st.next && !isDuplicate(st, d))
    st.missing = [];                                           // new log (log_split change)
  st.next = d.seq_first + d.events.length;
}
// Then, when st.missing is not empty and no request is pending (or it timed out):
// encodeDownlink({ data: { seq: n, command: "req_events",
//                          req_events: { ranges: st.missing.slice(0, 8) } } })  -> fPort 85
```

Retry an unanswered request a few times (the device may not have had an
uplink to receive it on), then give the range up.

There is no automatic acknowledgement: a LoRaWAN confirmed uplink would only
confirm delivery to the network server and costs gateway downlink airtime, so
the PoC relies on seq + `req_events`.

## NFC read: `req_events_page`

Client-driven, stateless (like `req_history_page`). Each tap returns one
`Response.event_page` (an `AlarmReport`) with up to 6 events:

| request field | meaning |
|---|---|
| `start_seq` | cursor (omitted = oldest retained) |
| `from_unix`, `to_unix` | optional time window |

| reply field | meaning |
|---|---|
| `events`, `seq_first`, `base_time`, `time_synced` | as on fPort 4 (full) |
| `oldest_seq` | oldest retained seq |
| `next_seq` | pass back as `start_seq` for the next page |
| `has_more` | false = the end of the log |

```json
{ "seq": 5, "command": "req_events_page", "req_events_page": { "start_seq": 81 } }
```

Loop: `start_seq = 0` → read → `start_seq = next_seq` while `has_more`. Reading
over NFC does not change what is uploaded over LoRaWAN. Note: the event log uses
the NFC mailbox (ST25DV FTM), so the reader must support raw ISO 15693 custom
commands (for example ACS ACR1552U or a phone).

## Duty cycle

The event log only queues frames. `app_radio` keeps one sliding-hour ledger
(EU868: 1 % = 36 s/h) and holds a frame until it fits; LoRaMAC additionally
enforces the per-sub-band limits. A large backlog (hundreds of events) is
therefore spread over time automatically.

## Shell (debug / bench images)

| command | |
|---|---|
| `eventlog info` | state, next / oldest seq, seq span of every page |
| `eventlog flush` | upload pending events now |
| `eventlog req time <from> <to>` / `rng <seq> <count> …` / `seq <seq> …` | replay as by downlink |
| `eventlog page [seq]` | what one NFC read returns |
| `alarm sim <rule> on\|off [low\|high\|trigger] [value]` | inject an alarm edge |
| `accel sim <s> [period_ms]` | simulate accelerometer motion |

The bench image with the flash ring:
`west build -b sticker sticker/app -- -DEXTRA_CONF_FILE="debug.conf;debug-history-flash.conf;debug-evlog.conf" -DEXTRA_DTC_OVERLAY_FILE=debug-history-flash.overlay`
(16 KB partition = 8 pages: `log_split 25` gives 2 pages there).

## Tests

* `tests/eventlog` (native_sim flash simulator): seq, reboot recovery, overflow
  gap, eviction, replay selectors, raw framing, NFC export paging.
* `tests/cmd`: `req_events` / `req_events_page` dispatch, NFC page size.
* `app/decoder/ttn.test.js`: all fPort-4 formats, replay, `req_events`,
  `req_events_page`.
