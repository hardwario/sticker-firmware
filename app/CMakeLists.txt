#
# Copyright (c) 2025 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sticker)

# Firmware version + source baked into the image (logged at boot, reported by
# the GetInfo command). CI passes the numeric version components and build type
# via -DAPP_VERSION_* / -DAPP_BUILD_TYPE (see .github/workflows/build.yml):
#   tag v1.3.4 -> 1/3/4, type 0 (MAIN)
#   branch/PR  -> 0/0/0, type 1 (DEV)
# Local builds (no override) use the hardcoded development version below and are
# always type 2 (CUSTOM). The debug/release config is a separate axis
# (CONFIG_FW_DEBUG), not set here.
if(NOT DEFINED APP_VERSION_MAJOR)
  set(APP_BUILD_TYPE 2) # CUSTOM
  set(APP_VERSION_MAJOR 1)
  set(APP_VERSION_MINOR 4)
  set(APP_VERSION_PATCH 0)
endif()
add_compile_definitions(
  APP_VERSION_MAJOR=${APP_VERSION_MAJOR}
  APP_VERSION_MINOR=${APP_VERSION_MINOR}
  APP_VERSION_PATCH=${APP_VERSION_PATCH}
  APP_BUILD_TYPE=${APP_BUILD_TYPE})
message(STATUS "Firmware version: ${APP_VERSION_MAJOR}.${APP_VERSION_MINOR}.${APP_VERSION_PATCH} (build type ${APP_BUILD_TYPE})")

# Shared headers (sticker/drivers/...) live in the repo's top-level include/.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

# Note: nanopb generates src/app_config.pb.{c,h} here; west configen generates
# src/app_config.{c,h} (struct/Settings/shell) from app_config.yml. Different
# extensions, same stem — no clash.
zephyr_nanopb_sources(app src/app_config.proto)

target_sources_ifdef(CONFIG_ADC app PRIVATE src/app_battery.c)
target_sources_ifdef(CONFIG_APP_LORA_P2P app PRIVATE src/app_p2p.c)
target_sources_ifdef(CONFIG_DS28E17 app PRIVATE src/app_machine_probe.c)
target_sources_ifdef(CONFIG_LIS2DH app PRIVATE src/app_accel.c)
target_sources_ifdef(CONFIG_LORAWAN app PRIVATE src/app_lrw.c)
target_sources_ifdef(CONFIG_LORAWAN app PRIVATE src/app_report.c)
target_include_directories_ifdef(CONFIG_LORAWAN app PRIVATE
  ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/mac
  ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/mac/region
  ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/system
  ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/boards
)
target_sources_ifdef(CONFIG_OPT3001 app PRIVATE src/app_opt3001.c)
target_sources_ifdef(CONFIG_POWEROFF app PRIVATE src/app_power.c)
target_sources_ifdef(CONFIG_SHELL app PRIVATE src/app_ats.c)
target_sources_ifdef(CONFIG_SHT4X app PRIVATE src/app_sht4x.c)
target_sources_ifdef(CONFIG_W1 app PRIVATE src/app_ds18b20.c)
target_sources_ifdef(CONFIG_W1 app PRIVATE src/app_w1.c)
target_sources_ifdef(CONFIG_W1 app PRIVATE src/app_w1_slots.c)
target_sources_ifdef(CONFIG_WATCHDOG app PRIVATE src/app_wdog.c)
target_sources(app PRIVATE src/app_alarm.c)
target_sources(app PRIVATE src/app_alarm_rules.c)
target_sources(app PRIVATE src/app_buzzer.c)
target_sources(app PRIVATE src/app_calibration.c)
target_sources(app PRIVATE src/app_clock.c)
target_sources(app PRIVATE src/app_cmd.c)
target_sources(app PRIVATE src/app_compose.c)
target_sources(app PRIVATE src/app_config.c)
target_sources(app PRIVATE src/app_counters.c)
target_sources(app PRIVATE src/app_sensor_read.c)
target_sources(app PRIVATE src/app_hall.c)
target_sources(app PRIVATE src/app_history.c)
target_sources(app PRIVATE src/app_input.c)
target_sources(app PRIVATE src/app_led.c)
target_sources(app PRIVATE src/app_mpl3115a2.c)
target_sources(app PRIVATE src/app_nfc_parser.c)
target_sources(app PRIVATE src/app_config_ingest.c)
target_sources(app PRIVATE src/app_nfc.c)
target_sources(app PRIVATE src/app_ccm.c)

# app_ccm's block cipher: the STM32WL HW AES peripheral by default. When it is
# disabled (CONFIG_APP_CCM_HW_AES=n, e.g. the variant-A soft fallback) the CCM
# layer uses the LoRaMac soft-SE AES instead — pull that source + its prekeyed
# API in (the HW build needs neither).
if(NOT CONFIG_APP_CCM_HW_AES)
  set(SOFT_SE_DIR ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/peripherals/soft-se)
  target_sources(app PRIVATE ${SOFT_SE_DIR}/aes.c)
  target_include_directories(app PRIVATE ${SOFT_SE_DIR})
  set_source_files_properties(src/app_ccm.c ${SOFT_SE_DIR}/aes.c
    PROPERTIES COMPILE_DEFINITIONS "AES_ENC_PREKEYED")
endif()
target_sources(app PRIVATE src/app_pyq1648.c)
target_sources(app PRIVATE src/app_sensor.c)
target_sources(app PRIVATE src/app_settings.c)
target_sources(app PRIVATE src/app_radio.c)
target_sources(app PRIVATE src/main.c)
