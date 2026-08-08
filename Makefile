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

TEST_DIR = tests
TEST_SOURCES = $(TEST_DIR)/test_main.cpp $(TEST_DIR)/test_log_parser.cpp $(TEST_DIR)/test_filter.cpp
TEST_TARGET = $(BUILD_DIR)/slaptrack_tests

# Single source of truth for the version: CMakeLists.txt
# Build number: git commit count (same for all clones at the same commit)
VERSION_H = $(SRC_DIR)/version.h
VERSION_H_IN = $(SRC_DIR)/version.h.in
BUILD_NUM := $(shell git rev-list --count HEAD 2>/dev/null || echo 1)
SLAPTRACK_VER := $(shell grep "^project" CMakeLists.txt | sed -n 's/.*VERSION *\([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p')

.PHONY: all clean test FORCE version

all: version $(TARGET)

# Regenerate generated files whenever the template or CMakeLists.txt changes.
version: $(VERSION_H_IN) CMakeLists.txt
	@echo "Generating version files from CMakeLists.txt (v$(SLAPTRACK_VER), build $(BUILD_NUM))..."
	@mkdir -p $(SRC_DIR)
	@sed -e "s/@SLAPTRACK_VERSION@/$(SLAPTRACK_VER)/g" \
	    -e "s/@SLAPTRACK_BUILD@/$(BUILD_NUM)/" \
	    $(VERSION_H_IN) > $(VERSION_H)
	@sed -i.bak "s/1\.4\.0/$(SLAPTRACK_VER)/" \
	    $(SRC_DIR)/embedded.hpp
	@rm -f $(SRC_DIR)/embedded.hpp.bak

$(TARGET): $(OBJECTS) version
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.cpp $(VERSION_H) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DBUILD_NUMBER=$(BUILD_NUM) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(VERSION_H) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DBUILD_NUMBER=$(BUILD_NUM) -c $< -o $@

$(TEST_TARGET): $(TEST_SOURCES) $(SRC_DIR)/log_parser.cpp $(SRC_DIR)/log_parser.h $(SRC_DIR)/filter.cpp $(SRC_DIR)/filter.h $(SRC_DIR)/log_parser.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc -Itests $(TEST_SOURCES) $(SRC_DIR)/log_parser.cpp $(SRC_DIR)/filter.cpp -o $@

test: $(TEST_TARGET)
	$(TEST_TARGET)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
