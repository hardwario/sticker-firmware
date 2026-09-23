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

# The shared join KAT fixture (tests/ccm/p2p_join_kat.json, #417 / GitLab #73)
# has fw_version 1.4.0.0 among its inputs, and the JoinRequest tag covers it.
# This suite compiles app_p2p.c directly rather than through app/CMakeLists.txt,
# so it does not inherit the firmware's version definitions and app_version.h
# would fall back to 0.0.0. Pin the fixture's values here: they are inputs to a
# fixed vector, not a claim about what the firmware version is.
target_compile_definitions(app PRIVATE
  APP_VERSION_MAJOR=1
  APP_VERSION_MINOR=4
  APP_VERSION_PATCH=0
)
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
