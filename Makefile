.PHONY: all deploy release debug flash gdb init clean

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
