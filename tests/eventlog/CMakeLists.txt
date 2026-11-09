# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_eventlog)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# Event log flash ring (app_eventlog.c) on the native_sim flash simulator:
# seq numbering, reboot recovery, queue overflow gaps, ring eviction, replay
# selection and the raw fPort-4 framing. Radio, clock and the AlarmReport
# encoder are stubbed (src/stubs.c).
target_include_directories(app PRIVATE ${APP_DIR}/src)
target_sources(app PRIVATE
  ${APP_DIR}/src/app_eventlog.c
  src/main.c
  src/stubs.c
)
