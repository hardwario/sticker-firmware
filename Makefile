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

init:
	@west build -p always -t initlevels -b sticker

clean:
	@rm -rf build

.PHONY: Version input where is acitaved in prj.conf CONFIG_W1=y
v_input:
	@sed -i 's/^CONFIG_W1=n/CONFIG_W1=y/' prj.conf
.PHONY: Version Clime where is acitaved in prj.conf CONFIG_W1=n
v_clime:
	@sed -i 's/^CONFIG_W1=y/CONFIG_W1=n/' prj.conf

