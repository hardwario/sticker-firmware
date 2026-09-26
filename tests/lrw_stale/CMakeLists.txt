# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_lrw_stale)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# app_lrw_stale.c is the pure M-2 stale-uplink decision (F29 duty-cycle hold).
target_include_directories(app PRIVATE ${APP_DIR}/src)
target_sources(app PRIVATE
  ${APP_DIR}/src/app_lrw_stale.c
  src/main.c
)
