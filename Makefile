# Makefile - Wrapper around pup for orchestration tasks
#
# Prerequisites: pup must be installed in PATH
#
# Usage:
#   make              # Configure and build
#   make test         # Run unit and E2E tests
#   make install      # Install to ~/bin (or PREFIX=/usr/local)
#   make tidy         # Run clang-tidy on all sources
#   make format       # Format all sources with clang-format
#   make clean        # Clean build outputs
#   make distclean    # Full reset: remove build directory

PREFIX ?= $(HOME)
BUILD_DIR := build

# Detect mold linker
MOLD_PATH := $(shell command -v mold 2>/dev/null)
ifneq ($(MOLD_PATH),)
export USE_MOLD := 1
endif

# Verbose mode
ifeq ("$(V)", "1")
  BUILD_OPTIONS = -v
endif

COMPDB := compile_commands.json

.PHONY: all build configure test install compdb tidy tidy-fix format format-check check clean distclean

all: build

# Configure: generate tup.config (pup skips if already up-to-date)
configure:
	pup configure -B $(BUILD_DIR) $(BUILD_OPTIONS)

# Build: configure first, then build
build: configure
	pup -B $(BUILD_DIR) $(BUILD_OPTIONS)

test: build
	./$(BUILD_DIR)/test/unit/pup_test

install: build
	@mkdir -p $(PREFIX)/bin
	install -m 755 $(BUILD_DIR)/pup $(PREFIX)/bin/pup
	@echo "Installed pup to $(PREFIX)/bin/pup"

compdb: configure
	@echo "Generating compile_commands.json..."
	@pup show compdb -B $(BUILD_DIR) > $(COMPDB)

tidy: compdb
	@echo "Running clang-tidy..."
	@run-clang-tidy -p .

tidy-fix: compdb
	@echo "Running clang-tidy with fixes..."
	@run-clang-tidy -p . -fix

format:
	@echo "Formatting sources..."
	@find src include -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

format-check:
	@echo "Checking format..."
	@find src include -name '*.cpp' -o -name '*.hpp' | xargs clang-format --dry-run --Werror

check: format-check tidy test
	@echo "All checks passed."

clean:
	pup clean -B $(BUILD_DIR)

distclean:
	pup distclean -B $(BUILD_DIR)
