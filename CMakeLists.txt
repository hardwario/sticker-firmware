#
# Copyright (c) 2024 HARDWARIO a.s.
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sticker)

target_sources_ifdef(CONFIG_ADC app PRIVATE src/app_battery.c)
target_sources_ifdef(CONFIG_APP_PROFILE_STICKER_MOTION app PRIVATE src/app_pyq1648.c)
target_sources_ifdef(CONFIG_LIS2DH app PRIVATE src/app_accel.c)
target_sources_ifdef(CONFIG_OPT3001 app PRIVATE src/app_opt3001.c)
target_sources_ifdef(CONFIG_SHELL app PRIVATE src/app_tester.c)
target_sources_ifdef(CONFIG_SHT4X app PRIVATE src/app_sht40.c)
target_sources_ifdef(CONFIG_W1 app PRIVATE src/app_ds18b20.c)
target_sources_ifdef(CONFIG_W1 app PRIVATE src/app_w1.c)
target_sources_ifdef(CONFIG_WATCHDOG app PRIVATE src/app_wdog.c)
target_sources(app PRIVATE src/app_alarm.c)
target_sources(app PRIVATE src/app_compose.c)
target_sources(app PRIVATE src/app_config.c)
target_sources(app PRIVATE src/app_led.c)
target_sources(app PRIVATE src/app_lrw.c)
target_sources(app PRIVATE src/app_sensor.c)
target_sources(app PRIVATE src/main.c)
