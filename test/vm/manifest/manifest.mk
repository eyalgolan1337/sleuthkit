# Builds the Manifest library, the comparison step and its self-test
# (test/vm/README.md, "Manifests and the comparison step"). Plain GNU make, not
# part of the autotools build. Run it from the repository root:
#
#   make -f test/vm/manifest/manifest.mk            # library, comparison step, self-test
#   make -f test/vm/manifest/manifest.mk check      # ... and run the self-test
#   make -f test/vm/manifest/manifest.mk clean
#
# Output goes to $(BUILD_DIR) (default build/vm-tools, gitignored).

SRC_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= build/vm-tools

CXX ?= g++
CXXSTD ?= -std=c++14
CXXFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Wpedantic
CPPUNIT_CFLAGS ?= $(shell cppunit-config --cflags 2>/dev/null)
CPPUNIT_LIBS ?= $(shell cppunit-config --libs 2>/dev/null || pkg-config --libs cppunit)

LIB_SRCS := manifest.cpp compare.cpp compare_cli.cpp
LIB_OBJS := $(LIB_SRCS:%.cpp=$(BUILD_DIR)/%.o)
LIB := $(BUILD_DIR)/libtskmanifest.a
COMPARE := $(BUILD_DIR)/compare-manifests
SELFTEST := $(BUILD_DIR)/manifest-selftest

.PHONY: all check clean
all: $(LIB) $(COMPARE) $(SELFTEST)

check: $(SELFTEST)
	$(SELFTEST)

# Dependency files go in $(BUILD_DIR)/.deps/*.Po, like automake's, and the
# sources are compiled by absolute path, so they list absolute header paths:
# gate 1's C++17/C++20 check finds the sources that include a changed header
# through these files (test/vm/lib/cxx_std_check.py).
DEP_DIR := $(BUILD_DIR)/.deps

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR) $(DEP_DIR)
	$(CXX) $(CXXSTD) $(WARNINGS) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $(DEP_DIR)/$*.Po -MT $@ -c -o $@ $(abspath $<)

$(BUILD_DIR)/selftest.o: CPPFLAGS += $(CPPUNIT_CFLAGS)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(COMPARE): $(BUILD_DIR)/compare_main.o $(LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

$(SELFTEST): $(BUILD_DIR)/selftest.o $(LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(CPPUNIT_LIBS)

$(BUILD_DIR) $(DEP_DIR):
	mkdir -p $@

clean:
	rm -f $(BUILD_DIR)/*.o $(DEP_DIR)/*.Po $(LIB) $(COMPARE) $(SELFTEST)

-include $(wildcard $(DEP_DIR)/*.Po)
