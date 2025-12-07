# Makefile - Wrapper around tup for orchestration tasks
#
# Usage:
#   make              # Build with tup
#   make test         # Run unit and E2E tests
#   make tidy         # Run clang-tidy on all sources
#   make format       # Format all sources with clang-format
#   make clean        # Clean build artifacts

ifeq ("$(V)", "1")
	TUP_OPTIONS += --verbose
else
	TUP_OPTIONS += --quiet
endif

# Compiler flags for clang-tidy (must match Tuprules.tup)
CXXFLAGS := -std=c++23 -I include -I third_party

# Source files
CXX_SOURCES := $(shell find src -name '*.cpp')
HPP_HEADERS := $(shell find include -name '*.hpp')
ALL_SOURCES := $(CXX_SOURCES) $(HPP_HEADERS)

.PHONY: all build test tidy tidy-fix format check clean

all: build

build:
	tup $(TUP_OPTIONS)

test: build
	./build/test/unit/pup_test
	./test/e2e/run_tests.sh

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
	rm -rf .tup build
	tup init
