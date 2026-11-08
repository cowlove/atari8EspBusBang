CXX ?= g++
CCACHE ?= ccache
CXX := $(CCACHE) $(CXX)
PROFILE ?= 0
PGFLAGS ?= -pg
VIDEO ?= 0
RUN_SEC ?= 36000
BOOT_CONFIG ?= CSIM
LIBSMB2_DIR := ../libsmb2
LIBSMB2_INCLUDE_DIR := $(abspath $(LIBSMB2_DIR)/include)
LIBSMB2_BUILD_DIR := $(abspath build-libsmb2)
LIBSMB2_LIBDIR := $(LIBSMB2_BUILD_DIR)/lib
LIBSMB2_LIB := $(LIBSMB2_LIBDIR)/libsmb2.so
LIBSMB2_CPPFLAGS := -I$(LIBSMB2_INCLUDE_DIR)/smb2 -I$(LIBSMB2_INCLUDE_DIR)
CSIM_RPATH := -Wl,-rpath,'$$ORIGIN/../build-libsmb2/lib'
CPPFLAGS += $(LIBSMB2_CPPFLAGS) -DCSIM -DATARI800_SIM -DBOOT_CONFIG='"$(BOOT_CONFIG)"' -DRUN_SEC=$(RUN_SEC) -g -Iinclude -I../../main -I../../main/csim -I../../components/sidecar_link
# The Sidecar app runs inside CSIM over the simulated i2slink
# (csim_sidecar.cpp); csim.h turns on A8BB_SIDECAR_ENABLE.
SIDECAR_LINK_DIR := ../../components/sidecar_link
SIDECAR_APP_DIR := ../../sidecar/main
# Disk images (components/disk_image), shared by the host and Sidecar apps.
DISK_IMAGE_DIR := ../../components/disk_image
SIDECAR_CPPFLAGS := -I$(SIDECAR_APP_DIR) -I$(DISK_IMAGE_DIR)
CPPFLAGS += $(SIDECAR_CPPFLAGS)
RUNNER_MODE ?= 1
ifeq ($(RUNNER_MODE),1)
CPPFLAGS += -DATARI800_SIM_RUNNER
endif
CXXFLAGS ?= -std=gnu++20 -Wall -Wno-sign-compare -fpermissive
CXXFLAGS += -MMD -MP -fPIC
LDFLAGS += -g -Wl,--no-undefined $(CSIM_RPATH)
LDLIBS += $(LIBSMB2_LIB) -lz

ifeq ($(PROFILE),1)
CSIM_PROFILE_FLAGS := $(PGFLAGS)
ATARI800_PROFILE_FLAGS := $(PGFLAGS)
endif
CXXFLAGS += $(CSIM_PROFILE_FLAGS)
LDFLAGS += $(CSIM_PROFILE_FLAGS)

MAIN_DIR := ../../main
BUILD_DIR := build
HYBRID_BUILD_DIR := build-atari
SOURCES := $(filter-out $(MAIN_DIR)/core1.cpp,$(wildcard $(MAIN_DIR)/*.cpp))
OBJECTS := $(patsubst $(MAIN_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
OBJECTS += $(BUILD_DIR)/csim_stubs.o
SIDECAR_OBJECT_NAMES := sidecar.o i2slink_csim.o sidecarApp.o csim_sidecar.o diskFlash.o gzipImage.o diskImageSmb.o sio.o
OBJECTS += $(addprefix $(BUILD_DIR)/,$(SIDECAR_OBJECT_NAMES))
DEPFILES := $(OBJECTS:.o=.d)

ATARI800_BUILD ?= $(CURDIR)/atari800
ifeq ($(VIDEO),1)
ATARI800_DEFAULT_CONFIGURE_ARGS := --with-video=sdl2 --with-sound=sdl2
else
ATARI800_DEFAULT_CONFIGURE_ARGS := --with-video=no --with-sound=no
endif
ATARI800_CONFIGURE_ARGS ?= $(ATARI800_DEFAULT_CONFIGURE_ARGS) --disable-audiorecording --disable-dependency-tracking
ATARI800_CFLAGS ?= -g -Ofast -Wno-error=unused-function
ATARI800_CXXFLAGS ?=
ATARI800_LDFLAGS ?=
ATARI800_CC ?= $(CCACHE) gcc
ATARI800_CONFIG_CFLAGS := $(ATARI800_CFLAGS) $(ATARI800_PROFILE_FLAGS)
ATARI800_CONFIG_CXXFLAGS := $(ATARI800_CXXFLAGS) $(ATARI800_PROFILE_FLAGS)
ATARI800_CONFIG_LDFLAGS := $(ATARI800_LDFLAGS) $(ATARI800_PROFILE_FLAGS)
ATARI800_CONFIG_SIGNATURE := PROFILE=$(PROFILE);VIDEO=$(VIDEO);PGFLAGS=$(PGFLAGS);CC=$(ATARI800_CC)
HYBRID_BIN ?= $(CURDIR)/build-atari/atari800-csim
HYBRID_OBJECTS := $(patsubst $(MAIN_DIR)/%.cpp,$(HYBRID_BUILD_DIR)/%.o,$(SOURCES))
HYBRID_OBJECTS += $(HYBRID_BUILD_DIR)/csim_stubs.o
HYBRID_OBJECTS += $(addprefix $(HYBRID_BUILD_DIR)/,$(SIDECAR_OBJECT_NAMES))
HYBRID_DEPFILES := $(HYBRID_OBJECTS:.o=.d)
HYBRID_CPPFLAGS := $(LIBSMB2_CPPFLAGS) -DCSIM -DATARI800_SIM -DBOOT_CONFIG='"$(BOOT_CONFIG)"' -DRUN_SEC=$(RUN_SEC) -g -Iinclude -I../../main -I../../main/csim -I../../components/sidecar_link
HYBRID_CPPFLAGS += $(SIDECAR_CPPFLAGS)
PROFILE_FLAGS_STAMP := $(BUILD_DIR)/.profile-flags
ATARI800_PROFILE_STAMP := $(abspath $(HYBRID_BUILD_DIR).atari800-profile-flags)

.PHONY: all hybrid atari800-configure clean FORCE

all: hybrid

runner: build/a800-csim-runner

build/a800-csim-runner: $(OBJECTS) $(LIBSMB2_LIB) FORCE
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

hybrid: $(HYBRID_OBJECTS) atari800-configure
	$(MAKE) -C $(ATARI800_BUILD)/src -f $(CURDIR)/hybrid-link.mk atari800-hybrid \
		CSIM_OBJECTS="$(abspath $(HYBRID_OBJECTS))" HYBRID_BIN="$(abspath $(HYBRID_BIN))" \
		CSIM_LDLIBS="$(LIBSMB2_LIB) -lz"

atari800-configure: | $(HYBRID_BUILD_DIR)
	@if [ ! -x "$(ATARI800_BUILD)/configure" ]; then \
		cd "$(ATARI800_BUILD)" && ./autogen.sh; \
	fi
	configure_atari800() { \
		if [ -f "$(ATARI800_BUILD)/src/Makefile" ]; then \
			$(MAKE) -C "$(ATARI800_BUILD)/src" clean; \
		fi; \
		( cd "$(ATARI800_BUILD)" && \
			CC="$(ATARI800_CC)" \
			CFLAGS="$(ATARI800_CONFIG_CFLAGS)" \
			CXXFLAGS="$(ATARI800_CONFIG_CXXFLAGS)" \
			LDFLAGS="$(ATARI800_CONFIG_LDFLAGS)" \
			./configure $(ATARI800_CONFIGURE_ARGS) ); \
	}; \
	if [ ! -f "$(ATARI800_BUILD)/src/Makefile" ]; then \
		configure_atari800 && printf '%s\n' '$(ATARI800_CONFIG_SIGNATURE)' > "$(ATARI800_PROFILE_STAMP)"; \
	elif [ ! -f "$(ATARI800_PROFILE_STAMP)" ]; then \
		configure_atari800 || exit $$?; \
		printf '%s\n' '$(ATARI800_CONFIG_SIGNATURE)' > "$(ATARI800_PROFILE_STAMP)"; \
	elif [ "$$(cat "$(ATARI800_PROFILE_STAMP)")" != '$(ATARI800_CONFIG_SIGNATURE)' ]; then \
		configure_atari800 && printf '%s\n' '$(ATARI800_CONFIG_SIGNATURE)' > "$(ATARI800_PROFILE_STAMP)"; \
	fi

FORCE:

# pbirom.h is generated from pbirom.asm by main/Makefile, which tracks its
# dependencies, so ask that Makefile to bring it up to date on every build,
# before anything compiles (order-only; not in git, so a fresh checkout
# needs it too). Its path matches the compiler's .d files, and make re-reads
# its timestamp after the recipe, so only the objects that include it
# (mmu.cpp, pbi.cpp) rebuild, and only when it actually changed.
PBIROM_H := $(MAIN_DIR)/pbirom.h
$(PBIROM_H): FORCE
	$(MAKE) -C $(MAIN_DIR) pbirom.h

$(OBJECTS) $(HYBRID_OBJECTS): | $(PBIROM_H)

$(BUILD_DIR)/%.o: $(MAIN_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/csim_stubs.o: csim_stubs.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/sidecar.o $(BUILD_DIR)/i2slink_csim.o: $(BUILD_DIR)/%.o: $(SIDECAR_LINK_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/sidecarApp.o: $(SIDECAR_APP_DIR)/sidecarApp.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/diskFlash.o $(BUILD_DIR)/gzipImage.o $(BUILD_DIR)/diskImageSmb.o $(BUILD_DIR)/sio.o: $(BUILD_DIR)/%.o: $(DISK_IMAGE_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/csim_sidecar.o: csim_sidecar.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(LIBSMB2_LIB):
	@test -f "$(LIBSMB2_DIR)/CMakeLists.txt" || { \
		echo "libsmb2 submodule missing; run git submodule update --init csim/libsmb2" >&2; \
		exit 1; \
	}
	cmake -S "$(LIBSMB2_DIR)" -B "$(LIBSMB2_BUILD_DIR)" -DBUILD_SHARED_LIBS=ON
	cmake --build "$(LIBSMB2_BUILD_DIR)" --parallel

$(HYBRID_BUILD_DIR)/%.o: $(MAIN_DIR)/%.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HYBRID_BUILD_DIR)/csim_stubs.o: csim_stubs.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HYBRID_BUILD_DIR)/sidecar.o $(HYBRID_BUILD_DIR)/i2slink_csim.o: $(HYBRID_BUILD_DIR)/%.o: $(SIDECAR_LINK_DIR)/%.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HYBRID_BUILD_DIR)/sidecarApp.o: $(SIDECAR_APP_DIR)/sidecarApp.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HYBRID_BUILD_DIR)/diskFlash.o $(HYBRID_BUILD_DIR)/gzipImage.o $(HYBRID_BUILD_DIR)/diskImageSmb.o $(HYBRID_BUILD_DIR)/sio.o: $(HYBRID_BUILD_DIR)/%.o: $(DISK_IMAGE_DIR)/%.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HYBRID_BUILD_DIR)/csim_sidecar.o: csim_sidecar.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJECTS) $(HYBRID_OBJECTS): $(LIBSMB2_LIB)

$(OBJECTS) $(HYBRID_OBJECTS): $(PROFILE_FLAGS_STAMP)

$(PROFILE_FLAGS_STAMP): FORCE | $(BUILD_DIR)
	@printf '%s\n' 'PROFILE=$(PROFILE);PGFLAGS=$(PGFLAGS);RUN_SEC=$(RUN_SEC);BOOT_CONFIG=$(BOOT_CONFIG);LIBSMB2=$(LIBSMB2_DIR)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm $@.tmp; fi

$(BUILD_DIR):
	mkdir -p $@

$(HYBRID_BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(HYBRID_BUILD_DIR)

-include $(HYBRID_DEPFILES)

-include $(DEPFILES)
