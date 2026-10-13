#!/usr/bin/env bash
#
# Build + run every native_sim ztest suite (issue #51 part 3).
#
# We drive west build + the native executable directly rather than twister:
# twister mis-resolves the native_sim/native/64 board variant in this Zephyr
# fork, and the plain native_sim (32-bit) target needs gcc-multilib. west build
# of the 64-bit variant gives the same coverage and runs anywhere with a host gcc.
#
# Run from the repo root inside a west workspace (ZEPHYR_BASE set or discoverable).

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
BOARD="native_sim/native/64"
BUILD="$REPO/build-tests"
LOG="$BUILD.log"
SUITE_TIMEOUT="${SUITE_TIMEOUT:-600}" # seconds per suite, far above the slowest one

fail=0
for dir in "$HERE"/*/; do
	[ -f "${dir}testcase.yaml" ] || continue
	name="$(basename "$dir")"
	printf '\n\033[1;36m=== %s ===\033[0m\n' "$name"
	# The build log goes to a file and is shown on failure (#485 B9: the
	# compiler error used to vanish into /dev/null).
	if ! west build -p always -b "$BOARD" -d "$BUILD" "$dir" >"$LOG" 2>&1; then
		printf '\033[1;31mBUILD FAIL\033[0m %s\n' "$name"
		tail -n 60 "$LOG"
		fail=1
		continue
	fi
	# A hung suite must not hold the job until the runner's own limit.
	if timeout "$SUITE_TIMEOUT" "$BUILD/zephyr/zephyr.exe"; then
		printf '\033[1;32mPASS\033[0m %s\n' "$name"
	else
		printf '\033[1;31mFAIL\033[0m %s\n' "$name"
		fail=1
	fi
done

rm -rf "$BUILD" "$LOG"
exit $fail
