# Design: table-driven config apply/fill (#262)

> Scaffold / working design note. To be refined during implementation and
> removed (or folded into `scripts/west_commands/README`) before this PR leaves
> draft.

## Goal

Replace the per-field **generated code** in `app_config_ingest.c` (and later the
settings + debug-shell duplication in `app_config.c`) with a per-field **rodata
descriptor table** walked by one hand-written generic interpreter.

Target saving: **2–4 KB release flash** (release is at 99.93 % @ `3a25890`),
more in debug where the shell handlers deduplicate over the same table. No
wire-format change of any kind — verifiable 1:1 by the existing configen pytest
round-trip suite and the `tests/cmd` ztest.

## Current shape (baseline @ `3a25890`)

`scripts/west_commands/templates/config_ingest.c.j2` emits, per field, an inline
block whose shape is always: `has_` guard → write-transport check (M-3, #172) →
range/enum/bytes handling → assignment or `FAULT(tag)`. The same field metadata
is repeated on the fill (read) side and a third time in `config.c.j2`
(settings load/save + shell).

The metadata is already computed in `configen.py` (`_ingest_model`, ~line 629)
and is exactly what a descriptor needs:

| descriptor field | source in configen param dict |
|---|---|
| `tag`            | `p["tag"]` (`proto_id`) |
| `kind`           | `p["kind"]` — `bool`/`enum`/`bytes`/`float`/`int_ranged`/`plain` |
| `has_off`        | `offsetof(<c_message>, has_<proto_name>)` |
| `src_off`        | `offsetof(<c_message>, <proto_name>)` |
| `dst_off`        | `offsetof(struct app_config, <c_name>)` |
| `size`           | `sizeof` value / bytes length |
| `min` / `max`    | `p["min"]` / `p["max"]` (INT), `0` / `enum_max` (ENUM) |
| `wr_transports`  | derived from `no_write_lrw` / `no_write_nfc` |
| flags            | `zero_allowed`, `omit_if_zero`, `dump`, `dump_nfc_only` |

## Plan

### Phase 1 — apply/fill interpreter (this PR, core ~2–4 KB)
1. configen emits `struct cfg_field_desc <group>_desc[]` (rodata) per group,
   plus a group table (`{ desc, n, c_message size }`), instead of the inline
   `apply_*` / `fill_*` bodies.
2. Hand-write the generic interpreter in a **non-generated** file
   (`app/src/app_config_table.c` + header): `cfg_apply(desc, n, tp, src, config,
   fault)` and `cfg_fill(desc, n, dst, ids, n_ids, config)`. Enum validates via
   `min..max` and casts on assignment; bytes via `size` + `memcpy`.
3. `app_config_apply_<group>()` / `fill_<group>()` become thin wrappers that call
   the interpreter with the group's descriptor table (keeps the public API and
   `app_config_ingest.h` stable).
4. Keep the omit-if-zero `slot_empty` helper driven off the same table.

### Phase 2 — settings load/save (follow-up)
Drive the `settings_*` load/save handlers in `config.c.j2` off the same
descriptor (add the settings key name to the descriptor).

### Phase 3 — debug shell (follow-up, biggest *debug* win)
Deduplicate the `cmd_*` / `print_*` shell handlers over the table.

## Explicitly NOT in the table (stay hand-written / generated)
- `app_config_alarms_slot_empty()` special casing.
- Claim-token write-once semantics (#170).
- NFC-only key readback lists (#162).

## Gotchas
- Template work MUST use `west configen -t <worktree>/scripts/west_commands/templates`
  — the default lookup uses the manifest repo and silently ignores worktree
  template edits.
- `__packed` the descriptor so the rodata cost is the ~14–16 B/field claimed;
  measure actual `.rodata` vs `.text` delta with `arm-zephyr-eabi-size` on the
  release build before/after.
- Regenerate via local `python -c` (not `west configen` / `tools/test.sh`) to
  avoid the known stale-dispatch rewrite of `app_cmd.c`.

## Validation
- `pytest scripts/west_commands/tests` (configen round-trip).
- `tests/cmd` ztest on `native_sim/native/64`.
- Release build flash delta via `arm-zephyr-eabi-size`.
- Diff the generated `.pb.c`/proto — must be byte-identical (no wire change).
