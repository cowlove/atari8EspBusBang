LLVM_MOS_BIN ?= /opt/llvm-mos/bin
ATARIMAX_CC ?= $(LLVM_MOS_BIN)/mos-atari8-cart-atarimax128-clang
IDF_PATH ?= /opt/esp-idf
SPIFFS_GEN := $(IDF_PATH)/components/spiffs/spiffsgen.py
BUILD_DIR := .build
ROM_BUILD := $(BUILD_DIR)/stress.rom
IMAGE_SIZE := 0x420000
IMAGE_FILES := $(wildcard *.car *.rom *.atr)

.PHONY: all spiffs.bin clean

all: spiffs.bin

$(ROM_BUILD): stress.c | $(BUILD_DIR)
	$(ATARIMAX_CC) $< -Oz -o $@

stress.car: $(ROM_BUILD) ../tools/make_cart.py
	python3 ../tools/make_cart.py $(ROM_BUILD) $@

$(BUILD_DIR):
	mkdir -p $@

spiffs.bin: stress.car $(IMAGE_FILES)
	@set -eu; \
	stage="$$(mktemp -d ./.spiffs-stage.XXXXXX)"; \
	trap 'rm -rf "$$stage"' EXIT; \
	for file in *.car *.rom *.atr; do \
		test -f "$$file" || continue; \
		cp -- "$$file" "$$stage/"; \
	done; \
	python3 "$(SPIFFS_GEN)" $(IMAGE_SIZE) "$$stage" "$@" \
		--block-size 4096 --page-size 256 --obj-name-len 32 --meta-len 4 \
		--no-magic --no-magic-len

clean:
	rm -rf .build stress.car spiffs.bin
