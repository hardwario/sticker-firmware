# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# NOTE: do not call this variable APP_DIR — find_package(Zephyr) below
# overwrites APP_DIR with APPLICATION_SOURCE_DIR internally (cmake/modules/
# kconfig.cmake's zephyr_get(APP_DIR ...)), silently clobbering it.
set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_buzzer)

# Real app_buzzer.c (melody engine + playback thread), driven against native_sim's
# built-in gpio_emul (app.overlay) standing in for the pir-si/pir-dl pins the
# buzzer HW variant shares with PIR (#338). No stubs.c needed — app_buzzer.c has
# no dependency outside GPIO/logging.
target_include_directories(app PRIVATE ${APP_SRC_DIR}/src)
target_sources(app PRIVATE
  ${APP_SRC_DIR}/src/app_buzzer.c
  src/main.c
)
