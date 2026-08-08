CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra

# -------------------------------------------------------------------
# std::filesystem linkage: GCC < 9 needs -lstdc++fs; GCC >= 9 does not
# (libstdc++fs is gone from GCC 14 / Fedora 42+).  On very old
# toolchains the separate archive is still present and must be linked.
# -------------------------------------------------------------------
GCC_MAJOR := $(shell $(CXX) -dumpversion | cut -d. -f1)
LDFLAGS = -lncurses -lz -lbz2 -llzma
ifneq ($(shell [ "$(GCC_MAJOR)" -lt 9 ] && echo old),)
  LDFLAGS += -lstdc++fs
endif

SRC_DIR = src
BUILD_DIR = build
TARGET = $(BUILD_DIR)/slaptrack

# Use wildcard so any new .cpp file in src/ is picked up automatically.
# No need to edit this list when adding a new source file.
SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

BUILD_FILE = build_number.txt
TEST_DIR = tests
TEST_SOURCES = $(TEST_DIR)/test_main.cpp $(TEST_DIR)/test_log_parser.cpp $(TEST_DIR)/test_filter.cpp
TEST_TARGET = $(BUILD_DIR)/slaptrack_tests

.PHONY: all clean test FORCE

all: $(TARGET)

FORCE:

# Auto-increment build number on every build
$(BUILD_FILE): FORCE
	@expr $$(cat $@ 2>/dev/null || echo 0) + 1 > $@

$(TARGET): $(OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.cpp $(BUILD_FILE) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DBUILD_NUMBER=$$(cat $(BUILD_FILE)) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DBUILD_NUMBER=$$(cat $(BUILD_FILE)) -c $< -o $@

# Unit tests: compile the parser/filter units (no ncurses needed) plus
# the harness and run them.  No test framework dependency.
$(TEST_TARGET): $(TEST_SOURCES) $(SRC_DIR)/log_parser.cpp $(SRC_DIR)/log_parser.h $(SRC_DIR)/filter.cpp $(SRC_DIR)/filter.h $(SRC_DIR)/log_parser.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc -Itests $(TEST_SOURCES) $(SRC_DIR)/log_parser.cpp $(SRC_DIR)/filter.cpp -o $@

test: $(TEST_TARGET)
	$(TEST_TARGET)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
