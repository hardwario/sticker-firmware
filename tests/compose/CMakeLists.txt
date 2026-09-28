# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# NOTE: do not call this variable APP_DIR — find_package(Zephyr) below
# overwrites APP_DIR with APPLICATION_SOURCE_DIR internally (cmake/modules/
# kconfig.cmake's zephyr_get(APP_DIR ...)), silently clobbering it.
set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# app_compose.c's W1-slot encoding is CONFIG_W1/CONFIG_DS28E17-gated (issue
# #395), which live in app/Kconfig — reuse it outright instead of duplicating
# the symbols here (same idiom as tests/nfc_hw).
set(KCONFIG_ROOT ${APP_SRC_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_compose)

set(APP_DIR ${APP_SRC_DIR})

# Generate app_config.pb.{c,h} like the app does, but with RELPATH = app dir so
# the header lands at <bindir>/src/app_config.pb.h (matching the
# `#include "src/app_config.pb.h"` in app sources).
list(APPEND CMAKE_MODULE_PATH $ENV{ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_GENERATE_CPP_STANDALONE OFF)
nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${APP_DIR}
  ${APP_DIR}/src/app_config.proto)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${APP_DIR}/src)
target_sources(app PRIVATE
  ${proto_srcs}
  ${APP_DIR}/src/app_compose.c
  src/main.c
  src/stubs.c
)
