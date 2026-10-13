# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# NOTE: do not call this variable APP_DIR before find_package(Zephyr) — it
# overwrites APP_DIR internally (see tests/compose/CMakeLists.txt).
set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_w1_slots)

set(APP_DIR ${APP_SRC_DIR})

# app_w1_slots.c encodes SensorReading, so it needs the generated nanopb header.
list(APPEND CMAKE_MODULE_PATH $ENV{ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
set(NANOPB_GENERATE_CPP_STANDALONE OFF)
nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${APP_DIR}
  ${APP_DIR}/src/app_config.proto)

# Real app_w1_slots.c (rebind / teach / expected type / mismatch, #430) over
# fake DS18B20 + machine-probe drivers — see src/stubs.c.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${APP_DIR}/src)
target_sources(app PRIVATE
  ${proto_srcs}
  ${APP_DIR}/src/app_w1_slots.c
  ${APP_DIR}/src/app_sensor_types.c
  src/main.c
  src/stubs.c
)
