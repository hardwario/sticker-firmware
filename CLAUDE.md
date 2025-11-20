# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a Zephyr RTOS-based firmware project for the HARDWARIO Sticker device - an IoT sensor device with LoRaWAN connectivity, STM32WLE5CC MCU, NFC configuration, and multiple sensor support.

## Build Commands

All commands should be run from `sticker/sticker/app/`:

```bash
# Build release
make

# Build debug (with debug.conf)
make debug

# Flash to device
make flash

# Start RTT terminal (for debug output)
make rttt

# Start GDB debugger
make gdb

# Clean build artifacts
make clean

# Menuconfig
make config
make config_debug

# Build both release and debug hexes to deploy/
make deploy

# Format source code (run from sticker/sticker/)
make format
```

## Project Structure

```
sticker/sticker/           # Main application repository
├── app/                   # Application code
│   ├── src/               # Source files (main.c, app_*.c modules)
│   ├── prj.conf           # Base Kconfig configuration
│   └── debug.conf         # Debug overlay configuration
├── boards/sticker/        # Custom board definition
├── drivers/               # Custom Zephyr drivers (mpl3115a2, w1_ds28e17)
├── dts/bindings/          # Device tree bindings
├── include/               # Header files
└── west.yml               # West manifest (uses hardwario/sticker-zephyr)
```

## Architecture

- **Zephyr-based**: Uses West build system and Zephyr RTOS
- **Modular app_* design**: Each subsystem (sensors, NFC, LoRaWAN, etc.) is in its own `app_*.c` file
- **Nanopb protobuf**: NFC configuration uses protobuf (`src/nfc_config.proto`)
- **Conditional compilation**: Sensor drivers enabled via Kconfig (CONFIG_SHT4X, CONFIG_LIS2DH, CONFIG_OPT3001, etc.)
- **Custom board**: Target board is `sticker` defined in `boards/sticker/`

## Key Technologies

- **MCU**: STM32WLE5CC (Cortex-M4 with LoRa radio)
- **Connectivity**: LoRaWAN
- **Sensors**: SHT40 (temp/humidity), LIS2DH (accelerometer), OPT3001 (light), MPL3115A2 (pressure), DS18B20 (1-Wire temp), PYQ1648 (PIR)
- **NFC**: Configuration via NDEF records
- **RTT**: Segger RTT for debug output (address: 0x20000800)

## Development Setup

Requires Python virtual environment with West, RTTT, and Protocol Buffers tools installed. See README.md for full setup instructions.
