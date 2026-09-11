# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Test-local DT binding for the fake LoRa modem (dts/bindings/lora/...), and
# the app's Kconfig root so CONFIG_APP_CCM_HW_AES resolves (off here -> soft-SE).
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# NOTE: not APP_DIR -- find_package(Zephyr) overwrites APP_DIR internally.
set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)
set(KCONFIG_ROOT ${APP_SRC_DIR}/Kconfig)
set(SOFT_SE_DIR $ENV{ZEPHYR_BASE}/../modules/lib/loramac-node/src/peripherals/soft-se)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_p2p_logic)

target_include_directories(app PRIVATE ${APP_SRC_DIR}/src ${SOFT_SE_DIR})
target_sources(app PRIVATE
  ${APP_SRC_DIR}/src/app_p2p.c
  ${APP_SRC_DIR}/src/app_ccm.c
  ${SOFT_SE_DIR}/aes.c
  src/emul_lora.c
  src/stubs.c
  src/main.c
)
set_source_files_properties(${APP_SRC_DIR}/src/app_ccm.c ${SOFT_SE_DIR}/aes.c
  PROPERTIES COMPILE_DEFINITIONS "AES_ENC_PREKEYED")
