LLVM_CONFIG ?= /home/dxic/llvmlearn/llvm-project/llvm/build/bin/llvm-config

CXX      := clang++
CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags) -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags)
LIBS     := $(shell $(LLVM_CONFIG) --libs native passes) \
            $(shell $(LLVM_CONFIG) --system-libs)

BUILD_DIR := build

.DEFAULT_GOAL := all

PROGRAMS := \
    $(BUILD_DIR)/opt-pipe0 \
    $(BUILD_DIR)/opt-pipe2 \
    $(BUILD_DIR)/ir-diff \
    $(BUILD_DIR)/ir-source-loc \
    $(BUILD_DIR)/ir-rename

.PHONY: all clean

all: $(PROGRAMS)

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/opt-pipe0: $(BUILD_DIR)/main.o $(BUILD_DIR)/func.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/opt-pipe2: $(BUILD_DIR)/main2.o $(BUILD_DIR)/func2.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/ir-diff: $(BUILD_DIR)/ir_diff.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/ir-source-loc: $(BUILD_DIR)/ir_source_loc.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/ir-rename: $(BUILD_DIR)/ir_rename.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
