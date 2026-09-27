# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The common uplink path of app_radio.c (doc/plan/460 F4) against a fake
# backend: CONFIG_LORAWAN and CONFIG_RADIO_P2P stay unset, so neither real
# backend is compiled in. NOTE: not APP_DIR -- find_package(Zephyr) overwrites it.
set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_radio_common)

target_include_directories(app PRIVATE ${APP_SRC_DIR}/src)
target_sources(app PRIVATE
  ${APP_SRC_DIR}/src/app_radio.c
  src/main.c
  src/stubs.c
)
