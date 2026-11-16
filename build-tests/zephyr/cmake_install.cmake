# Install script for directory: /home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/zephyr

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/arch/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/lib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/soc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/boards/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/subsys/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/drivers/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/sticker/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/acpica/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/cmsis/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/cmsis-dsp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/cmsis-nn/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/cmsis_6/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/dhara/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/fatfs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/adi/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_afbr/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_ambiq/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/atmel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_bouffalolab/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_espressif/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_ethos_u/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_gigadevice/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_infineon/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_intel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_microchip/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_nordic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/nuvoton/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_nxp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/openisa/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/quicklogic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_realtek/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_renesas/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_rpi_pico/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_sifli/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_silabs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_st/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_stm32/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_tdk/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_telink/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/ti/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_wch/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hal_wurthelektronik/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/xtensa/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/hostap/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/liblc3/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/libmctp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/libmetal/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/libsbc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/littlefs/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/lora-basics-modem/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/loramac-node/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/lvgl/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/mbedtls/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/mcuboot/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/mipi-sys-t/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/nanopb/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/nrf_wifi/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/open-amp/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/openthread/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/percepio/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/picolibc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/segger/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/trusted-firmware-a/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/trusted-firmware-m/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/uoscore-uedhoc/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/zcbor/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/xenlib/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/modules/nrf_hw_models/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/kernel/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/cmake/flash/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/cmake/usage/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/cmake/reports/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/hymbajs/Projects/20_Github/sticker-firmware/sticker-z45/sticker/build-tests/zephyr/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
