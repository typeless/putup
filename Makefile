# Makefile - Wrapper around putup for orchestration tasks
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
PUTUP ?= $(PREFIX)/bin/putup
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

# Configure: generate tup.config (putup skips if already up-to-date)
configure:
	$(PUTUP) configure -B $(BUILD_DIR) $(BUILD_OPTIONS)

# Build: configure first, then build
build: configure
	$(PUTUP) -B $(BUILD_DIR) $(BUILD_OPTIONS)

test: build
	./$(BUILD_DIR)/test/unit/putup_test

install: build
	@mkdir -p $(PREFIX)/bin
	install -m 755 $(BUILD_DIR)/putup $(PREFIX)/bin/putup
	ln -sf putup $(PREFIX)/bin/pup
	@echo "Installed putup to $(PREFIX)/bin/putup (with pup symlink)"

compdb: configure
	@echo "Generating compile_commands.json..."
	@$(PUTUP) show compdb -B $(BUILD_DIR) > $(COMPDB)

RUN_CLANG_TIDY ?= run-clang-tidy

tidy: compdb
	@echo "Running clang-tidy..."
	@$(RUN_CLANG_TIDY) -p . $(TIDY_FLAGS) 'src/|test/'

tidy-fix: compdb
	@echo "Running clang-tidy with fixes..."
	@$(RUN_CLANG_TIDY) -p . -fix $(TIDY_FLAGS) 'src/|test/'

format:
	@echo "Formatting sources..."
	@find src include -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

format-check:
	@echo "Checking format..."
	@find src include -name '*.cpp' -o -name '*.hpp' | xargs clang-format --dry-run --Werror

check: format-check tidy test
	@echo "All checks passed."

clean:
	$(PUTUP) clean -B $(BUILD_DIR)

distclean:
	$(PUTUP) distclean -B $(BUILD_DIR)
