# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_ccm)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)
# LoRaMac soft-SE AES — the software block-cipher backend app_ccm uses when
# CONFIG_APP_CCM_HW_AES is off (undefined here), i.e. the exact production
# fallback path. The HW peripheral path is target-only and validated on hardware.
set(SOFT_SE_DIR $ENV{ZEPHYR_BASE}/../modules/lib/loramac-node/src/peripherals/soft-se)

target_include_directories(app PRIVATE ${APP_DIR}/src ${SOFT_SE_DIR})
target_sources(app PRIVATE
  ${APP_DIR}/src/app_ccm.c
  ${SOFT_SE_DIR}/aes.c
  src/main.c
)
set_source_files_properties(${APP_DIR}/src/app_ccm.c ${SOFT_SE_DIR}/aes.c
  PROPERTIES COMPILE_DEFINITIONS "AES_ENC_PREKEYED")
