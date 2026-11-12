# Convenience wrapper around the CMake build.
# The actual build system is CMake; this Makefile only saves typing.
# Override on the CLI: make BUILD_TYPE=Release
#                       make BUILD_DIR=out

SHELL := /bin/sh

.DEFAULT_GOAL := build

BUILD_DIR  ?= build
BUILD_TYPE ?= Debug

CMAKE        = cmake
CLANG_FORMAT = clang-format
CLANG_TIDY   = clang-tidy

APP_BIN  := $(BUILD_DIR)/app/rk3588_deploy
TEST_DIR := $(BUILD_DIR)/test

SOURCES := $(shell find app library test probes \( -name '*.cpp' -o -name '*.h' \))
# probes/ is not in the native compile database, see tidy-board
TIDY_SOURCES := $(shell find app library test \( -name '*.cpp' -o -name '*.h' \))

.PHONY: all build configure run test clean distclean format tidy tidy-board help

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	$(CMAKE) --build $(BUILD_DIR)

run: build
	$(APP_BIN)

test: build
	@status=0; \
	for exe in $(TEST_DIR)/*; do \
		[ -x "$$exe" ] && [ -f "$$exe" ] || continue; \
		echo "== $$exe =="; \
		"$$exe" || status=1; \
	done; \
	exit $$status

# Keep configured cache, drop compiled objects/binaries only.
clean:
	@if [ -d $(BUILD_DIR) ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

# Wipe the whole build tree, including CMakeCache.
distclean:
	rm -rf $(BUILD_DIR)

format:
	$(CLANG_FORMAT) -i $(SOURCES)

# Needs compile_commands.json, which `configure` generates
# (CMAKE_EXPORT_COMPILE_COMMANDS is ON in the root CMakeLists.txt).
tidy: configure
	$(CLANG_TIDY) -p $(BUILD_DIR) $(TIDY_SOURCES)

# probes/ is cross-compiled (needs the sysroot and rknn_api.h), so it is checked against the cross compile database.
BUILD_ARM_DIR ?= build-arm
tidy-board:
	$(CMAKE) -S . -B $(BUILD_ARM_DIR) -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DBUILD_PROBES=ON
	$(CLANG_TIDY) -p $(BUILD_ARM_DIR) $(shell find probes -name '*.cpp')

help:
	@echo "Targets:"
	@echo "  build       Configure + compile (default)"
	@echo "  run         Build and run app/rk3588_deploy"
	@echo "  test        Build and run every executable under test/"
	@echo "  format      clang-format -i over app/, library/, test/, probes/"
	@echo "  tidy        clang-tidy over app/, library/, test/"
	@echo "  tidy-board  clang-tidy over probes/ (cross compile database, needs sysroot)"
	@echo "  clean       Remove compiled objects/binaries, keep CMake cache"
	@echo "  distclean   Remove the entire build directory"
	@echo ""
	@echo "Overrides:"
	@echo "  make BUILD_TYPE=Release"
	@echo "  make BUILD_DIR=out"
