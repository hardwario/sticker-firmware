# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# NOTE: do not call this variable APP_DIR — find_package(Zephyr) below
# overwrites APP_DIR with APPLICATION_SOURCE_DIR internally (cmake/modules/
# kconfig.cmake's zephyr_get(APP_DIR ...)), silently clobbering it.
set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# Scoped custom devicetree binding for the ST25DV i2c_emul model (dts/bindings/
# under this test dir only — not the repo-wide dts/bindings/, since this
# compatible only exists for this test's emulated hardware).
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# app_nfc.c BUILD_ASSERTs on CONFIG_APP_NFC_ENCRYPTION (or CONFIG_FW_DEBUG) —
# both live in app/Kconfig, which no other test needs and which this
# directory's own (nonexistent) Kconfig wouldn't provide by default. Reuse the
# real app's Kconfig root outright instead of duplicating its symbols.
set(KCONFIG_ROOT ${APP_SRC_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_nfc_hw)
set(SOFT_SE_DIR $ENV{ZEPHYR_BASE}/../modules/lib/loramac-node/src/peripherals/soft-se)

list(APPEND CMAKE_MODULE_PATH $ENV{ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_GENERATE_CPP_STANDALONE OFF)
nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${APP_SRC_DIR}
  ${APP_SRC_DIR}/src/app_config.proto)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${APP_SRC_DIR}/src ${SOFT_SE_DIR})
target_sources(app PRIVATE
  ${proto_srcs}
  ${APP_SRC_DIR}/src/app_nfc.c
  ${APP_SRC_DIR}/src/app_nfc_parser.c   # real NDEF parsing (proven standalone in tests/ndef)
  ${APP_SRC_DIR}/src/app_ccm.c          # real AES-CCM (proven standalone in tests/ccm)
  ${SOFT_SE_DIR}/aes.c
  ${APP_SRC_DIR}/src/app_cmd.c          # real command dispatch (proven standalone in tests/cmd)
  ${APP_SRC_DIR}/src/app_config_ingest.c
  src/emul_st25dv.c
  src/main.c
  src/stubs.c
)
set_source_files_properties(${APP_SRC_DIR}/src/app_ccm.c ${SOFT_SE_DIR}/aes.c
  PROPERTIES COMPILE_DEFINITIONS "AES_ENC_PREKEYED")
