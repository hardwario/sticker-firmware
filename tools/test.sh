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
if have west; then
  banner "configen output in sync"
  if west configen "$REPO/app/src/app_config.yml" -o "$REPO/app/src/" >/dev/null 2>&1 \
     && git -C "$REPO" diff --quiet -- \
        app/src/app_config.c app/src/app_config.h \
        app/src/app_config.proto app/src/app_config.options.in; then
    ok "configen output in sync"
  else
    bad "configen output in sync — regenerate and commit (git diff app/src/app_config.*)"
  fi
else
  skip "configen sync — 'west' not found"
fi

banner "summary"
printf 'pass=%d fail=%d skip=%d\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
