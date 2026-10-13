#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

# Fail when a STICKER module patch (zephyr/patches.yml, `west patch apply`) is
# missing from a module source: FILE must contain MARKER ("STICKER-<issue>").
# Used at configure time via include() and at build time via `cmake -P`, so an
# incremental build after `west update` (which resets the module) cannot link
# an unpatched image either.
file(STRINGS "${FILE}" _sticker_patch_hit REGEX "${MARKER}")
if(NOT _sticker_patch_hit)
  message(FATAL_ERROR
    "${FILE} is missing the STICKER patch ${MARKER}.\n"
    "Run `west patch apply` in the workspace (from a git worktree: west patch apply "
    "-b <worktree>/zephyr/patches -l <worktree>/zephyr/patches.yml), or pass "
    "-DSTICKER_ALLOW_UNPATCHED_MODULES=ON for a throwaway build.")
endif()
