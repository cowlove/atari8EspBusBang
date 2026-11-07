.DEFAULT_GOAL := all

LLVM_MOS_BIN ?= $(HOME)/opt/llvm-mos/bin
ATARIMAX_CC ?= $(LLVM_MOS_BIN)/mos-atari8-cart-atarimax128-clang
HELLO_CC ?= $(LLVM_MOS_BIN)/mos-atari8-cart-std-clang
MEGACART_CC ?= $(LLVM_MOS_BIN)/mos-atari8-cart-megacart-clang
DOS_CC ?= $(LLVM_MOS_BIN)/mos-atari8-dos-clang
IDF_PATH ?= /opt/esp-idf
SPIFFS_GEN := $(IDF_PATH)/components/spiffs/spiffsgen.py
BUILD_DIR := .build
ROM_BUILD := $(BUILD_DIR)/stress.rom
CART_BUILD := $(BUILD_DIR)/stress.car
HELLO_BUILD := $(BUILD_DIR)/hello.rom
HELLO_EXE_BUILD := $(BUILD_DIR)/hello.exe
SIMPLEMEM_BUILD := $(BUILD_DIR)/simplemem.rom
MEGACART_ROM_BUILD := $(BUILD_DIR)/stress-megacart.rom
MEGACART_CART_BUILD := $(BUILD_DIR)/stress-megacart.car
CMPHALT_EXE := $(BUILD_DIR)/cmphalt.EXE
# The spiffs partition size, from ../partitions.csv (spiffs.bin depends on it).
IMAGE_SIZE := $(shell ../tools/partinfo.sh spiffs size)
IMAGE_FILE_GLOBS := *.[cC][aA][rR] *.[rR][oO][mM] *.[aA][tT][rR] *.[cC][aA][rR].[gG][zZ] *.[rR][oO][mM].[gG][zZ] *.[aA][tT][rR].[gG][zZ]
IMAGE_FILES := $(filter-out stress.car,$(wildcard $(IMAGE_FILE_GLOBS)))
CONFIG_FILE := $(wildcard config.txt)

.PHONY: all clean

$(CMPHALT_EXE): Makefile cmphalt.c | $(BUILD_DIR)
	$(LLVM_MOS_BIN)/mos-atari8-dos-clang -Oz cmphalt.c -o $@

cmphalt: $(CMPHALT_EXE)
.PHONY: cmphalt

all: spiffs.bin

$(ROM_BUILD): Makefile stress.c read.s cartridge.h atari_memory.h | $(BUILD_DIR)
	$(ATARIMAX_CC) stress.c read.s -Oz -Wl,--defsym=__cart_rom_size=128 -o $@

$(BUILD_DIR)/hello.rom: Makefile hello.c read.s cartridge.h atari_memory.h | $(BUILD_DIR)
	$(HELLO_CC) hello.c read.s -Oz -o $@

$(SIMPLEMEM_BUILD): Makefile simplemem.c | $(BUILD_DIR)
	$(HELLO_CC) simplemem.c -Oz -o $@

simplemem.rom: $(SIMPLEMEM_BUILD)
	cp -- $< $@

$(MEGACART_ROM_BUILD): Makefile stress.c read.s cartridge.h atari_memory.h | $(BUILD_DIR)
	$(MEGACART_CC) -DSTRESS_MEGACART=1 stress.c read.s -Oz -Wl,--defsym=__cart_rom_size=128 -o $@

$(CART_BUILD): $(ROM_BUILD) ../tools/make_cart.py
	python3 ../tools/make_cart.py $(ROM_BUILD) $@ --type 41 --enable-dos

$(MEGACART_CART_BUILD): $(MEGACART_ROM_BUILD) ../tools/make_cart.py
	python3 ../tools/make_cart.py $(MEGACART_ROM_BUILD) $@ --type 29 --enable-dos

$(HELLO_EXE_BUILD): Makefile hello.c | $(BUILD_DIR)
	$(DOS_CC) -Oz hello.c -o $@

hello.rom.gz: $(HELLO_BUILD) Makefile
	gzip -n -c $< > $@.tmp
	mv $@.tmp $@

stress.car.gz: $(CART_BUILD) Makefile
	gzip -n -c $< > $@.tmp
	mv $@.tmp $@

stress-megacart.car.gz: $(MEGACART_CART_BUILD) Makefile
	gzip -n -c $< > $@.tmp
	mv $@.tmp $@

d1.atr.gz: $(HELLO_EXE_BUILD) x2.cmd x.cmd x256.cmd x192.cmd ../tools/prepare_spiffs_disk.sh
	../tools/prepare_spiffs_disk.sh d1

d2.atr.gz: $(HELLO_EXE_BUILD) $(CMPHALT_EXE) x.bat ../tools/prepare_spiffs_disk.sh
	../tools/prepare_spiffs_disk.sh d2

llvm_d1.atr.gz: $(HELLO_EXE_BUILD) ../tools/prepare_spiffs_disk.sh
	../tools/prepare_spiffs_disk.sh llvm_d1

$(BUILD_DIR):
	mkdir -p $@

spiffs.bin: hello.rom.gz simplemem.rom Makefile ../partitions.csv stress.car.gz stress-megacart.car.gz d1.atr.gz d2.atr.gz llvm_d1.atr.gz $(IMAGE_FILES) $(CONFIG_FILE)
	@set -eu; \
	stage="$$(mktemp -d ./.spiffs-stage.XXXXXX)"; \
	trap 'rm -rf "$$stage"' EXIT; \
	for file in $(IMAGE_FILE_GLOBS); do \
		test -f "$$file" || continue; \
		[ "$$file" != stress.car ] || continue; \
		echo "$$file" "$$stage"; \
		cp -- "$$file" "$$stage/"; \
	done; \
	if [ -f config.txt ]; then cp -- config.txt "$$stage/"; fi; \
	python3 "$(SPIFFS_GEN)" $(IMAGE_SIZE) "$$stage" "$@" \
		--block-size 4096 --page-size 256 --obj-name-len 32 --meta-len 4 \
		--no-magic --no-magic-len

clean:
	rm -rf .build simplemem.rom stress.car.gz stress-megacart.car.gz spiffs.bin
