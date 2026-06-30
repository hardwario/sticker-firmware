.PHONY: all deploy release debug flash flash_debug flash_release gdb rttt init clean config config_debug final_build final_flash final_clean

all: release

deploy:
	@rm -rf deploy
	@mkdir deploy
	@rm -rf build
	@$(MAKE) debug
	@cp build/zephyr/zephyr.hex deploy/sticker-debug.hex
	@rm -rf build
	@$(MAKE) release
	@cp build/zephyr/zephyr.hex deploy/sticker-release.hex

release:
	@west build -p always -b sticker

debug:
	@west build -p always -b sticker -- -DEXTRA_CONF_FILE=debug.conf

flash:
	@west flash

flash_debug:
	@west flash --hex-file deploy/sticker-debug.hex

flash_release:
	@west flash --hex-file deploy/sticker-release.hex

gdb:
	@west debug

rttt:
	@rttt --device STM32WLE5CC --address 0x20000800

init:
	@west build -p always -t initlevels -b sticker

clean:
	@rm -rf build

config:
	@west build -t menuconfig -b sticker -p always

config_debug:
	@west build -t menuconfig -b sticker -- -DEXTRA_CONF_FILE=debug.conf

# --------------------------------------------------------------------------
# Variant-B NFC firmware update: the custom bootloader (../boot) + the app
# linked at code_partition (nfc-boot.conf, release-only). Run from the MAIN
# checkout, not a git worktree — the `sticker` board would otherwise be defined
# twice (worktree + this repo as a Zephyr module) and the build fails with
# "board defined multiple times".
#
# Targets:
#   make final_build    build bootloader + variant-B app -> deploy/
#                       (sticker-bootloader.hex, sticker-app.{hex,bin,sfu})
#   make final_flash    flash bootloader (@0x08000000) + app (@code_partition),
#                       no --erase so NVS (DevEUI/keys/config) is preserved
#   make final_clean    remove the build-boot / build-app-b dirs
#
# Examples:
#   make final_build                              # 1.4.0, build_type CUSTOM(2)
#   make final_build BUILD_TYPE=0                 # 1.4.0 MAIN (release)
#   make final_build BUILD_TYPE=1                 # 1.4.0-dev (DEV branch build)
#   make final_build VERSION_MINOR=5 VERSION_PATCH=0   # 1.5.0
#   make final_build VERSION_PATCH=2 BUILD_TYPE=0      # 1.4.2 MAIN
#   make final_flash                              # flash both images
#   make final_flash JLINK_SN=822005110           # pick a specific J-Link
#   make final_build LOAD_ADDR=0x08009000         # override code_partition base
#
# Build two test images that differ only by version (flash one, then the other):
#   make final_build BUILD_TYPE=0 && cp deploy/sticker-app.sfu /tmp/fw-main.sfu
#   make final_build BUILD_TYPE=1 && cp deploy/sticker-app.sfu /tmp/fw-dev.sfu
#
# Overridable vars: VERSION_MAJOR/MINOR/PATCH, BUILD_TYPE (0=MAIN 1=DEV 2=CUSTOM),
# LOAD_ADDR, JLINK_SN. fw_version baked into the .sfu = (maj<<24|min<<16|patch<<8|type).
# --------------------------------------------------------------------------
VERSION_MAJOR ?= 1
VERSION_MINOR ?= 4
VERSION_PATCH ?= 0
# 0 = MAIN (release/tag), 1 = DEV (branch), 2 = CUSTOM (local build).
BUILD_TYPE    ?= 2
LOAD_ADDR     ?= 0x08009000
JLINK_SN      ?=

VER_ARGS := -DAPP_VERSION_MAJOR=$(VERSION_MAJOR) \
            -DAPP_VERSION_MINOR=$(VERSION_MINOR) \
            -DAPP_VERSION_PATCH=$(VERSION_PATCH) \
            -DAPP_BUILD_TYPE=$(BUILD_TYPE)
FW_VERSION := $(shell printf '0x%02x%02x%02x%02x' $(VERSION_MAJOR) $(VERSION_MINOR) $(VERSION_PATCH) $(BUILD_TYPE))
SNR_ARG    := $(if $(JLINK_SN),--snr $(JLINK_SN),)

final_build:
	@west build -p always -b sticker ../boot -d build-boot
	@west build -p always -b sticker . -d build-app-b -- -DEXTRA_CONF_FILE=nfc-boot.conf $(VER_ARGS)
	@mkdir -p deploy
	@cp build-boot/zephyr/zephyr.hex   deploy/sticker-bootloader.hex
	@cp build-app-b/zephyr/zephyr.hex  deploy/sticker-app.hex
	@cp build-app-b/zephyr/zephyr.bin  deploy/sticker-app.bin
	@python3 ../scripts/mksfu.py build-app-b/zephyr/zephyr.bin deploy/sticker-app.sfu \
	    --load-addr $(LOAD_ADDR) --fw-version $(FW_VERSION)
	@echo "final_build -> deploy/ : sticker-bootloader.hex, sticker-app.{hex,bin,sfu}"

final_flash:
	@west flash -d build-boot $(SNR_ARG)
	@west flash -d build-app-b $(SNR_ARG)

final_clean:
	@rm -rf build-boot build-app-b
