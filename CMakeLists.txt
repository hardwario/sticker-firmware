#
# Copyright (c) 2024 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blinky)

target_sources(app PRIVATE src/app_ds18b20.c)
target_sources(app PRIVATE src/app_sht40.c)
target_sources(app PRIVATE src/app_w1.c)
target_sources(app PRIVATE src/main.c)

zephyr_linker_sources_ifdef(CONFIG_SEGGER_RTT_SECTION_CUSTOM RAM_SECTIONS SORT_KEY 0x0 segger_rtt.ld)
