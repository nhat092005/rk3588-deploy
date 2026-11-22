# Convenience wrapper around the CMake build.
# The actual build system is CMake; this Makefile only saves typing.
# Default: cross-compile for the board into build-arm/. The toolchain is a variable:
#   make                                     cross build (board)
#   make TOOLCHAIN= BUILD_DIR=build          native build (PC), probes and board tests skipped
#   make BUILD_TYPE=Release
#   make CMAKE_ARGS=-DRK3588_SYSROOT=<path>

SHELL := /bin/sh

.DEFAULT_GOAL := build

TOOLCHAIN  ?= cmake/aarch64-toolchain.cmake
BUILD_DIR  ?= build-arm
BUILD_TYPE ?= Debug
CMAKE_ARGS ?=

CMAKE        = cmake
CLANG_FORMAT = clang-format
CLANG_TIDY   = clang-tidy
CMAKE_FORMAT = cmake-format

SOURCES := $(shell find app library test probes bench \( -name '*.cpp' -o -name '*.h' \))
# aarch64-toolchain.cmake is hand-formatted (comments, one flag per line), cmake-format would rewrite it
CMAKE_SOURCES := $(shell find . -path './build*' -prune -o \( -name CMakeLists.txt -o -name '*.cmake' \) ! -name aarch64-toolchain.cmake -print)

.PHONY: all build configure clean distclean format tidy help

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) $(if $(TOOLCHAIN),-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN)) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_ARGS)

build: configure
	$(CMAKE) --build $(BUILD_DIR)

# Keep configured cache, drop compiled objects/binaries only.
clean:
	@if [ -d $(BUILD_DIR) ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

# Wipe the whole build tree, including CMakeCache.
distclean:
	rm -rf $(BUILD_DIR)

format:
	$(CLANG_FORMAT) -i $(SOURCES)
	$(CMAKE_FORMAT) -i $(CMAKE_SOURCES)

# Needs compile_commands.json, which `configure` generates
# (CMAKE_EXPORT_COMPILE_COMMANDS is ON in the root CMakeLists.txt).
tidy: configure
	$(CLANG_TIDY) -p $(BUILD_DIR) $(SOURCES)

help:
	@echo "Targets:"
	@echo "  build       Configure + compile (default)"
	@echo "  format      clang-format -i over app/, library/, test/, probes/, bench/; cmake-format -i over CMake files"
	@echo "  tidy        clang-tidy over app/, library/, test/, probes/, bench/ (uses BUILD_DIR database)"
	@echo "  clean       Remove compiled objects/binaries, keep CMake cache"
	@echo "  distclean   Remove the entire build directory"
	@echo ""
	@echo "Variables:"
	@echo "  TOOLCHAIN   CMake toolchain file, empty = native (default cmake/aarch64-toolchain.cmake)"
	@echo "  BUILD_DIR   build directory (default build-arm)"
	@echo "  BUILD_TYPE  Debug or Release (default Debug)"
	@echo "  CMAKE_ARGS  extra cmake arguments"
	@echo ""
	@echo "What to build (app, each test, each probe): ON/OFF in cmake/build_config.cmake"
