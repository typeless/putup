# Makefile - Wrapper around pup/tup for orchestration tasks
#
# Usage:
#   make              # Build with pup (dogfooding)
#   make TUP=1        # Build with tup (fallback/benchmarking)
#   make test         # Run unit and E2E tests
#   make install      # Install to ~/bin (or PREFIX=/usr/local)
#   make tidy         # Run clang-tidy on all sources
#   make format       # Format all sources with clang-format
#   make clean        # Clean build artifacts

# Installation prefix (default: ~, so pup installs to ~/bin)
PREFIX ?= $(HOME)

# Build tool selection: pup by default, tup with TUP=1
# Priority: ./build/pup > pup in PATH > tup
PUP_LOCAL := $(shell test -x ./build/pup && echo yes)
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
	BUILD_CMD = ./build/pup build
	ifeq ("$(V)", "1")
		BUILD_OPTIONS = -v
	endif
	INIT_CMD = ./build/pup init
else ifneq ($(PUP_PATH),)
	BUILD_CMD = pup build
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

# Compiler flags for clang-tidy (must match Tuprules.tup)
CXXFLAGS := -std=c++20 -I include -I third_party

# Source files
CXX_SOURCES := $(shell find src -name '*.cpp')
HPP_HEADERS := $(shell find include -name '*.hpp')
ALL_SOURCES := $(CXX_SOURCES) $(HPP_HEADERS)

.PHONY: all build test install tidy tidy-fix format check clean

all: build

build:
ifdef TUP
	@test -d .tup || tup init
else ifeq ($(PUP_LOCAL),yes)
	@test -d .pup || ./build/pup init
else ifneq ($(PUP_PATH),)
	@test -d .pup || pup init
else
	@test -d .tup || tup init
endif
	$(BUILD_CMD) $(BUILD_OPTIONS)

test: build
	./build/test/unit/pup_test
	./test/e2e/run_tests.sh

install: build
	@mkdir -p $(PREFIX)/bin
	install -m 755 build/pup $(PREFIX)/bin/pup
	@echo "Installed pup to $(PREFIX)/bin/pup"

# Run clang-tidy on all source files
tidy:
	@echo "Running clang-tidy..."
	@clang-tidy $(CXX_SOURCES) -- $(CXXFLAGS)

# Run clang-tidy with automatic fixes
tidy-fix:
	@echo "Running clang-tidy with fixes..."
	@clang-tidy -fix $(CXX_SOURCES) -- $(CXXFLAGS)

# Run clang-tidy in parallel (faster on multi-core)
tidy-parallel:
	@echo "Running clang-tidy in parallel..."
	@printf '%s\n' $(CXX_SOURCES) | xargs -P$$(nproc) -I{} clang-tidy {} -- $(CXXFLAGS)

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
	./build/pup clean
else ifneq ($(PUP_PATH),)
	pup clean
else
	find build -mindepth 1 ! -name 'tup.config' -exec rm -rf {} + 2>/dev/null || true
	rm -f *.o test/unit/*.o pup test/unit/pup_test 2>/dev/null || true
endif
	rm -rf .tup .pup
ifneq ($(PUP_PATH),)
	pup init
else
	tup init
endif
