# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone "gw-sim" bench firmware for the raw-LoRa P2P transport (#118,
# doc/p2p.md §14) — the two-STICKER rig's "network side" stand-in. A real
# target build (west build -b sticker tests/p2p), NOT a native_sim ztest suite
# like this directory's siblings would be: it needs the real STM32WL radio.
#
# Deliberately minimal: only app_ccm.c (the AES-CCM/ECB primitive) is reused
# from the main app. No app_p2p.c/app_transport.c, no sensors/NFC/LoRaWAN/
# history/protobuf — none of that RAM/flash budget the main app.CMakeLists.txt
# has to juggle applies here, which is the whole point of a separate firmware
# instead of yet another Kconfig combination of the real one.

cmake_minimum_required(VERSION 3.20.0)

set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# Reuse the real app's Kconfig root instead of redeclaring CONFIG_APP_CCM_HW_AES
# (same idiom as tests/nfc_hw/CMakeLists.txt) — this directory has no Kconfig of
# its own otherwise.
set(KCONFIG_ROOT ${APP_SRC_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(p2p_gw_sim)

target_include_directories(app PRIVATE ${APP_SRC_DIR}/src)
target_sources(app PRIVATE
  ${APP_SRC_DIR}/src/app_ccm.c
  src/main.c
)

# app_ccm's block cipher: the STM32WL HW AES peripheral by default (mirrors
# app/CMakeLists.txt exactly — see there for the rationale). CONFIG_APP_CCM_HW_AES
# defaults to y on this target (SOC_SERIES_STM32WLX), so the soft-SE fallback
# below normally does not apply; kept for parity / a debug override.
if(NOT CONFIG_APP_CCM_HW_AES)
  set(SOFT_SE_DIR ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/peripherals/soft-se)
  target_sources(app PRIVATE ${SOFT_SE_DIR}/aes.c)
  target_include_directories(app PRIVATE ${SOFT_SE_DIR})
  set_source_files_properties(${APP_SRC_DIR}/src/app_ccm.c ${SOFT_SE_DIR}/aes.c
    PROPERTIES COMPILE_DEFINITIONS "AES_ENC_PREKEYED")
endif()
