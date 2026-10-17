LLVM_MOS_BIN ?= /opt/llvm-mos/bin
ATARIMAX_CC ?= $(LLVM_MOS_BIN)/mos-atari8-cart-atarimax128-clang
IDF_PATH ?= /opt/esp-idf
SPIFFS_GEN := $(IDF_PATH)/components/spiffs/spiffsgen.py
BUILD_DIR := .build
ROM_BUILD := $(BUILD_DIR)/stress.rom
IMAGE_SIZE := 0x420000
IMAGE_FILE_GLOBS := *.[cC][aA][rR] *.[rR][oO][mM] *.[aA][tT][rR]
IMAGE_FILES := $(wildcard $(IMAGE_FILE_GLOBS))
CONFIG_FILE := $(wildcard config.txt)

.PHONY: all spiffs.bin clean

all: spiffs.bin

$(ROM_BUILD): stress.c | $(BUILD_DIR)
	$(ATARIMAX_CC) $< -Oz -o $@

stress.car: $(ROM_BUILD) ../tools/make_cart.py
	python3 ../tools/make_cart.py $(ROM_BUILD) $@

$(BUILD_DIR):
	mkdir -p $@

spiffs.bin: Makefile stress.car $(IMAGE_FILES) $(CONFIG_FILE)
	@set -eu; \
	stage="$$(mktemp -d ./.spiffs-stage.XXXXXX)"; \
	trap 'rm -rf "$$stage"' EXIT; \
	for file in $(IMAGE_FILE_GLOBS); do \
		test -f "$$file" || continue; \
		cp -- "$$file" "$$stage/"; \
	done; \
	if [ -f config.txt ]; then cp -- config.txt "$$stage/"; fi; \
	python3 "$(SPIFFS_GEN)" $(IMAGE_SIZE) "$$stage" "$@" \
		--block-size 4096 --page-size 256 --obj-name-len 32 --meta-len 4 \
		--no-magic --no-magic-len

clean:
	rm -rf .build stress.car spiffs.bin
