// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#pragma once

// Use std::expected if available (C++23 with proper library support),
// otherwise use expected-lite from Martin Moene

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#    include <expected>
namespace pup {
using std::expected;
using std::unexpected;
} // namespace pup
#else
// Force expected-lite to use nonstd implementation
#    define nsel_CONFIG_SELECT_EXPECTED nsel_EXPECTED_NONSTD
// Use expected-lite (https://github.com/martinmoene/expected-lite)
#    include "../../../third_party/expected.hpp"
namespace pup {
using nonstd::expected;
using nonstd::unexpected;
} // namespace pup
#endif
