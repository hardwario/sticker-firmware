#
# Copyright (c) 2025 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sticker)

# Firmware version string baked into the image (logged at boot, reported by the
# GetInfo command). CI passes the release tag or short commit hash via
# -DAPP_VERSION_OVERRIDE (see .github/workflows/build.yml). Local builds fall
# back to `git describe`, then to "dev" if git is unavailable.
if(NOT DEFINED APP_VERSION_OVERRIDE)
  execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} describe --always --dirty --tags
    OUTPUT_VARIABLE APP_VERSION_OVERRIDE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT APP_VERSION_OVERRIDE)
  set(APP_VERSION_OVERRIDE "dev")
endif()
add_compile_definitions(APP_VERSION_STRING="${APP_VERSION_OVERRIDE}")
message(STATUS "Firmware version: ${APP_VERSION_OVERRIDE}")

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

zephyr_nanopb_sources(app src/nfc_config.proto)

target_sources_ifdef(CONFIG_ADC app PRIVATE src/app_battery.c)
target_sources_ifdef(CONFIG_DS28E17 app PRIVATE src/app_machine_probe.c)
target_sources_ifdef(CONFIG_LIS2DH app PRIVATE src/app_accel.c)
target_sources_ifdef(CONFIG_LORAWAN app PRIVATE src/app_lrw.c)
target_include_directories_ifdef(CONFIG_LORAWAN app PRIVATE
  ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/mac
  ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/mac/region
  ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/system
  ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/boards
)
target_sources_ifdef(CONFIG_OPT3001 app PRIVATE src/app_opt3001.c)
target_sources_ifdef(CONFIG_SHELL app PRIVATE src/app_tester.c)
target_sources_ifdef(CONFIG_SHT4X app PRIVATE src/app_sht4x.c)
target_sources_ifdef(CONFIG_W1 app PRIVATE src/app_ds18b20.c)
target_sources_ifdef(CONFIG_W1 app PRIVATE src/app_w1.c)
target_sources_ifdef(CONFIG_WATCHDOG app PRIVATE src/app_wdog.c)
target_sources(app PRIVATE src/app_alarm.c)
target_sources(app PRIVATE src/app_calibration.c)
target_sources(app PRIVATE src/app_compose.c)
target_sources(app PRIVATE src/app_config.c)
target_sources(app PRIVATE src/app_hall.c)
target_sources(app PRIVATE src/app_input.c)
target_sources(app PRIVATE src/app_led.c)
target_sources(app PRIVATE src/app_mpl3115a2.c)
target_sources(app PRIVATE src/app_ndef_parser.c)
target_sources(app PRIVATE src/app_nfc_ingest.c)
target_sources(app PRIVATE src/app_nfc.c)
target_sources(app PRIVATE src/app_pyq1648.c)
target_sources(app PRIVATE src/app_sensor.c)
target_sources(app PRIVATE src/app_settings.c)
target_sources(app PRIVATE src/main.c)
