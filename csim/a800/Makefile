CXX ?= g++
CCACHE ?= ccache
CXX := $(CCACHE) $(CXX)
CPPFLAGS += -DCSIM -Iinclude -I../../main -I../../main/csim
CXXFLAGS ?= -std=gnu++20 -Wall -Wextra -fpermissive
CXXFLAGS += -MMD -MP -fPIC
LDFLAGS += -shared -Wl,--no-undefined

MAIN_DIR := ../../main
BUILD_DIR := build
SOURCES := $(filter-out $(MAIN_DIR)/core1.cpp,$(wildcard $(MAIN_DIR)/*.cpp))
OBJECTS := $(patsubst $(MAIN_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
OBJECTS += $(BUILD_DIR)/csim_stubs.o
DEPFILES := $(OBJECTS:.o=.d)

.PHONY: all clean FORCE

all: build/a800-csim-link-check.so

build/a800-csim-link-check.so: $(OBJECTS) FORCE
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS)

FORCE:

$(BUILD_DIR)/%.o: $(MAIN_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/csim_stubs.o: csim_stubs.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPFILES)
