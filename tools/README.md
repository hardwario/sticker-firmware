# tools

## `test.sh` — run everything CI checks, locally

```bash
source ~/.venv/bin/activate     # west, pytest, node on PATH
tools/test.sh                   # full run
SKIP_BUILD=1 tools/test.sh      # skip the slow firmware builds
```

Steps (exits non-zero on the first hard failure):

1. **Firmware builds** — release + debug (`west build -p always -b sticker .`).
2. **Decoder tests** — `node --test` in `app/decoder/` (the LoRaWAN codec).
3. **configen tests** — `pytest scripts/west_commands/tests` (generator + proto round-trip).
4. **clang-format** — `--dry-run --Werror` over `app/src/*.{c,h}`; fails on any drift.
5. **configen in sync** — regenerate from `app_config.yml` and fail if the committed
   `app_config.{c,h,proto,options.in}`, `app_cmd.c` (dispatch region), or the decoder's
   command map (`app/decoder/ttn.js`) would change. Imports `scripts/west_commands/configen.py`
   directly by path rather than shelling out to `west configen` — the latter resolves to
   whichever checkout the west manifest's `self` project points at, which in a git worktree
   is a *different* checkout than this one, so it would silently regenerate from the wrong
   copy of the generator.

The CI `test` job (`.github/workflows/build.yml`) runs steps 2–4 (no Zephyr toolchain);
this script is the fuller local superset that also builds and checks generated-file sync.

Install the Python test deps once: `pip install -r scripts/west_commands/requirements-dev.txt`.

## clang-format pre-commit hook

`tools/hooks/pre-commit` auto-runs `clang-format -i` on staged `app/src/*.{c,h}` before
each commit (CI then verifies the same with `--dry-run --Werror`). Enable once per clone:

```bash
git config core.hooksPath tools/hooks
```
