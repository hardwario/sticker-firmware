# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_history_flash)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# Exercises the NVS flash backend of app_history (CONFIG_APP_HISTORY_FLASH=y)
# on the native_sim flash simulator, so the meta-write coalescing and its
# reboot-survival can be validated (the RAM backend in tests/history can't).
target_include_directories(app PRIVATE ${APP_DIR}/src)
target_sources(app PRIVATE
  ${APP_DIR}/src/app_history.c
  src/main.c
  src/stubs.c
)
