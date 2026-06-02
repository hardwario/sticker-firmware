# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_history)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# app_history.c uses no protobuf — just the struct headers from app/src.
target_include_directories(app PRIVATE ${APP_DIR}/src)
target_sources(app PRIVATE
  ${APP_DIR}/src/app_history.c
  src/main.c
  src/stubs.c
)
