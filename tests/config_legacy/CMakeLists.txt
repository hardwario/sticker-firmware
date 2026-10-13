# Copyright (c) 2026 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_app_config_legacy)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app)

# app_config_legacy.c (#498) converts the NVS values of the removed bool GP caps
# into cap_gpio_mode / cap_gpio_input_a/_b. It needs only the config struct
# from app_config.h; settings_name_steq() / settings_delete() are stubbed in
# main.c (no settings backend).
target_include_directories(app PRIVATE ${APP_DIR}/src)
target_sources(app PRIVATE
  ${APP_DIR}/src/app_config_legacy.c
  src/main.c
)
