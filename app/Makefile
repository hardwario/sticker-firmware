.PHONY: all deploy release debug flash flash_debug flash_release gdb rttt init clean config config_debug

# Optional bench variables. Both default to today's behaviour, so an unset
# invocation is byte-for-byte the command it always was.
#
#   JLINK_SN   J-Link serial to use, for rigs with more than one probe attached
#              (the P2P bench has two: the DUT's and the northbridge's). Passed
#              to the Zephyr runner as --dev-id, which reaches JLinkExe as
#              -USB <SN>.  e.g.  make flash JLINK_SN=802002696
#   RTT_ADDR   Address of the _SEGGER_RTT control block. The default below is
#              right for the release/debug images, but it MOVES between builds
#              -- read it from the image with
#              `nm build/zephyr/zephyr.elf | grep ' _SEGGER_RTT$$'` when rttt
#              cannot find the block.
JLINK_SN ?=
RTT_ADDR ?= 0x20000800

ifneq ($(JLINK_SN),)
JLINK_DEV_ID := --dev-id $(JLINK_SN)
RTTT_SERIAL := --serial $(JLINK_SN)
endif

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
	@west flash $(JLINK_DEV_ID)

flash_debug:
	@west flash $(JLINK_DEV_ID) --hex-file deploy/sticker-debug.hex

flash_release:
	@west flash $(JLINK_DEV_ID) --hex-file deploy/sticker-release.hex

gdb:
	@west debug

rttt:
	@rttt $(RTTT_SERIAL) --device STM32WLE5CC --address $(RTT_ADDR)

init:
	@west build -p always -t initlevels -b sticker

clean:
	@rm -rf build

config:
	@west build -t menuconfig -b sticker -p always

config_debug:
	@west build -t menuconfig -b sticker -- -DEXTRA_CONF_FILE=debug.conf
