#
# Copyright (c) 2026 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

# STICKER simulator (native_sim): emulators of the board's peripherals and the
# `sim` shell that sets what they report. See doc/simulator.md.

zephyr_library()
zephyr_include_directories(.)
zephyr_library_sources(sim_adc.c sim_shell.c)
zephyr_library_sources_ifdef(CONFIG_SHT4X emul_sht4x.c)
zephyr_library_sources_ifdef(CONFIG_OPT3001 emul_opt3001.c)
