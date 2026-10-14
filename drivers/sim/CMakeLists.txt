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

if(CONFIG_LORA_SX126X)
  zephyr_library_sources(emul_sx126x.c)
  # The air's UDP socket lives in the host-side runner (host libc).
  target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/sim_air_bottom.c)
endif()

if(CONFIG_STICKER_SIM_ST25DV)
  zephyr_library_sources(emul_st25dv.c)
  target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/sim_nfc_bottom.c)
endif()
