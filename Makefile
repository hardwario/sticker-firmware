all: release

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
