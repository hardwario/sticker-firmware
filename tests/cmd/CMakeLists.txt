# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# NOTE: do not call this variable APP_DIR — find_package(Zephyr) below
# overwrites APP_DIR with APPLICATION_SOURCE_DIR internally (cmake/modules/
# kconfig.cmake's zephyr_get(APP_DIR ...)), silently clobbering it.
set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# app_cmd.c's buzzer-play dispatch is CONFIG_APP_BUZZER-gated (issue #395),
# which lives in app/Kconfig — reuse it outright instead of duplicating the
# symbol here (same idiom as tests/nfc_hw).
set(KCONFIG_ROOT ${APP_SRC_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_cmd)

set(APP_DIR ${APP_SRC_DIR})

list(APPEND CMAKE_MODULE_PATH $ENV{ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_GENERATE_CPP_STANDALONE OFF)
nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${APP_DIR}
  ${APP_DIR}/src/app_config.proto)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${APP_DIR}/src)
# B8: app_cmd_handle_req_history routes req_history on the transport, and its
# `#if defined(CONFIG_RADIO_P2P)` arm was not compiled natively at all before
# this -- so the P2P dispatch path had no test. The real Kconfig symbol
# `depends on LORA`, which this suite has no reason to pull in (it stubs
# app_p2p_start_history_replay anyway), so the gate is set for this target
# directly. It selects which branch of app_cmd.c compiles; nothing else here
# consults it.
target_compile_definitions(app PRIVATE CONFIG_RADIO_P2P=1)

target_sources(app PRIVATE
  ${proto_srcs}
  ${APP_DIR}/src/app_cmd.c
  ${APP_DIR}/src/app_config_ingest.c   # real APPLY_*/FILL_* range validation
  src/main.c
  src/stubs.c
)
