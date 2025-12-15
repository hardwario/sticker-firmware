.PHONY: all deploy release debug flash flash_debug flash_release gdb rttt init clean config config_debug

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
	@west build -b sticker -- -DEXTRA_CONF_FILE=debug.conf

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
