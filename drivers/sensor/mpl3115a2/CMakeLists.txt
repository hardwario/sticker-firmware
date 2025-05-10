#
# Copyright (c) 2025 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

zephyr_library()

zephyr_library_sources(mpl3115a2.c)
zephyr_library_sources_ifdef(CONFIG_MPL3115A2_SHELL mpl3115a2_shell.c)
