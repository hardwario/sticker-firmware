all:
	@west build -b sticker

debug:
	@west build -b sticker -- -DEXTRA_CONF_FILE=debug.conf

flash:
	@west flash

clean:
	@rm -rf build
