# STICKER NFC bootloader — bare-metal, no Zephyr. Fails the build above 4 KB.
#
#   make                 # build/boot.elf, build/boot.hex, build/boot.bin + size
#   make CROSS=<prefix>  # override the toolchain prefix

ZEPHYR_SDK ?= $(HOME)/zephyr-sdk-0.16.8
CROSS ?= $(ZEPHYR_SDK)/arm-zephyr-eabi/bin/arm-zephyr-eabi-
TOP := $(abspath $(CURDIR)/../..)
HAL := $(TOP)/modules/hal/stm32/stm32cube/stm32wlxx/soc
CMSIS := $(TOP)/modules/hal/cmsis/CMSIS/Core/Include

CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

CFLAGS := -mcpu=cortex-m4 -mthumb -Os -g -std=c11 -Wall -Wextra -Werror \
	-ffreestanding -fno-common -ffunction-sections -fdata-sections \
	-fno-tree-loop-distribute-patterns -DSTM32WLE5xx \
	-I$(HAL) -I$(CMSIS)
LDFLAGS := -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-Map=build/boot.map \
	-Wl,--print-memory-usage -Tboot.ld

SRC := src/main.c src/startup.c

all: build/boot.hex build/boot.bin
	@$(SIZE) build/boot.elf

build/boot.elf: $(SRC) boot.ld Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRC) -lgcc -o $@

build/boot.hex: build/boot.elf
	$(OBJCOPY) -O ihex $< $@

build/boot.bin: build/boot.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf build

.PHONY: all clean
