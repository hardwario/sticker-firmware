# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_sensor_types)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# The generated registry is pure tables + lookups; it only needs the config
# struct layout (offsetof of the cap_* gates) from app_config.h.
target_include_directories(app PRIVATE ${APP_DIR}/src)
target_sources(app PRIVATE
  ${APP_DIR}/src/app_sensor_types.c
  src/main.c
)
