# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_slot)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# app_slot.c is pure grid math (no kernel calls) — the report cadence (F27).
target_include_directories(app PRIVATE ${APP_DIR}/src)
target_sources(app PRIVATE
  ${APP_DIR}/src/app_slot.c
  src/main.c
)
