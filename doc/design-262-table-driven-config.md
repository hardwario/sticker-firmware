# Table-driven config apply/fill/settings/shell (#262)

Replaces the per-field **generated code** in the config machinery with per-field
**rodata descriptor tables** walked by hand-written generic interpreters. No
wire-format and no NVS-format change: the descriptors encode exactly the
validation/mapping/ordering the generated code did.

## Result (measured @ base v1.4.0 `3a25890`)

| Build | Flash before | Flash after | Saved |
|-------|-------------|-------------|-------|
| Release | 99.93 % (163 720 B) | **95.40 % (156 296 B)** | **−7 424 B** |
| Debug   | ~99 %              | **92.36 % (226 972 B)** | (Phase 1+2+3) |

Per generated object (release / debug text):

| File | Before | After |
|------|--------|-------|
| `app_config_ingest.c` | 6 664 B | 1 436 B |
| `app_config_table.c` (new interpreter) | — | 534 B |
| `app_config.c` (release) | 6 040 B | 2 406 B |
| `app_config.c` (debug, incl. shell) | 12 154 B | 9 786 B |

## Phases

### Phase 1 — apply/fill interpreter (release −4 688 B)
- configen emits `struct cfg_field_desc <group>_desc[]` (rodata) per proto
  submessage group in `app_config_ingest.c`.
- Hand-written generic `cfg_apply` / `cfg_fill` / `cfg_slot_empty` in
  `app_config_table.c` walk it. `app_config_apply_*()` / `fill_*()` /
  `*_slot_empty()` become thin wrappers — public API and signatures unchanged.
- Descriptor: tag, kind (BOOL/PLAIN/RANGED/BYTES), size (`sizeof`), flags
  (no-write-lrw/nfc, dump, zero-ok, omit-if-zero), `offsetof` for has_/src_ (proto
  submessage) and dst_ (struct app_config), and [min,max] for RANGED (enums use
  [0, enum_max]). Enum = RANGED; scalar load/store via `memcpy` (no alignment /
  aliasing assumptions). float/double/64-bit are rejected loudly by configen (no
  such config field exists) rather than silently mis-generated.

### Phase 2 — settings load/save (release −2 736 B)
- One flat `struct app_config_setting { key; off; size; }` table drives both
  `h_set` (load) and `h_export` (export) as loops, replacing the per-key
  `SETTINGS_SET` / `EXPORT_FUNC` macro expansions.
- `config-version` is the last table entry, preserving the "export schema marker
  last" brownout-safety ordering. `h_commit` clamping, migration and
  `factory_reset` preserve_on_reset stay generated (unchanged, still tested).

### Phase 3 — debug shell (debug −2 392 B, release unaffected)
- One generic getter (`print_field`) + setter (`cmd_field`) walk a
  `struct cfg_shell_field[]` descriptor plus per-enum token tables, replacing the
  per-parameter `print_<name>()` / `cmd_<name>()` pair (93 → ~3 functions). Every
  `SHELL_CMD_ARG` dispatches to `cmd_field`, keyed by the sub-command name.
- Validation, error messages and display formats are preserved 1:1 (including
  `serial-number`'s zero-padded width via `%0*u`, `interval-sample`'s zero_allowed,
  `claim-token`'s write-once, and the enum token/help lists).

## Not in the tables (stay hand-written / generated)
Claim-token write-once (#170, via `cmd_bytes`), NFC-only key readback (#162, the
`CFG_F_DUMP`+DUMP_FIELDS split), `h_commit` clamp/migration, `factory_reset`.

## Validation
- configen pytest round-trip: **24/24** (incl. regen-matches-committed).
- `tests/cmd` ztest (real apply/fill/paging/nfc-only/range/transport): **15/15**.
- JS decoder: **38/38**. clang-format (CI-pinned 22.1.5) over all `app/src`: clean.
- Release + debug firmware build clean.
- **Not yet runtime-tested:** Phase 3 shell has no ztest harness (none exists for
  the shell). The code is logic-preserving; a `config <key> <val>` + `config show`
  + `settings save` smoke test on target is recommended before release.

## Gotchas (for maintainers)
- Regenerate with the **worktree** templates: `west configen -t
  <worktree>/scripts/west_commands/templates` (the default lookup uses the
  manifest repo and silently ignores worktree template edits).
- Adding a config field is now one YAML row → one descriptor row in each table;
  the access model (#172) is enforced in exactly one place (`cfg_apply`).
