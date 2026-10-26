CXX ?= g++
CCACHE ?= ccache
CXX := $(CCACHE) $(CXX)
PROFILE ?= 0
PGFLAGS ?= -pg
RUNTIME_SEC ?= 30000
TEST_SEC ?= -1
CPPFLAGS += -DCSIM -DATARI800_SIM -DBOOT_CONFIG='"CSIM"' -DTEST_SEC=$(TEST_SEC) -g -Iinclude -I../../main -I../../main/csim
RUNNER_MODE ?= 1
ifeq ($(RUNNER_MODE),1)
CPPFLAGS += -DATARI800_SIM_RUNNER
endif
CPPFLAGS += -DRUNTIME_SEC=$(RUNTIME_SEC)
CXXFLAGS ?= -std=gnu++20 -Wall -Wextra -fpermissive
CXXFLAGS += -MMD -MP -fPIC
LDFLAGS += -g -Wl,--no-undefined
LDLIBS += -lz

ifeq ($(PROFILE),1)
CSIM_PROFILE_FLAGS := $(PGFLAGS)
ATARI800_PROFILE_FLAGS := $(PGFLAGS)
endif
CXXFLAGS += $(CSIM_PROFILE_FLAGS)
LDFLAGS += $(CSIM_PROFILE_FLAGS)

MAIN_DIR := ../../main
BUILD_DIR := build/
HYBRID_BUILD_DIR := build-atari/
SOURCES := $(filter-out $(MAIN_DIR)/core1.cpp,$(wildcard $(MAIN_DIR)/*.cpp))
OBJECTS := $(patsubst $(MAIN_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
OBJECTS += $(BUILD_DIR)/csim_stubs.o
DEPFILES := $(OBJECTS:.o=.d)

ATARI800_BUILD ?= $(CURDIR)/atari800
ATARI800_CONFIGURE_ARGS ?= --with-video=sdl2 --with-sound=sdl2 --disable-audiorecording --disable-dependency-tracking
ATARI800_CFLAGS ?= -g -O2 -Wno-error=unused-function
ATARI800_CXXFLAGS ?=
ATARI800_LDFLAGS ?=
ATARI800_CONFIG_CFLAGS := $(ATARI800_CFLAGS) $(ATARI800_PROFILE_FLAGS)
ATARI800_CONFIG_CXXFLAGS := $(ATARI800_CXXFLAGS) $(ATARI800_PROFILE_FLAGS)
ATARI800_CONFIG_LDFLAGS := $(ATARI800_LDFLAGS) $(ATARI800_PROFILE_FLAGS)
ATARI800_PROFILE_SIGNATURE := PROFILE=$(PROFILE);PGFLAGS=$(PGFLAGS)
HYBRID_BIN ?= $(CURDIR)/build-atari/atari800-csim
HYBRID_OBJECTS := $(patsubst $(MAIN_DIR)/%.cpp,$(HYBRID_BUILD_DIR)/%.o,$(SOURCES))
HYBRID_OBJECTS += $(HYBRID_BUILD_DIR)/csim_stubs.o
HYBRID_DEPFILES := $(HYBRID_OBJECTS:.o=.d)
HYBRID_CPPFLAGS := -DCSIM -DATARI800_SIM -DBOOT_CONFIG='"CSIM"' -DTEST_SEC=$(TEST_SEC) -g -Iinclude -I../../main -I../../main/csim
PROFILE_FLAGS_STAMP := $(BUILD_DIR)/.profile-flags
ATARI800_PROFILE_STAMP := $(abspath $(HYBRID_BUILD_DIR).atari800-profile-flags)

.PHONY: all hybrid atari800-configure clean FORCE

all: build/a800-csim-runner

build/a800-csim-runner: $(OBJECTS) FORCE
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

hybrid: $(HYBRID_OBJECTS) atari800-configure
	$(MAKE) -C $(ATARI800_BUILD)/src -f $(CURDIR)/hybrid-link.mk atari800-hybrid \
		CSIM_OBJECTS="$(abspath $(HYBRID_OBJECTS))" HYBRID_BIN="$(abspath $(HYBRID_BIN))"

atari800-configure: | $(HYBRID_BUILD_DIR)
	@if [ ! -x "$(ATARI800_BUILD)/configure" ]; then \
		cd "$(ATARI800_BUILD)" && ./autogen.sh; \
	fi
	@if [ ! -f "$(ATARI800_BUILD)/src/Makefile" ] || \
	   [ ! -f "$(ATARI800_PROFILE_STAMP)" ] || \
	   [ "$$(cat "$(ATARI800_PROFILE_STAMP)" 2>/dev/null)" != '$(ATARI800_PROFILE_SIGNATURE)' ]; then \
		if [ -f "$(ATARI800_BUILD)/src/Makefile" ]; then \
			$(MAKE) -C "$(ATARI800_BUILD)/src" clean; \
		fi; \
		cd "$(ATARI800_BUILD)" && \
			CFLAGS="$(ATARI800_CONFIG_CFLAGS)" \
			CXXFLAGS="$(ATARI800_CONFIG_CXXFLAGS)" \
			LDFLAGS="$(ATARI800_CONFIG_LDFLAGS)" \
			./configure $(ATARI800_CONFIGURE_ARGS) && \
		printf '%s\n' '$(ATARI800_PROFILE_SIGNATURE)' > "$(ATARI800_PROFILE_STAMP)"; \
	fi

FORCE:

$(BUILD_DIR)/%.o: $(MAIN_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/csim_stubs.o: csim_stubs.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HYBRID_BUILD_DIR)/%.o: $(MAIN_DIR)/%.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HYBRID_BUILD_DIR)/csim_stubs.o: csim_stubs.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJECTS) $(HYBRID_OBJECTS): $(PROFILE_FLAGS_STAMP)

$(PROFILE_FLAGS_STAMP): FORCE | $(BUILD_DIR)
	@printf '%s\n' 'PROFILE=$(PROFILE);PGFLAGS=$(PGFLAGS)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm $@.tmp; fi

$(BUILD_DIR):
	mkdir -p $@

$(HYBRID_BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(HYBRID_BUILD_DIR)

-include $(HYBRID_DEPFILES)

-include $(DEPFILES)
