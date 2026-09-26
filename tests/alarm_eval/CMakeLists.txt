# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# NOTE: do not call this variable APP_DIR — find_package(Zephyr) below
# overwrites APP_DIR with APPLICATION_SOURCE_DIR internally (cmake/modules/
# kconfig.cmake's zephyr_get(APP_DIR ...)), silently clobbering it.
set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# app_alarm.c's buzzer indication (alarm_buzzer_sync, #397) is
# CONFIG_APP_BUZZER-gated (issue #395), which lives in app/Kconfig — reuse it
# outright instead of duplicating the symbol here (same idiom as tests/cmd).
set(KCONFIG_ROOT ${APP_SRC_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_alarm_eval)

set(APP_DIR ${APP_SRC_DIR})

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
