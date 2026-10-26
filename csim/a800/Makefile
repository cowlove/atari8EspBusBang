CXX ?= g++
CCACHE ?= ccache
CXX := $(CCACHE) $(CXX)
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

MAIN_DIR := ../../main
BUILD_DIR := build/
HYBRID_BUILD_DIR := build-atari/
SOURCES := $(filter-out $(MAIN_DIR)/core1.cpp,$(wildcard $(MAIN_DIR)/*.cpp))
OBJECTS := $(patsubst $(MAIN_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
OBJECTS += $(BUILD_DIR)/csim_stubs.o
DEPFILES := $(OBJECTS:.o=.d)

ATARI800_BUILD ?= /tmp/atari800-phase3
HYBRID_BIN ?= $(ATARI800_BUILD)/src/atari800-csim
HYBRID_OBJECTS := $(patsubst $(MAIN_DIR)/%.cpp,$(HYBRID_BUILD_DIR)/%.o,$(SOURCES))
HYBRID_OBJECTS += $(HYBRID_BUILD_DIR)/csim_stubs.o
HYBRID_DEPFILES := $(HYBRID_OBJECTS:.o=.d)
HYBRID_CPPFLAGS := -DCSIM -DATARI800_SIM -DBOOT_CONFIG='"CSIM"' -DTEST_SEC=$(TEST_SEC) -g -Iinclude -I../../main -I../../main/csim

.PHONY: all hybrid clean FORCE

all: build/a800-csim-runner

build/a800-csim-runner: $(OBJECTS) FORCE
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

hybrid: $(HYBRID_OBJECTS)
	$(MAKE) -C $(ATARI800_BUILD)/src -f $(CURDIR)/hybrid-link.mk atari800-hybrid \
		CSIM_OBJECTS="$(abspath $(HYBRID_OBJECTS))" HYBRID_BIN="$(abspath $(HYBRID_BIN))"

FORCE:

$(BUILD_DIR)/%.o: $(MAIN_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/csim_stubs.o: csim_stubs.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HYBRID_BUILD_DIR)/%.o: $(MAIN_DIR)/%.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HYBRID_BUILD_DIR)/csim_stubs.o: csim_stubs.cpp | $(HYBRID_BUILD_DIR)
	$(CXX) $(HYBRID_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

$(HYBRID_BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(HYBRID_BUILD_DIR)

-include $(HYBRID_DEPFILES)

-include $(DEPFILES)
