# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_cmd)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

list(APPEND CMAKE_MODULE_PATH $ENV{ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_GENERATE_CPP_STANDALONE OFF)
nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${APP_DIR}
  ${APP_DIR}/src/app_config.proto)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${APP_DIR}/src)
target_sources(app PRIVATE
  ${proto_srcs}
  ${APP_DIR}/src/app_cmd.c
  ${APP_DIR}/src/app_config_ingest.c   # real APPLY_*/FILL_* range validation
  ${APP_DIR}/src/app_config_table.c    # generic table-driven apply/fill interpreter
  src/main.c
  src/stubs.c
)
