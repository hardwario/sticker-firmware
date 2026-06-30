# Copyright (c) 2025 HARDWARIO a.s.
# SPDX-License-Identifier: Apache-2.0
#
# STICKER NFC bootloader (variant B). WIP — see boot/README.md.
# Build (interim, before sysbuild adoption):
#   west build -b sticker boot -d build/boot

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sticker_nfc_boot)

# Shared protocol contract (sticker/include is on the Zephyr include path
# via the module's top-level CMakeLists; add it explicitly for a standalone
# bootloader build too).
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

target_sources(app PRIVATE
  src/main.c
  src/st25dv_mb.c
  src/flash_writer.c
  src/verify.c
  src/auth.c
)

# Phone-free crypto+erase self-test (diagnostic): build with -DBOOT_SELFTEST=ON.
if(BOOT_SELFTEST)
  zephyr_compile_definitions(CONFIG_BOOT_SELFTEST)
endif()

# DFU retry test hook (diagnostic): build with -DBOOT_DFU_FAULT_INJECT=ON to drop
# the response to one DATA frame once, forcing the phone to resend it so the
# idempotent re-ack path (#236) can be exercised on hardware.
if(BOOT_DFU_FAULT_INJECT)
  zephyr_compile_definitions(CONFIG_BOOT_DFU_FAULT_INJECT)
endif()
