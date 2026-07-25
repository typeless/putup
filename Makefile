# Makefile - Wrapper around putup for orchestration tasks
#
# Usage:
#   make              # Configure and build
#   make test         # Run unit and E2E tests
#   make coverage     # Build instrumented, run tests, write gcovr report
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

# Coverage: gcov must match the g++ that compiled the objects. Default g++ is
# unversioned, so prefer gcov-<major> when present (handles hosts where plain
# gcov lags g++), falling back to gcov.
COVERAGE_DIR := build-coverage
COVERAGE_REPORT := $(COVERAGE_DIR)/report
GCC_MAJOR := $(shell g++ -dumpversion 2>/dev/null | cut -d. -f1)
GCOV ?= $(shell command -v gcov-$(GCC_MAJOR) >/dev/null 2>&1 && echo gcov-$(GCC_MAJOR) || echo gcov)
GCOVR ?= gcovr
GCOVR_FLAGS := --root . --filter 'src/' --filter 'include/pup/' \
	--exclude-throw-branches --exclude-unreachable-branches \
	--gcov-executable '$(GCOV)'

.PHONY: all build configure test coverage install compdb tidy tidy-fix format format-check check clean distclean bootstrap

all: build

# Configure: generate tup.config (putup skips if already up-to-date)
configure:
	$(PUTUP) configure -B $(BUILD_DIR) $(BUILD_OPTIONS)

# Build: configure first, then build
build: configure
	$(PUTUP) -B $(BUILD_DIR) $(BUILD_OPTIONS)

test: build
	test/run-tests.sh ./$(BUILD_DIR)/test/unit/putup_test

# Coverage: build a gcov-instrumented variant, run the full test suite (with
# PUP pointing at the instrumented binary so E2E subprocess runs count too),
# then aggregate with gcovr into a summary + HTML + Cobertura report.
coverage:
	@command -v $(GCOVR) >/dev/null 2>&1 || { echo "gcovr not found. Install it with: pipx install gcovr  (or: pip install --user gcovr)"; exit 1; }
	CONFIG=coverage $(PUTUP) configure -B $(COVERAGE_DIR) $(BUILD_OPTIONS)
	CONFIG=coverage $(PUTUP) -B $(COVERAGE_DIR) $(BUILD_OPTIONS)
	find $(COVERAGE_DIR) -name '*.gcda' -delete 2>/dev/null || true
	PUP="$(CURDIR)/$(COVERAGE_DIR)/putup" test/run-tests.sh ./$(COVERAGE_DIR)/test/unit/putup_test
	@mkdir -p $(COVERAGE_REPORT)
	$(GCOVR) $(GCOVR_FLAGS) --print-summary \
		--html-details $(COVERAGE_REPORT)/index.html \
		--cobertura $(COVERAGE_REPORT)/coverage.xml \
		--json-summary $(COVERAGE_REPORT)/summary.json \
		$(COVERAGE_DIR)
	@echo "Coverage report written to $(COVERAGE_REPORT)/index.html"

install: build
	@mkdir -p $(PREFIX)/bin
	install -m 755 $(BUILD_DIR)/putup $(PREFIX)/bin/putup
	ln -sf putup $(PREFIX)/bin/pup
	@echo "Installed putup to $(PREFIX)/bin/putup (with pup symlink)"

# -D LTO: the bootstrap is a one-shot full build, so it keeps the LTO that the
# development default drops for incremental-rebuild speed (see 6ce999f10).
bootstrap: build
	@echo "Regenerating bootstrap scripts..."
	@./$(BUILD_DIR)/putup show script -B $(BUILD_DIR) -D LTO > bootstrap-linux.sh
	@CONFIG=macosx ./$(BUILD_DIR)/putup configure -B $(BUILD_DIR) > /dev/null
	@./$(BUILD_DIR)/putup show script -B $(BUILD_DIR) -D LTO > bootstrap-macos.sh
	@./$(BUILD_DIR)/putup configure -B $(BUILD_DIR) > /dev/null
	@chmod +x bootstrap-linux.sh bootstrap-macos.sh
	@echo "Regenerated bootstrap-linux.sh and bootstrap-macos.sh (commit them with Tupfile changes)"

compdb: configure
	@echo "Generating compile_commands.json..."
	@$(PUTUP) show compdb -B $(BUILD_DIR) > $(COMPDB)

RUN_CLANG_TIDY ?= run-clang-tidy

tidy: compdb
	@echo "Running clang-tidy..."
	@$(RUN_CLANG_TIDY) -p . $(TIDY_FLAGS) '$(CURDIR)/(src|test)/'

tidy-fix: compdb
	@echo "Running clang-tidy with fixes..."
	@$(RUN_CLANG_TIDY) -p . -fix $(TIDY_FLAGS) '$(CURDIR)/(src|test)/'

format:
	@echo "Formatting sources..."
	@find src include -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

format-check:
	@echo "Checking format..."
	@find src include -name '*.cpp' -o -name '*.hpp' | xargs clang-format --dry-run --Werror

iwyu: compdb
	@echo "Checking includes..."
	@fail=0; \
	for f in $$(find src include/pup -name '*.cpp' -o -name '*.hpp' | grep -v third_party | sort); do \
		changes=$$(clang-include-cleaner-18 -p . --print=changes "$$f" 2>/dev/null | grep "^- "); \
		if [ -n "$$changes" ]; then \
			echo "$$f:"; echo "$$changes"; fail=1; \
		fi; \
	done; \
	if [ "$$fail" -eq 1 ]; then echo "Dead includes found."; exit 1; fi; \
	echo "No dead includes found."

check: format-check tidy test
	@echo "All checks passed."

clean:
	$(PUTUP) clean -B $(BUILD_DIR)

distclean:
	$(PUTUP) distclean -B $(BUILD_DIR)
	rm -rf $(COVERAGE_DIR)
