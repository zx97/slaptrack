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

.PHONY: all clean FORCE

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

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
