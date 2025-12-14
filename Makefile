# Makefile - Wrapper around pup/tup for orchestration tasks
#
# Usage:
#   make              # Build with pup (dogfooding)
#   make TUP=1        # Build with tup (fallback/benchmarking)
#   make test         # Run unit and E2E tests
#   make install      # Install to ~/bin (or PREFIX=/usr/local)
#   make tidy         # Run clang-tidy on all sources
#   make format       # Format all sources with clang-format
#   make clean        # Clean build outputs (keep .pup, tup.config)
#   make distclean    # Full reset: remove outputs + .pup + .tup

# Installation prefix (default: ~, so pup installs to ~/bin)
PREFIX ?= $(HOME)

# Build output directory
BUILD_DIR := build

# Detect mold linker (faster linking)
MOLD_PATH := $(shell command -v mold 2>/dev/null)
ifneq ($(MOLD_PATH),)
export USE_MOLD := 1
endif

# Build tool selection: pup by default, tup with TUP=1
# Priority: ./build/pup > pup in PATH > tup
PUP_LOCAL := $(shell test -x ./$(BUILD_DIR)/pup && echo yes)
PUP_PATH := $(shell command -v pup 2>/dev/null)

ifdef TUP
	BUILD_CMD = tup
	ifeq ("$(V)", "1")
		BUILD_OPTIONS = --verbose
	else
		BUILD_OPTIONS = --quiet
	endif
	INIT_CMD = tup init
else ifeq ($(PUP_LOCAL),yes)
	BUILD_CMD = ./$(BUILD_DIR)/pup build -B $(BUILD_DIR)
	ifeq ("$(V)", "1")
		BUILD_OPTIONS = -v
	endif
	INIT_CMD = ./$(BUILD_DIR)/pup init
else ifneq ($(PUP_PATH),)
	BUILD_CMD = pup build -B $(BUILD_DIR)
	ifeq ("$(V)", "1")
		BUILD_OPTIONS = -v
	endif
	INIT_CMD = pup init
else
	# Bootstrap: no pup available, use tup
	BUILD_CMD = tup
	ifeq ("$(V)", "1")
		BUILD_OPTIONS = --verbose
	else
		BUILD_OPTIONS = --quiet
	endif
	INIT_CMD = tup init
endif

# Source files
CXX_SOURCES := $(shell find src -name '*.cpp')
HPP_HEADERS := $(shell find include -name '*.hpp')
ALL_SOURCES := $(CXX_SOURCES) $(HPP_HEADERS)

# Compilation database for clang-tidy
COMPDB := compile_commands.json

.PHONY: all build test install tidy tidy-fix format check clean distclean compdb

all: build

build:
ifdef TUP
	@test -d .tup || tup init
else ifeq ($(PUP_LOCAL),yes)
	@test -d .pup || ./$(BUILD_DIR)/pup init
else ifneq ($(PUP_PATH),)
	@test -d .pup || pup init
else
	@test -d .tup || tup init
endif
	@mkdir -p $(BUILD_DIR)
	$(BUILD_CMD) $(BUILD_OPTIONS)

test: build
	./$(BUILD_DIR)/test/unit/pup_test
	./test/e2e/run_tests.sh

install: build
	@mkdir -p $(PREFIX)/bin
	install -m 755 $(BUILD_DIR)/pup $(PREFIX)/bin/pup
	@echo "Installed pup to $(PREFIX)/bin/pup"

# Generate compile_commands.json using pup export compdb
compdb: build
	@echo "Generating compile_commands.json..."
	@./$(BUILD_DIR)/pup export compdb -B $(BUILD_DIR) > $(COMPDB)

# Run clang-tidy on all source files (uses compile_commands.json)
tidy: compdb
	@echo "Running clang-tidy..."
	@run-clang-tidy -p . $(CXX_SOURCES)

# Run clang-tidy with automatic fixes
tidy-fix: compdb
	@echo "Running clang-tidy with fixes..."
	@run-clang-tidy -p . -fix $(CXX_SOURCES)

# Format all source files
format:
	@echo "Formatting sources..."
	@clang-format -i $(ALL_SOURCES)

# Check formatting without modifying (for CI)
format-check:
	@echo "Checking format..."
	@clang-format --dry-run --Werror $(ALL_SOURCES)

# Full check: format + tidy + test
check: format-check tidy test
	@echo "All checks passed."

clean:
ifeq ($(PUP_LOCAL),yes)
	./$(BUILD_DIR)/pup clean -B $(BUILD_DIR)
else ifneq ($(PUP_PATH),)
	pup clean -B $(BUILD_DIR)
else ifdef TUP
	@echo "Note: tup doesn't have a clean command, use 'make distclean'"
else
	rm -rf $(BUILD_DIR)/*.o $(BUILD_DIR)/pup $(BUILD_DIR)/test
endif

distclean:
ifeq ($(PUP_LOCAL),yes)
	./$(BUILD_DIR)/pup distclean -B $(BUILD_DIR)
else ifneq ($(PUP_PATH),)
	pup distclean -B $(BUILD_DIR)
else
	rm -rf $(BUILD_DIR) .pup .tup
endif
