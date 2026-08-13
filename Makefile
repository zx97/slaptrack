# Makefile for slaptrack — OpenLDAP Log Viewer (Pure ANSI edition)
#
# SPDX-License-Identifier: AGPL-3.0-or-later
# GNU Affero General Public License v3.0 (https://www.gnu.org/licenses/agpl-3.0.txt)
# Copyright (c) 2026 Manuel FLURY
# All rights reserved.
#
# Colored output follows the TUI-ANSI-METHODOLOGY section 12 (Makefile
# pattern): E/RST/BLD variables, cecho = printf "%b", named color macros,
# and an ANSI banner with a 256-color gradient.
# A silent mode (QUIET=true) disables colors.

# -------------------------------------------------------------------
# ANSI foundation (section 12 of the methodology)
# -------------------------------------------------------------------
E   = \033[
RST = $(E)0m
BLD = $(E)1m
DIM = $(E)2m
CUR_LEFT  = $(E)1D
CUR_RIGHT = $(E)1C

# 16 bright colors
RED    = $(E)91m
GREEN  = $(E)92m
YELLOW = $(E)93m
BLUE   = $(E)94m
MAGENTA= $(E)95m
CYAN   = $(E)96m
WHITE  = $(E)97m

B_RED     = $(BLD)$(RED)
B_GREEN   = $(BLD)$(GREEN)
B_YELLOW  = $(BLD)$(YELLOW)
B_BLUE    = $(BLD)$(BLUE)
B_MAGENTA = $(BLD)$(MAGENTA)
B_CYAN    = $(BLD)$(CYAN)
B_WHITE   = $(BLD)$(WHITE)

# Display mechanism: %b interprets \033 (the key of the system)
cecho = printf "%b" "$(1)"

# Line macros: $(1)=text, $(2)=suffix, $(3)=optional prefix (e.g. \n)
green   = $(call cecho,$(3)$(B_GREEN)$(1)$(RST)$(2))
red     = $(call cecho,$(3)$(B_RED)$(1)$(RST)$(2))
yellow  = $(call cecho,$(3)$(B_YELLOW)$(1)$(RST)$(2))
blue    = $(call cecho,$(3)$(B_BLUE)$(1)$(RST)$(2))
magenta = $(call cecho,$(3)$(B_MAGENTA)$(1)$(RST)$(2))
cyan    = $(call cecho,$(3)$(B_CYAN)$(1)$(RST)$(2))
dim     = $(call cecho,$(3)$(DIM)$(1)$(RST)$(2))

# Silent mode: QUIET=true → no colored output
cecho = printf "%b" "$(1)"
ifneq ($(QUIET),true)
  CE_INFO = $(call cecho,$(B_CYAN)$(1)$(RST))
  CE_WARN = $(call cecho,$(B_YELLOW)$(1)$(RST))
  CE_ERR  = $(call cecho,$(B_RED)$(1)$(RST))
  CE_OK   = $(call cecho,$(B_GREEN)$(1)$(RST))
  CE_HEAD = $(call cecho,$(B_MAGENTA)$(1)$(RST))
else
  CE_INFO = printf "%b" "$(1)"
  CE_WARN = printf "%b" "$(1)"
  CE_ERR  = printf "%b" "$(1)"
  CE_OK   = printf "%b" "$(1)"
  CE_HEAD = printf "%b" "$(1)"
endif

# -------------------------------------------------------------------
# Toolchain
# -------------------------------------------------------------------
CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread
# No ncurses! The ANSI edition links only zlib/bzip2/xz for compressed
# logs plus pthread for the follow-mode workers.
GCC_MAJOR := $(shell $(CXX) -dumpversion | cut -d. -f1)
LDFLAGS = -lz -lbz2 -llzma -pthread
ifneq ($(shell [ "$(GCC_MAJOR)" -lt 9 ] && echo old),)
  LDFLAGS += -lstdc++fs
endif

SRC_DIR = src
BUILD_DIR = build
TARGET = $(BUILD_DIR)/slaptrack
BANNER = ansi_banner.utf8

SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

TEST_DIR = tests
TEST_SOURCES = $(TEST_DIR)/test_main.cpp $(TEST_DIR)/test_log_parser.cpp $(TEST_DIR)/test_filter.cpp
TEST_TARGET = $(BUILD_DIR)/slaptrack_tests

# Single source of truth for the version: CMakeLists.txt
VERSION_H = $(SRC_DIR)/version.h
VERSION_H_IN = $(SRC_DIR)/version.h.in
BUILD_NUM := $(shell git rev-list --count HEAD 2>/dev/null || echo 1)
SLAPTRACK_VER := $(shell grep "^project" CMakeLists.txt | sed -n 's/.*VERSION *\([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p')

.PHONY: all clean test FORCE version banner

all: banner version $(TARGET)
	@$(call CE_OK,Build complete: $(TARGET))

# Print the ANSI banner (256-color gradient logo).
banner:
	@printf "%b" "$$(cat $(BANNER))\n"

# Regenerate generated files whenever the template or CMakeLists.txt changes.
version: $(VERSION_H_IN) CMakeLists.txt
	@$(call CE_INFO,Generating version files from CMakeLists.txt (v$(SLAPTRACK_VER), build $(BUILD_NUM))...)
	@mkdir -p $(SRC_DIR)
	@sed -e "s/@SLAPTRACK_VERSION@/$(SLAPTRACK_VER)/g" \
	    -e "s/@SLAPTRACK_BUILD@/$(BUILD_NUM)/" \
	    $(VERSION_H_IN) > $(VERSION_H)
	@sed -i.bak "s/1\.4\.0/$(SLAPTRACK_VER)/" \
	    $(SRC_DIR)/embedded.hpp
	@rm -f $(SRC_DIR)/embedded.hpp.bak

$(TARGET): $(OBJECTS) version
	@mkdir -p $(BUILD_DIR)
	@$(call CE_INFO,Linking $(TARGET)...)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.cpp $(VERSION_H) | $(BUILD_DIR)
	@$(call CE_INFO,Compiling $< ...)
	$(CXX) $(CXXFLAGS) -DBUILD_NUMBER=$(BUILD_NUM) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	@$(call CE_INFO,Compiling $< ...)
	$(CXX) $(CXXFLAGS) -DBUILD_NUMBER=$(BUILD_NUM) -c $< -o $@

$(TEST_TARGET): $(TEST_SOURCES) $(SRC_DIR)/log_parser.cpp $(SRC_DIR)/log_parser.h $(SRC_DIR)/filter.cpp $(SRC_DIR)/filter.h | $(BUILD_DIR)
	@$(call CE_INFO,Building tests...)
	$(CXX) $(CXXFLAGS) -Isrc -Itests $(TEST_SOURCES) $(SRC_DIR)/log_parser.cpp $(SRC_DIR)/filter.cpp -o $@

test: $(TEST_TARGET)
	@$(call CE_HEAD,Running tests...)
	$(TEST_TARGET)
	@$(call CE_OK,All tests passed)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@$(call CE_WARN,Cleaning $(BUILD_DIR)...)
	rm -rf $(BUILD_DIR)