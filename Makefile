.DEFAULT_GOAL := require-target

.PHONY: require-target
require-target:
	@echo "Error: You must specify a target!" >&2
	@echo "Usage: make <target>" >&2
	@exit 1

.PHONY: format
format:
	@find . -iname '*.h' -o -iname '*.c' | xargs clang-format -i --style=file:.clang-format
