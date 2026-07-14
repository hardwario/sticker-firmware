#!/usr/bin/env bash
#
# Run everything CI checks, locally, with one command (issue #51, part 4).
#
#   tools/test.sh              # full run: builds + node + pytest + format + sync
#   SKIP_BUILD=1 tools/test.sh # skip the slow firmware builds (quick iteration)
#
# Requires (same as CI): west + Zephyr SDK (for builds), node, python with
# scripts/west_commands/requirements-dev.txt installed. Activate the venv first
# (e.g. `source ~/.venv/bin/activate`). Exits non-zero on the first failure.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

PASS=0
FAIL=0
SKIP=0

banner() { printf '\n\033[1;36m=== %s ===\033[0m\n' "$1"; }
ok()     { printf '\033[1;32mPASS\033[0m %s\n' "$1"; PASS=$((PASS + 1)); }
bad()    { printf '\033[1;31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL + 1)); }
skip()   { printf '\033[1;33mSKIP\033[0m %s\n' "$1"; SKIP=$((SKIP + 1)); }
have()   { command -v "$1" >/dev/null 2>&1; }

run() { # run <label> <cmd...>
  local label="$1"; shift
  banner "$label"
  if "$@"; then ok "$label"; else bad "$label"; fi
}

# 1. Firmware builds (release + debug)
if [ "${SKIP_BUILD:-0}" = "1" ]; then
  skip "firmware builds (SKIP_BUILD=1)"
elif ! have west; then
  bad "firmware builds — 'west' not found (activate the venv / Zephyr env)"
else
  run "build release" bash -c "cd '$REPO/app' && west build -p always -b sticker ."
  run "build debug"   bash -c "cd '$REPO/app' && west build -p always -b sticker . -- -DEXTRA_CONF_FILE=debug.conf"
fi

# 2. JS decoder tests
if have node; then
  run "decoder tests (node --test)" bash -c "cd '$REPO/app/decoder' && node --test"
else
  bad "decoder tests — 'node' not found"
fi

# 3. configen + proto pytest
if have pytest; then
  run "configen tests (pytest)" pytest "$REPO/scripts/west_commands/tests"
else
  bad "configen tests — 'pytest' not found (pip install -r scripts/west_commands/requirements-dev.txt)"
fi

# 4. C formatting — app/src must be clang-format-clean.
if have clang-format && [ -f "$REPO/.clang-format" ]; then
  mapfile -t CFILES < <(git -C "$REPO" ls-files 'app/src/*.c' 'app/src/*.h')
  run "clang-format --dry-run --Werror" clang-format --dry-run --Werror "${CFILES[@]}"
else
  skip "clang-format (not installed or no .clang-format)"
fi

# 5. Generated config in sync with app_config.yml
#
# Deliberately NOT `west configen`: the west-command extension resolves to
# whichever checkout the west manifest's "self" project points at (fixed by
# west.yml, independent of $REPO) — in a git worktree that is the OTHER
# checkout sharing this workspace, not this one. Running it here would
# silently regenerate app_config.c/h/proto/options.in/app_cmd.c/ttn.js from
# THAT (possibly older/different) copy of configen.py and overwrite this
# worktree's files with it, in place, even when this very check then reports
# FAIL. Importing scripts/west_commands/configen.py directly by path always
# uses this worktree's own copy, exactly like `west configen` would from the
# manifest project itself. Also checks app_cmd.c/ttn.js, which the old bare
# `west configen` invocation silently regenerated (via its own default output
# paths) without ever being included in the sync check below.
if have python3; then
  banner "configen output in sync"
  if python3 -c "
import sys, argparse
from pathlib import Path
sys.path.insert(0, '$REPO/scripts/west_commands')
import configen
ns = argparse.Namespace(
    yaml_file=Path('$REPO/app/src/app_config.yml'), output_dir=Path('$REPO/app/src'),
    templates_dir=None, proto=None, options=None, no_proto=False,
    decoder=Path('$REPO/app/decoder/ttn.js'), app_cmd=Path('$REPO/app/src/app_cmd.c'), dry_run=False,
)
configen.Configen().do_run(ns, [])
" >/dev/null 2>&1 \
     && git -C "$REPO" diff --quiet -- \
        app/src/app_config.c app/src/app_config.h \
        app/src/app_config.proto app/src/app_config.options.in \
        app/src/app_cmd.c app/decoder/ttn.js; then
    ok "configen output in sync"
  else
    bad "configen output in sync — regenerate and commit (git diff app/src/app_config.* app/src/app_cmd.c app/decoder/ttn.js)"
  fi
else
  skip "configen sync — 'python3' not found"
fi

banner "summary"
printf 'pass=%d fail=%d skip=%d\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
