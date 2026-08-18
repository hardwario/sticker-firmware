# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_alarm_eval)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# Real app_alarm.c + app_alarm_rules.c (the dwell/confirm/hold state machine
# under test, #348) with the rest of the app stubbed out (radio, sensors,
# hall/input GPIO, report/history) — see src/stubs.c.
target_include_directories(app PRIVATE ${APP_DIR}/src)
target_sources(app PRIVATE
  ${APP_DIR}/src/app_alarm.c
  ${APP_DIR}/src/app_alarm_rules.c
  src/main.c
  src/stubs.c
)
