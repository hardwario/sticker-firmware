all: release

release:
	@west build -b sticker

debug:
	@west build -b sticker -- -DEXTRA_CONF_FILE=debug.conf

flash:
	@west flash

gdb:
	@west debug

clean:
	@rm -rf build
