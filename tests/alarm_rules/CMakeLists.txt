# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_alarm_rules)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# app_alarm_rules.c packs/unpacks raw bytes — no protobuf. It only needs the
# config struct (for the alarm_N slot fields) which the stubs provide.
target_include_directories(app PRIVATE ${APP_DIR}/src)
target_sources(app PRIVATE
  ${APP_DIR}/src/app_alarm_rules.c
  src/main.c
  src/stubs.c
)
