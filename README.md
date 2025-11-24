# STICKER Firmware

[![GitHub Actions](https://img.shields.io/github/actions/workflow/status/hardwario/sticker-firmware/build.yml)](https://github.com/hardwario/sticker-firmware/actions)
[![GitHub Release](https://img.shields.io/github/v/release/hardwario/sticker-firmware?sort=semver)](https://github.com/hardwario/sticker-firmware/releases)
[![GitHub License](https://img.shields.io/github/license/hardwario/sticker-firmware)](https://github.com/hardwario/sticker-firmware/blob/main/LICENSE)

---

## Overview

**STICKER** is HARDWARIO's compact LoRaWAN sensor platform: a battery-powered device that can collect environmental and motion data and connect external sensors, while running for many months on **two AA battery cells**.

This repository contains the **open firmware** for STICKER, built on the **Zephyr RTOS** and using the Zephyr toolchain (West, Zephyr SDK, CMake). It is intended for main firmware development and customization.

The firmware implementation covers functionality of the catalog applications:

* Catalog application [**STICKER Clime**](https://www.hardwario.com/sticker/clime/)
* Catalog application [**STICKER Input**](https://www.hardwario.com/sticker/input/)
* Catalog application [**STICKER Motion**](https://www.hardwario.com/sticker/motion/)

The firmware is focused on the **device side** - you bring your own LoRaWAN network server, gateway and cloud/backend.

### Learn more about STICKER

* Product page: **https://www.hardwario.com/sticker**
* Documentation: **https://docs.hardwario.com/sticker/**

---

## Key Features

STICKER platform highlights (hardware + firmware):

* **LoRaWAN connectivity** for long-range, low-power wireless communication
* **Low-power design** - operation from two AA battery cells with long lifetime
* **Built-in sensors**
  * Temperature, humidity, light intensity
  * PIR motion detection
  * 3-axis accelerometer
* **Extensibility**
  * 1-Wire interface for external temperature and other probes
  * Support for voltage measurement (up to 30 V) and digital/industrial inputs
* **NFC configuration**
  * Configure LoRaWAN settings, alarms and other parameters using NFC from a phone
* **Open firmware based on Zephyr**
  * Full control over application logic
  * Reuse of existing Zephyr drivers and subsystems

---

## Getting Started

### 1. Create a workspace

Create a workspace directory:

```bash
mkdir sticker
```

Enter the workspace directory:

```bash
cd sticker
```

### 2. Set up Python environment

Create Python virtual environment:

```bash
python -m venv .venv
```

Activate the virtual environment (Linux/macOS):

```bash
source .venv/bin/activate
```

Upgrade pip:

```bash
pip install --upgrade pip
```

### 3. Install Zephyr tooling

Install West (Zephyr meta-tool):

```bash
pip install west
```

Initialize Zephyr workspace and fetch modules:

```bash
west init -m https://github.com/hardwario/sticker-firmware.git
```

Fetch all modules:

```bash
west update
```

Export Zephyr CMake package:

```bash
west zephyr-export
```

Install required Python packages for Zephyr:

```bash
west packages pip --install
```

### 4. Additional tools

Install RTT terminal helper:

```bash
pip install rttt
```

Install Protocol Buffers tooling:

```bash
pip install protobuf grpcio-tools
```

### 5. Install Zephyr SDK

Install the appropriate Zephyr SDK version:

```bash
west sdk install
```

> Refer to the Zephyr documentation if you need to select a specific SDK version.

---

## Building & Flashing

All firmware-specific commands are run from the `app` directory:

```bash
cd app
```

### Build (production)

```bash
make
```

### Build (debug)

```bash
make debug
```

### Flash firmware

```bash
make flash
```

### Clean build outputs

```bash
make clean
```

### Open RTT terminal

```bash
make rttt
```

### Format source code

```bash
make format
```

> Available `make` targets may evolve over time; inspect the `Makefile` for details.

---

## Continuous Integration

This repository uses **GitHub Actions** for CI:

* Every push and pull request is built to ensure the firmware compiles successfully.
* You can browse runs and logs here: **https://github.com/hardwario/sticker-firmware/actions**

If you make any changes, please ensure they build cleanly in CI.

---

## Contributing

Contributions are welcome — whether you:

* Fix a bug
* Improve configuration or board support
* Extend support for additional sensors
* Enhance documentation or comments

### Basic workflow

1. **Fork** this repository

2. Create a feature branch:

   ```bash
   git checkout -b feature/my-improvement
   ```

3. Commit your changes with a clear message:

   ```bash
   git commit -am "Describe your change briefly"
   ```

4. **Open a Pull Request** against `main` with:

   * A short description of the change
   * Any relevant notes for reviewers

Try to keep changes focused and small; it makes review easier and more pleasant for everyone.

---

## Support & Resources

* STICKER product page: **https://www.hardwario.com/sticker**
* STICKER documentation: **https://docs.hardwario.com/sticker/**
* HARDWARIO forum: **https://forum.hardwario.com**
* HARDWARIO store: **https://store.hardwario.com**

If you are using STICKER in a commercial or larger-scale deployment and need guidance, you can also reach out via the contact form on the website.

---

## License

This project is licensed under the **Apache License 2.0** - see the [**LICENSE**](LICENSE) file for details.

---

Made with ❤ by [**HARDWARIO a.s.**](https://www.hardwario.com/) in the heart of Europe.
