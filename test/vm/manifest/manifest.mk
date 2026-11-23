# Builds the validation setup's C++ tools: the Manifest library, the
# comparison step, Ground truth capture (test/vm/capture/), the harness
# (test/vm/harness/), the Test corpus generator (test/vm/corpus/) and their
# self-tests (test/vm/README.md, "Manifests and
# the comparison step", "The harness and Ground truth capture"). Plain GNU
# make, not part of the autotools build. Run it from the repository root,
# after building the library (make), which the harness links:
#
#   make -f test/vm/manifest/manifest.mk                 # all tools and self-tests
#   make -f test/vm/manifest/manifest.mk check           # ... and run the self-tests
#   make -f test/vm/manifest/manifest.mk capture-static  # statically linked capture
#   make -f test/vm/manifest/manifest.mk harness-static  # statically linked harness
#   make -f test/vm/manifest/manifest.mk corpus-static   # statically linked generator
#                                         # (in a tree configured --disable-shared)
#   make -f test/vm/manifest/manifest.mk clean
#
# Output goes to $(BUILD_DIR) (default build/vm-tools, gitignored).

SRC_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
CAPTURE_DIR := $(SRC_DIR)/../capture
HARNESS_DIR := $(SRC_DIR)/../harness
CORPUS_DIR := $(SRC_DIR)/../corpus
BUILD_DIR ?= build/vm-tools

CXX ?= g++
CXXSTD ?= -std=c++14
CXXFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Wpedantic
CPPUNIT_CFLAGS ?= $(shell cppunit-config --cflags 2>/dev/null)
CPPUNIT_LIBS ?= $(shell cppunit-config --libs 2>/dev/null || pkg-config --libs cppunit)

LIB_SRCS := manifest.cpp compare.cpp compare_cli.cpp sha256.cpp
LIB_OBJS := $(LIB_SRCS:%.cpp=$(BUILD_DIR)/%.o)
LIB := $(BUILD_DIR)/libtskmanifest.a
COMPARE := $(BUILD_DIR)/compare-manifests
SELFTEST := $(BUILD_DIR)/manifest-selftest
CAPTURE := $(BUILD_DIR)/capture-ground-truth
HARNESS := $(BUILD_DIR)/tsk-harness
HARNESS_SELFTEST := $(BUILD_DIR)/harness-selftest
CORPUS := $(BUILD_DIR)/tsk-corpus

.PHONY: all check clean capture-static harness-static corpus-static
all: $(LIB) $(COMPARE) $(SELFTEST) $(CAPTURE) $(HARNESS) $(HARNESS_SELFTEST) $(CORPUS)

check: $(SELFTEST) $(HARNESS_SELFTEST)
	$(SELFTEST)
	$(HARNESS_SELFTEST)

# Dependency files go in $(BUILD_DIR)/.deps/*.Po, like automake's, and the
# sources are compiled by absolute path, so they list absolute header paths:
# gate 1's C++17/C++20 check finds the sources that include a changed header
# through these files (test/vm/lib/cxx_std_check.py).
DEP_DIR := $(BUILD_DIR)/.deps

DEPFLAGS = -MMD
COMPILE = $(CXX) $(CXXSTD) $(WARNINGS) $(CXXFLAGS) $(CPPFLAGS) $(DEPFLAGS) -MP -MF $(DEP_DIR)/$*.Po -MT $@ -c -o $@ $(abspath $<)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR) $(DEP_DIR)
	$(COMPILE)

# Ground truth capture (test/vm/capture/): uses manifest.h, so it sees
# $(SRC_DIR) as an include directory.
$(BUILD_DIR)/%.o: $(CAPTURE_DIR)/%.cpp | $(BUILD_DIR) $(DEP_DIR)
	$(COMPILE)
$(BUILD_DIR)/capture.o $(BUILD_DIR)/capture_main.o $(BUILD_DIR)/capture_selftest.o: CPPFLAGS += -I$(abspath $(SRC_DIR))

# The Test corpus generator (test/vm/corpus/). Its self-test checks the
# corpus through Ground truth capture, so it sees both directories.
$(BUILD_DIR)/%.o: $(CORPUS_DIR)/%.cpp | $(BUILD_DIR) $(DEP_DIR)
	$(COMPILE)
$(BUILD_DIR)/corpus_selftest.o: CPPFLAGS += -I$(abspath $(SRC_DIR)) -I$(abspath $(CAPTURE_DIR))

# The harness (test/vm/harness/) may use only the public libtsk API, so it is
# compiled against a staged install of the library (make install with
# DESTDIR=$(TSK_STAGE)): its only libtsk include directory holds the
# installed headers and nothing else. It is a system include directory
# (-isystem), so warnings in libtsk's headers don't show up as the harness's.
# Build the library (make) first.
TSK_PC := tsk/tsk.pc
TSK_STAGE := $(abspath $(BUILD_DIR))/tsk-stage
TSK_STAGE_STAMP := $(BUILD_DIR)/tsk-stage.stamp
tsk_pc_var = $(shell sed -n 's/^$(1)=//p' $(TSK_PC) 2>/dev/null)
TSK_STAGE_INCLUDE = $(TSK_STAGE)$(call tsk_pc_var,includedir)
TSK_STAGE_LIBDIR = $(TSK_STAGE)$(call tsk_pc_var,libdir)
TSK_SOURCES := $(shell find tsk -name '*.h' -o -name '*.hpp' 2>/dev/null)
HARNESS_OBJS := $(BUILD_DIR)/harness.o $(BUILD_DIR)/harness_main.o \
    $(BUILD_DIR)/harness_selftest.o
HARNESS_LIBS = -L$(TSK_STAGE_LIBDIR) -ltsk -Wl,-rpath,$(TSK_STAGE_LIBDIR)

$(TSK_STAGE_STAMP): tsk/libtsk.la $(TSK_PC) $(TSK_SOURCES) | $(BUILD_DIR)
	rm -rf $(TSK_STAGE)
	{ $(MAKE) install-nobase_includeHEADERS DESTDIR=$(TSK_STAGE) && \
	  $(MAKE) -C tsk install-libLTLIBRARIES install-nodist_pkgconfigDATA DESTDIR=$(TSK_STAGE); } \
	  >$(BUILD_DIR)/tsk-stage.log 2>&1 || { cat $(BUILD_DIR)/tsk-stage.log; exit 1; }
	touch $@

$(BUILD_DIR)/%.o: $(HARNESS_DIR)/%.cpp | $(BUILD_DIR) $(DEP_DIR) $(TSK_STAGE_STAMP)
	$(COMPILE)
$(HARNESS_OBJS): CPPFLAGS += -I$(abspath $(SRC_DIR)) -isystem $(TSK_STAGE_INCLUDE)
$(BUILD_DIR)/harness_selftest.o: CPPFLAGS += $(CPPUNIT_CFLAGS)
# -MD, not -MMD: the dependency files then list the -isystem libtsk headers
# too, so a change to them rebuilds the harness, and the harness self-test
# can check that every libtsk header came from the staged install.
$(HARNESS_OBJS): DEPFLAGS = -MD


$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(COMPARE): $(BUILD_DIR)/compare_main.o $(LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

SELFTEST_OBJS := $(BUILD_DIR)/selftest.o $(BUILD_DIR)/sha256_selftest.o \
    $(BUILD_DIR)/capture_selftest.o $(BUILD_DIR)/corpus_selftest.o
$(SELFTEST_OBJS): CPPFLAGS += $(CPPUNIT_CFLAGS)
$(SELFTEST): $(SELFTEST_OBJS) $(BUILD_DIR)/capture.o $(BUILD_DIR)/corpus.o $(LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(CPPUNIT_LIBS)

$(CAPTURE): $(BUILD_DIR)/capture_main.o $(BUILD_DIR)/capture.o $(LIB)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

# Ground truth capture runs in the Validation VM's guest, whose glibc is older
# than the devcontainer's, so it must link fully statically with no library
# beyond libstdc++ and glibc. The harness self-test checks that it does.
CAPTURE_STATIC := $(BUILD_DIR)/static/capture-ground-truth
capture-static: $(CAPTURE_STATIC)
$(CAPTURE_STATIC): $(BUILD_DIR)/capture_main.o $(BUILD_DIR)/capture.o $(LIB)
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -static -o $@ $^

$(CORPUS): $(BUILD_DIR)/corpus_main.o $(BUILD_DIR)/corpus.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

# The generator runs in the guest too: fully static, like capture-static.
CORPUS_STATIC := $(BUILD_DIR)/static/tsk-corpus
corpus-static: $(CORPUS_STATIC)
$(CORPUS_STATIC): $(BUILD_DIR)/corpus_main.o $(BUILD_DIR)/corpus.o
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -static -o $@ $^

# The harness for the guest (Guest-side runs): fully static, like
# capture-static. Build it in a tree configured --disable-shared, so the staged
# library is libtsk.a. Its own dependencies are the -l flags of libtsk.la's
# dependency_libs (tsk.pc's Libs.private leaves out zlib); g++ adds libstdc++.
# test/vm/guest-build.sh does this in build/guest-static/.
HARNESS_STATIC := $(BUILD_DIR)/static/tsk-harness
TSK_LIBS_PRIVATE = $(filter -l%,$(shell sed -n "s/^dependency_libs='\(.*\)'/\1/p" tsk/libtsk.la 2>/dev/null))
harness-static: $(HARNESS_STATIC)
$(HARNESS_STATIC): $(BUILD_DIR)/harness_main.o $(BUILD_DIR)/harness.o $(LIB) $(TSK_STAGE_STAMP)
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -static -o $@ $(filter %.o %.a,$^) \
	    -L$(TSK_STAGE_LIBDIR) -ltsk $(TSK_LIBS_PRIVATE) -lpthread

$(HARNESS): $(BUILD_DIR)/harness_main.o $(BUILD_DIR)/harness.o $(LIB) $(TSK_STAGE_STAMP)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(filter %.o %.a,$^) $(HARNESS_LIBS)

$(HARNESS_SELFTEST): $(BUILD_DIR)/harness_selftest.o $(BUILD_DIR)/harness.o $(LIB) $(TSK_STAGE_STAMP)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(filter %.o %.a,$^) $(HARNESS_LIBS) $(CPPUNIT_LIBS)

$(BUILD_DIR) $(DEP_DIR):
	mkdir -p $@

clean:
	rm -f $(BUILD_DIR)/*.o $(DEP_DIR)/*.Po $(LIB) $(COMPARE) $(SELFTEST) $(CAPTURE) \
	    $(HARNESS) $(HARNESS_SELFTEST) $(TSK_STAGE_STAMP) $(CAPTURE_STATIC) \
	    $(HARNESS_STATIC) $(CORPUS) $(CORPUS_STATIC)
	rm -rf $(TSK_STAGE)

-include $(wildcard $(DEP_DIR)/*.Po)
