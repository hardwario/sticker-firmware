# Design: commands in configen (YAML-driven command schema)

**Status:** proposal (2026-06-11). No code generated yet — design only, per request.
**Context:** sticker-firmware, after the config regroup + ingest codegen (PR #105).
Goal: let `app_config.yml` also define **which commands exist / are usable**, so the
command protocol stops drifting across three hand-maintained places.

## 1. Why

Commands (the `Command`/`Response` protobuf protocol on LoRaWAN fPort 85 + NFC) are
hand-maintained in **three** places that must stay in lockstep — the exact drift the
config-param codegen just eliminated:

1. **proto** (`app_config.proto`, hand-written region): `Command` oneof + each
   sub-message (`SetParam`, `GetParam`, `ReqHistory`, `AlarmRule`, …) and `Response`.
2. **firmware** (`app_cmd.c`): the `dispatch()` `switch (cmd->which_body)` + the
   `handle_*` functions.
3. **decoder** (`ttn.js`): `_CMD_NAMES`/`_CMD_TAGS` + per-command encode/decode.

A new command, a renamed one, or a removed one means editing all three (and the
manager-app). "Select which commands are usable" = today, hand-edit the oneof + switch.

## 2. Current command inventory (source of truth for the YAML)

| name | proto_id | body msg | kind | response | notes |
|------|----------|----------|------|----------|-------|
| set_param | 2 | SetParam | handler | ack | applies 4 config submessages, may set SETTINGS_SAVE |
| get_param | 3 | GetParam | handler | config_dump | |
| get_info | 4 | GetInfo (empty) | builtin | **info** | Info only, **no ack** (already so) |
| get_config | 5 | GetConfig | handler | config_dump | paged |
| settings_save | 6 | SettingsSave (empty) | action | ack | → APP_CMD_ACTION_SETTINGS_SAVE |
| reboot | 7 | Reboot (empty) | action | ack | → APP_CMD_ACTION_REBOOT |
| factory_reset | 8 | FactoryReset (empty) | action | ack | reset application/sensors/alarms to defaults but **keep device + lorawan** (the `preserve_on_migration` set) so the device stays provisioned & connected — **not** a full NVS wipe (see §4a) |
| force_send | 9 | ForceSend (empty) | handler | **none** | just trigger telemetry send — **no ack** (the telemetry IS the answer; saves an uplink) |
| reset_counters | 10 | ResetCounters | handler | ack | resets hall/input counters |
| req_history | 11 | ReqHistory | handler | none | **transport-aware** (LRW only); the HistoryFrames are the answer |
| clock_sync | 12 | ClockSync (empty) | handler | **info (deferred)** | force RTC resync, **no ack**; send an Info uplink **once the sync lands** (carries the synced unix_time) |
| alarm_rule | 13 | AlarmRule | handler | ack | sets APP_CMD_ACTION_ALARM_RULES_SAVE |
| **w1_scan** | **14** | W1Scan (empty / opt timeout) | handler | **w1_scan** (new) | **NEW**: enumerate the 1-Wire bus, return discovered ROMs; the app then teaches a slot via `set_param sensorN_rom=<rom>`. Needs a new `Response.W1Scan { repeated bytes rom }`. Big over LRW (N×8 B) → prefer NFC/shell or cap N. |

Three **kinds**: `action` (set *action), `builtin` (generated response, e.g. get_info),
`handler` (custom C). Some handlers need `transport`. **Response is orthogonal to kind** —
`ack` / `info` / `config_dump` / `none` — and is kept minimal to save uplinks: a command
whose real answer is the data it triggers emits **no ack** (`force_send`, `req_history`).
`clock_sync` returns the **synced time via Info**, deferred until the DeviceTime answer lands.

## 3. Proposed YAML `commands:` section

```yaml
commands:
  message: Command          # wrapper (existing)
  response: Response        # response wrapper (existing)
  list:
    - {name: set_param,      proto_id: 2,  body: SetParam,      kind: handler, response: ack}
    - {name: get_param,      proto_id: 3,  body: GetParam,      kind: handler, response: config_dump}
    - {name: get_info,       proto_id: 4,  body: GetInfo,       kind: builtin, response: info}
    - {name: get_config,     proto_id: 5,  body: GetConfig,     kind: handler, response: config_dump}
    - {name: settings_save,  proto_id: 6,  body: SettingsSave,  kind: action, action: SETTINGS_SAVE, response: ack}
    - {name: reboot,         proto_id: 7,  body: Reboot,        kind: action, action: REBOOT, response: ack}
    - {name: factory_reset,  proto_id: 8,  body: FactoryReset,  kind: action, action: FACTORY_RESET, response: ack}
    - {name: force_send,     proto_id: 9,  body: ForceSend,     kind: handler, response: none}
    - {name: reset_counters, proto_id: 10, body: ResetCounters, kind: handler, response: ack}
    - {name: req_history,    proto_id: 11, body: ReqHistory,    kind: handler, response: none, transports: [lrw]}
    - {name: clock_sync,     proto_id: 12, body: ClockSync,     kind: handler, response: info_deferred}
    - {name: alarm_rule,     proto_id: 13, body: AlarmRule,     kind: handler, action: ALARM_RULES_SAVE, response: ack}
    - {name: w1_scan,        proto_id: 14, body: W1Scan,        kind: handler, response: w1_scan}
```

Per-entry fields:
- `name` / `proto_id` — wire identity (append-only; same no-renumber guard as config).
- `body` — the protobuf sub-message type (hand-written, see §4 caveat).
- `kind` — `action` | `builtin` | `handler`.
- `action` — `APP_CMD_ACTION_*` to set (for `action`, or a handler that also defers).
- `transports` — **optional allow-list** (`lrw`/`nfc`/`shell`); omitted = all. The
  "which commands are usable [where]" knob (e.g. `req_history` LRW-only).
- `response` — the success response body, **kept minimal to save uplinks**:
  - `ack` — empty confirmation;
  - `info` / `config_dump` — a data response (no separate ack);
  - `none` — **emit nothing** (`which_body` stays 0; the command's effect — telemetry,
    history frames — is the answer). Used by `force_send`, `req_history`;
  - `info_deferred` — no immediate response; an Info uplink is sent later, when the
    triggered async op completes (`clock_sync` → after the DeviceTime answer lands, the
    Info carries the freshly-synced `unix_time`).

**Selecting which commands are usable = add/remove a list entry (or set `transports`).**
**No redundant acks** — only `ack` when there is genuinely nothing else to return.

## 4. What configen generates vs stays hand-written

**Generated:**
- **proto**: the `Command { oneof body { … } }` field list (from `list`, in proto_id order,
  with reserved tags for removed commands). References the hand-written sub-message types.
- **decoder `ttn.js`**: `_CMD_NAMES` / `_CMD_TAGS` maps from the list.
- **firmware dispatch**: a generated `app_cmd_dispatch()` (replaces the hand `switch`) that
  per kind:
  - `action` → set `*action = APP_CMD_ACTION_<action>` and emit `response` (usually `ack`);
  - `builtin` → emit the named `response` (e.g. `fill_info` for `info`);
  - `handler` → call `app_cmd_handle_<name>(transport, cmd, resp, action)` (or a trimmed
    signature) implemented by hand; the handler fills the declared `response`;
  - enforce `transports` → `UNSUPPORTED_FIELD`/`BAD_REQUEST` if the command arrives on a
    disallowed transport (closes the get-it-only-on-LRW logic that req_history hand-codes).
  - `response` routing: `none`/`info_deferred` leave `which_body == 0` (no immediate uplink —
    `app_cmd_handle` already treats 0 as "emit nothing"). For `info_deferred` the async
    side (e.g. the clock module's sync-complete hook) sends the Info uplink later via the
    existing autonomous-Info path (`app_cmd_build_info` + `app_lrw_queue_response`).
- C **command tag constants / a registry table** + a `BUILD_ASSERT` that every `handler`
  has its `handle_*` defined (link error otherwise).

**Stays hand-written (codegen can't own the logic):**
- The **sub-message bodies** (`SetParam{…}`, `ReqHistory{…}`, `AlarmRule{…}`) — their fields
  vary too much; describing them in YAML would re-invent proto. configen generates the oneof
  and references them; the messages live in the hand-written proto region (as today).
  *(Optional later: generate trivial empty bodies — GetInfo/Reboot/… — from `kind`.)*
- The **handler bodies** (`handle_set_param`, `handle_req_history`, …) — custom C.
- The per-command **encode/decode of complex bodies** in `ttn.js` (SetParam/AlarmRule/…);
  the name↔tag map + empty-body commands are generated.

So "everything in one place" applies to the **command list / wire identity / dispatch
routing / availability**, not the per-command payload logic.

## 4a. factory_reset semantics (keep device + lorawan)

The remote `factory_reset` command must **not de-provision** the device — it resets the
application/sensors/alarms config to defaults but **preserves device identity and the
LoRaWAN keys**, so the device keeps its connection after the reboot.

- Preserved set = exactly the **`preserve_on_migration`** fields already flagged in the
  YAML (secret_key, serial_number, nonce_counter, all `lrw_*`). configen already generates
  that restore whitelist for the schema-migration path in `h_commit` (#87) — reuse it:
  generate an `app_config_factory_defaults()` that assigns `m_app_config_defaults` then
  restores the `preserve_on_migration` fields from the current config. The
  `APP_CMD_ACTION_FACTORY_RESET` handler calls it + `settings_save` + reboot.
- This replaces the current behaviour (full NVS erase via `app_settings_reset`, which wipes
  identity too). A **true full wipe** (incl. identity / de-provision) stays **shell/SWD-only**
  (e.g. `settings reset`), never reachable over LRW/NFC.
- Synergy: same generated whitelist powers schema migration (#87) and factory_reset — one
  source of truth for "what survives a reset".

## 4b. w1_scan command (discover ROMs → teach via set_param)

Remote equivalent of the shell `w1 enroll` / auto-detect: scan the 1-Wire bus and return
the discovered ROM addresses so the app can bind them to slots.

- **Command** `w1_scan` (proto_id 14), empty body (optional `timeout_ms`). `kind: handler`
  → `handle_w1_scan()` runs a ROM search on the DS2484 bus, collects the addresses.
- **New response** `Response.W1Scan { repeated bytes rom = 1; }` (8-byte ROMs; optionally a
  per-ROM `family`/`type` byte so the app can pre-suggest a slot type). nanopb options:
  `max_count` (e.g. 8) + `max_size:8`. This is a **new Response oneof body** (e.g. tag 7).
- **Workflow:** `w1_scan` → app shows the ROM list → user picks a slot → `set_param`
  `sensors{ sensorN_rom = <rom hex> }` (the teach) → save. Mirrors the existing slot model
  (`app_w1_slots`, ROM-bound slots) and the `sensorN_rom` config fields.
- **Payload note:** N ROMs × 8 B is large for LRW DR0 — either cap N, page it (like
  ConfigDump), or restrict `transports: [nfc, shell]`. Decide per real bus size.
- **Response-codegen tie-in:** this is the first NEW Response body since the design; it
  reinforces open question #4 — either keep `Response` hand-written (add `W1Scan` by hand,
  reference it via the command's `response: w1_scan`) or let configen own the `Response`
  oneof too. Recommendation: keep `Response` bodies hand-written for now; only the
  command→response *wiring* is generated.

## 5. Decoder & manager-app impact

- `ttn.js`: `_CMD_NAMES`/`_CMD_TAGS` become generated; the `encodeDownlinkCommand`
  body-builders for complex commands stay (keyed by name).
- **manager-app** (Flutter, gitlab tester/manager-app, tightly coupled): regenerates Dart
  from the proto; the command list for its UI could also be driven by the YAML `commands:`
  (a natural fit for the schema-driven config window, manager-app #5). Mirror via #7 sync.

## 6. Phasing (own PR, after #105 lands; command dispatch is the control path → verify hard)

1. **Decoder + constants**: generate `_CMD_NAMES`/`_CMD_TAGS` + C command-tag table from
   `commands.list`. No behaviour change; locks the name↔tag map. (lowest risk)
2. **Dispatch table**: replace the hand `switch` with a generated table calling `handle_*` /
   setting actions / gating transports. Adversarially verify against the current switch
   (every command routes identically). Add a ztest per command kind.
3. **proto oneof**: generate the `Command` oneof from the list (append-only guard, reserved
   on removal). Sub-message bodies stay hand-written.
4. *(optional)* generate trivial empty bodies + `Response` oneof.

## 9. Generated files & the app_cmd style

configen output files (current + new). Same model as `app_config_ingest.c`: generated `.c`
files of *wiring*, hand-written `.c` files of *logic*. **`app_cmd.c` is NOT regenerated.**

| file | gen/hand | content |
|------|----------|---------|
| `app_config.c` / `.h` | gen | struct, shell, settings, h_commit (+`app_config_factory_defaults()`) |
| `app_config.proto` | **merged** | 2 generated marked regions — `AppConfigMessage` **and `Command` oneof body** — between markers; sub-message bodies + `Response` hand-written, preserved |
| `app_config.options.in` | merged | nanopb options (bytes max_length, GetParam/W1Scan max_count) — generated region + hand part |
| `app_config_ingest.c` | gen | `apply_`/`fill_` per config submessage |
| **`app_cmd_dispatch.c`** | **gen (NEW)** | the command dispatch table + `app_cmd_dispatch()`; calls hand-written handlers |
| **`app_cmd_handlers.h`** | gen (NEW) | prototypes of every `handler` command's `app_cmd_handle_<name>()` (+ `BUILD_ASSERT` glue) |
| `app_cmd.c` | **hand** | the `handle_*` bodies, `make_error`, `fill_info`, `build_*` — unchanged in style |
| `ttn.js` | merged | `_CMD_NAMES`/`_CMD_TAGS` generated region; complex body encode/decode hand |

**Generated `app_cmd_dispatch.c` — style (table-driven, uniform handler signature):**

```c
/* GENERATED by configen from app_config.yml `commands:` — do not edit. */
#include "app_cmd.h"
#include "app_cmd_handlers.h"   /* hand-written app_cmd_handle_<name>() prototypes */
#include "src/app_config.pb.h"

#define T(x) (1u << APP_CMD_TRANSPORT_##x)

typedef void (*cmd_fn)(enum app_cmd_transport, const Command *, Response *,
                       enum app_cmd_action *);

static const struct {
    uint32_t          which;       /* Command_<name>_tag */
    uint8_t           transports;  /* allowed mask, 0 = any */
    cmd_fn            handler;     /* NULL => pure action/ack */
    enum app_cmd_action action;    /* APP_CMD_ACTION_* or _NONE */
} CMD_TABLE[] = {
    { Command_set_param_tag,     0,        app_cmd_handle_set_param,     APP_CMD_ACTION_NONE },
    { Command_get_info_tag,      0,        app_cmd_handle_get_info,      APP_CMD_ACTION_NONE },
    { Command_settings_save_tag, 0,        NULL,                         APP_CMD_ACTION_SETTINGS_SAVE },
    { Command_reboot_tag,        0,        NULL,                         APP_CMD_ACTION_REBOOT },
    { Command_factory_reset_tag, 0,        NULL,                         APP_CMD_ACTION_FACTORY_RESET },
    { Command_req_history_tag,   T(LRW),   app_cmd_handle_req_history,   APP_CMD_ACTION_NONE },
    { Command_w1_scan_tag,       0,        app_cmd_handle_w1_scan,       APP_CMD_ACTION_NONE },
    /* … one row per commands.list entry … */
};

void app_cmd_dispatch(enum app_cmd_transport tp, const Command *cmd, Response *resp,
                      enum app_cmd_action *action)
{
    for (size_t i = 0; i < ARRAY_SIZE(CMD_TABLE); i++) {
        if (CMD_TABLE[i].which != cmd->which_body) {
            continue;
        }
        if (CMD_TABLE[i].transports && !(CMD_TABLE[i].transports & T_MASK(tp))) {
            app_cmd_make_error(resp, Response_Error_Code_UNSUPPORTED_FIELD, "transport");
            return;
        }
        if (CMD_TABLE[i].handler) {
            CMD_TABLE[i].handler(tp, cmd, resp, action);   /* fills resp (ack/info/none) */
        } else {
            *action = CMD_TABLE[i].action;                 /* pure action -> ack */
            resp->which_body = Response_ack_tag;
        }
        return;
    }
    app_cmd_make_error(resp, Response_Error_Code_NOT_IMPLEMENTED, "command");
}
```

Notes on the style:
- **Uniform handler signature** `(transport, cmd, resp, action)` (open Q#2 resolved) → the
  table is just data; handlers that ignore `transport`/`action` simply don't use them.
- **`builtin` collapses into `handler`** (e.g. `get_info` → `app_cmd_handle_get_info` builds
  the Info; `force_send` → handler leaves `which_body == 0` = no uplink). So at the table
  level there are only two cases: **handler** (call fn) or **pure action** (set action + ack).
  The `response:`/`kind: builtin` YAML fields then mainly drive the **decoder/manager-app**
  (what to expect) and the docs — the FW dispatch only needs handler-ptr + action + transports.
- `app_cmd.c` exposes `app_cmd_make_error` / `fill_info` (un-static a couple of helpers the
  generated dispatch calls) and implements each `app_cmd_handle_<name>()`.
- `app_cmd_handle()` (the public entry) is hand-written and simply decodes then calls the
  generated `app_cmd_dispatch()` — replacing today's inline `switch`.
- `app_cmd_handlers.h` (generated) declares every handler + a `BUILD_ASSERT`/linker guarantee
  that adding a `handler` command to the YAML forces you to implement its `handle_*`.

## 8. FW impact

Two independent tracks — the **behaviour changes** need no codegen (plain `app_cmd.c`/proto
edits, shippable now); the **codegen** is the maintainability layer on top.

### A. Behaviour changes (no codegen required)
- **force_send** → drop the ack: handler just triggers the send, leave `resp->which_body == 0`
  (already means "emit nothing"). −1 uplink. *(app_cmd.c, ~2 lines)*
- **clock_sync** → drop the ack; add a **sync-complete hook** in `app_clock`: when the
  DeviceTimeAns lands, send an Info uplink (`app_cmd_build_info` + `app_lrw_queue_response`).
  *New wiring in app_clock + a callback; the only non-trivial behaviour change.*
- **factory_reset** → stop full-erasing NVS; reset to defaults but **keep device + lorawan**
  (the `preserve_on_migration` set), save + reboot. Add `app_config_factory_defaults()`
  (defaults + restore preserved fields — same whitelist as the #87 migration). Keep the true
  full wipe shell-only (`settings reset`). *(app_cmd action handler + one config helper)*
- **w1_scan** → new command + new `Response.W1Scan { repeated bytes rom }` (proto + nanopb
  options) + `handle_w1_scan()` (DS2484 ROM search, reuse the app_w1_slots/ds2484 enumeration)
  + decoder W1Scan decode + the empty-body command encode. Decide transport/paging for size.
  *(largest behaviour item — a new command end-to-end)*
- **get_info** — no change (already Info, no ack).

### B. Codegen (configen `commands:` — the drift killer)
- New `build_commands_model()` + templates that generate, from `commands.list`:
  - the proto `Command` oneof (append-only guard, reserved on removal);
  - the decoder `_CMD_NAMES`/`_CMD_TAGS`;
  - a C dispatch table replacing the hand `switch (cmd->which_body)` — routes by `kind` /
    `response` / `transports`, calls `app_cmd_handle_<name>()`, sets `action`, enforces
    transport gating, handles `response: ack/info/none/info_deferred`;
  - `BUILD_ASSERT` every `handler` command has its `handle_*` defined.
- Sub-message bodies (`SetParam{…}`, `W1Scan{…}`, …) + `Response` bodies + handler logic
  stay hand-written; configen owns only the **list / wire identity / routing / availability**.
- The same configen run already knows `preserve_on_migration` → also generate
  `app_config_factory_defaults()` (ties A.factory_reset to B).

### Files touched
- `app_config.yml` (+`commands:` section), `scripts/west_commands/configen.py` +
  `config_*.c.j2` (gen), `app_config.proto` (Command oneof generated + new W1Scan body
  hand-written), `app_cmd.c` (switch → generated dispatch; handlers kept; +handle_w1_scan),
  `app_clock.*` (clock_sync hook), `app_config.{c,h}` (factory_defaults), `ttn.js` (gen maps
  + W1Scan decode), ztest/pytest/node, options.in (W1Scan max_count). manager-app mirrors.

### Risk & phasing (own PR after #105)
Dispatch is the **control path**. Phase: (1) decoder maps + C tag constants (no behaviour
change); (2) the 4 behaviour changes above (force_send/clock_sync/factory_reset/w1_scan) —
shippable without codegen; (3) generated dispatch table — **adversarially verify it routes
every command identically to the current switch** (bar the intended changes) + ztest per kind;
(4) generated proto Command oneof. Steps 2 and 3 are separable: behaviour first, codegen second.

## 7. Open questions
- Keep complex body encode/decode hand-written in `ttn.js`, or describe simple bodies in YAML too?
- Handler signature: unify to `(transport, cmd, resp, action)` so the generated table is uniform?
- Transport gating: enforce in the generated dispatch (recommended) vs leave to handlers.
- Where do `Response` bodies live in the YAML (or stay fully hand-written)?
