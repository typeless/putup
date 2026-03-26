// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/core/global_pool.hpp"
#include "pup/core/string_pool.hpp"
#include "pup/parser/var_tracking.hpp"

#include <algorithm>

using namespace pup::parser;

namespace {

auto id(std::string_view s) -> pup::StringId { return pup::global_pool().intern(s); }
auto sv(pup::StringId sid) -> std::string_view { return pup::global_pool().get(sid); }

auto find_history(pup::Vec<VarHistory> const& v, std::string_view name) -> VarHistory const*
{
    for (auto const& h : v) {
        if (sv(h.name) == name) {
            return &h;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("op_to_string converts operators correctly", "[var_tracking]")
{
    CHECK(op_to_string(Assignment::Op::Set) == "=");
    CHECK(op_to_string(Assignment::Op::Append) == "+=");
    CHECK(op_to_string(Assignment::Op::Define) == ":=");
    CHECK(op_to_string(Assignment::Op::SoftSet) == "?=");
    CHECK(op_to_string(Assignment::Op::WeakSet) == "?\?=");
}

TEST_CASE("filter_by_name filters assignments", "[var_tracking]")
{
    auto log = AssignmentLog {
        VarAssignment { .name = id("CC"), .filename = id("Tuprules.tup"), .line = 1, .op = Assignment::Op::Set, .value_before = id(""), .value_after = id("gcc") },
        VarAssignment { .name = id("CFLAGS"), .filename = id("Tuprules.tup"), .line = 2, .op = Assignment::Op::Set, .value_before = id(""), .value_after = id("-Wall") },
        VarAssignment { .name = id("CC"), .filename = id("Tupfile"), .line = 3, .op = Assignment::Op::Set, .value_before = id("gcc"), .value_after = id("clang") },
    };

    SECTION("filters by exact name")
    {
        auto filtered = filter_by_name(log, "CC");
        REQUIRE(filtered.size() == 2);
        CHECK(sv(filtered[0].value_after) == "gcc");
        CHECK(sv(filtered[1].value_after) == "clang");
    }

    SECTION("returns empty for non-matching name")
    {
        auto filtered = filter_by_name(log, "LDFLAGS");
        CHECK(filtered.empty());
    }
}

TEST_CASE("group_by_name returns empty for empty log", "[var_tracking]")
{
    auto histories = group_by_name(AssignmentLog {});
    CHECK(histories.empty());
}

TEST_CASE("group_by_name groups assignments", "[var_tracking]")
{
    auto log = AssignmentLog {
        VarAssignment { .name = id("CC"), .filename = id("Tuprules.tup"), .line = 1, .op = Assignment::Op::Set, .value_before = id(""), .value_after = id("gcc"), .is_effective = true },
        VarAssignment { .name = id("CFLAGS"), .filename = id("Tuprules.tup"), .line = 2, .op = Assignment::Op::Set, .value_before = id(""), .value_after = id("-Wall"), .is_effective = true },
        VarAssignment { .name = id("CFLAGS"), .filename = id("Tuprules.tup"), .line = 3, .op = Assignment::Op::Append, .value_before = id("-Wall"), .value_after = id("-Wall -O2"), .is_effective = true },
        VarAssignment { .name = id("CC"), .filename = id("Tupfile"), .line = 4, .op = Assignment::Op::SoftSet, .value_before = id("gcc"), .value_after = id("gcc"), .is_effective = false },
    };

    auto histories = group_by_name(log);

    SECTION("creates history for each variable")
    {
        REQUIRE(histories.size() == 2);
        CHECK(find_history(histories, "CC") != nullptr);
        CHECK(find_history(histories, "CFLAGS") != nullptr);
    }

    SECTION("collects all assignments in order")
    {
        auto const* cc_history = find_history(histories, "CC");
        REQUIRE(cc_history != nullptr);
        REQUIRE(cc_history->assignments.size() == 2);
        CHECK(sv(cc_history->assignments[0]->filename) == "Tuprules.tup");
        CHECK(sv(cc_history->assignments[1]->filename) == "Tupfile");
    }

    SECTION("tracks final value from effective assignments")
    {
        CHECK(sv(find_history(histories, "CC")->final_value) == "gcc");
        CHECK(sv(find_history(histories, "CFLAGS")->final_value) == "-Wall -O2");
    }
}
