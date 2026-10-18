LLVM_MOS_BIN ?= /opt/llvm-mos/bin
ATARIMAX_CC ?= $(LLVM_MOS_BIN)/mos-atari8-cart-atarimax128-clang
MEGACART_CC ?= $(LLVM_MOS_BIN)/mos-atari8-cart-megacart-clang
IDF_PATH ?= /opt/esp-idf
SPIFFS_GEN := $(IDF_PATH)/components/spiffs/spiffsgen.py
BUILD_DIR := .build
ROM_BUILD := $(BUILD_DIR)/stress.rom
CART_BUILD := $(BUILD_DIR)/stress.car
MEGACART_ROM_BUILD := $(BUILD_DIR)/stress-megacart.rom
MEGACART_CART_BUILD := $(BUILD_DIR)/stress-megacart.car
IMAGE_SIZE := 0x420000
IMAGE_FILE_GLOBS := *.[cC][aA][rR] *.[rR][oO][mM] *.[aA][tT][rR] *.[cC][aA][rR].[gG][zZ] *.[rR][oO][mM].[gG][zZ] *.[aA][tT][rR].[gG][zZ]
IMAGE_FILES := $(filter-out stress.car,$(wildcard $(IMAGE_FILE_GLOBS)))
CONFIG_FILE := $(wildcard config.txt)

.PHONY: all spiffs.bin clean

all: spiffs.bin

$(ROM_BUILD): stress.c read.s | $(BUILD_DIR)
	$(ATARIMAX_CC) stress.c read.s -Oz -Wl,--defsym=__atari_cart_status=0x05 -o $@

$(MEGACART_ROM_BUILD): stress.c read.s | $(BUILD_DIR)
	$(MEGACART_CC) -DSTRESS_MEGACART=1 stress.c read.s -Oz -Wl,--defsym=__cart_rom_size=128 -o $@

$(CART_BUILD): $(ROM_BUILD) ../tools/make_cart.py
	python3 ../tools/make_cart.py $(ROM_BUILD) $@

$(MEGACART_CART_BUILD): $(MEGACART_ROM_BUILD) ../tools/make_cart.py
	python3 ../tools/make_cart.py $(MEGACART_ROM_BUILD) $@ --type 40

stress.car.gz: $(CART_BUILD) Makefile
	gzip -9 -n -c $< > $@.tmp
	mv $@.tmp $@

stress-megacart.car.gz: $(MEGACART_CART_BUILD) Makefile
	gzip -9 -n -c $< > $@.tmp
	mv $@.tmp $@

$(BUILD_DIR):
	mkdir -p $@

spiffs.bin: Makefile stress.car.gz stress-megacart.car.gz $(IMAGE_FILES) $(CONFIG_FILE)
	@set -eu; \
	stage="$$(mktemp -d ./.spiffs-stage.XXXXXX)"; \
	trap 'rm -rf "$$stage"' EXIT; \
	for file in $(IMAGE_FILE_GLOBS); do \
		test -f "$$file" || continue; \
		[ "$$file" != stress.car ] || continue; \
		cp -- "$$file" "$$stage/"; \
	done; \
	if [ -f config.txt ]; then cp -- config.txt "$$stage/"; fi; \
	python3 "$(SPIFFS_GEN)" $(IMAGE_SIZE) "$$stage" "$@" \
		--block-size 4096 --page-size 256 --obj-name-len 32 --meta-len 4 \
		--no-magic --no-magic-len

clean:
	rm -rf .build stress.car.gz stress-megacart.car.gz spiffs.bin
