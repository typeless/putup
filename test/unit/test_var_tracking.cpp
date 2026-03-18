// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"
#include "pup/parser/var_tracking.hpp"

#include <algorithm>

using namespace pup::parser;

namespace {

auto find_history(std::vector<VarHistory> const& v, std::string_view name) -> VarHistory const*
{
    auto it = std::lower_bound(v.begin(), v.end(), name,
        [](auto const& h, auto const& n) { return h.name < n; });
    return (it != v.end() && it->name == name) ? &*it : nullptr;
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
        VarAssignment { .name = "CC", .filename = "Tuprules.tup", .line = 1, .op = Assignment::Op::Set, .value_before = "", .value_after = "gcc" },
        VarAssignment { .name = "CFLAGS", .filename = "Tuprules.tup", .line = 2, .op = Assignment::Op::Set, .value_before = "", .value_after = "-Wall" },
        VarAssignment { .name = "CC", .filename = "Tupfile", .line = 3, .op = Assignment::Op::Set, .value_before = "gcc", .value_after = "clang" },
    };

    SECTION("filters by exact name")
    {
        auto filtered = filter_by_name(log, "CC");
        REQUIRE(filtered.size() == 2);
        CHECK(filtered[0].value_after == "gcc");
        CHECK(filtered[1].value_after == "clang");
    }

    SECTION("returns empty for non-matching name")
    {
        auto filtered = filter_by_name(log, "LDFLAGS");
        CHECK(filtered.empty());
    }
}

TEST_CASE("group_by_name groups assignments", "[var_tracking]")
{
    auto log = AssignmentLog {
        VarAssignment { .name = "CC", .filename = "Tuprules.tup", .line = 1, .op = Assignment::Op::Set, .value_before = "", .value_after = "gcc", .is_effective = true },
        VarAssignment { .name = "CFLAGS", .filename = "Tuprules.tup", .line = 2, .op = Assignment::Op::Set, .value_before = "", .value_after = "-Wall", .is_effective = true },
        VarAssignment { .name = "CFLAGS", .filename = "Tuprules.tup", .line = 3, .op = Assignment::Op::Append, .value_before = "-Wall", .value_after = "-Wall -O2", .is_effective = true },
        VarAssignment { .name = "CC", .filename = "Tupfile", .line = 4, .op = Assignment::Op::SoftSet, .value_before = "gcc", .value_after = "gcc", .is_effective = false },
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
        CHECK(cc_history->assignments[0]->filename == "Tuprules.tup");
        CHECK(cc_history->assignments[1]->filename == "Tupfile");
    }

    SECTION("tracks final value from effective assignments")
    {
        CHECK(find_history(histories, "CC")->final_value == "gcc");
        CHECK(find_history(histories, "CFLAGS")->final_value == "-Wall -O2");
    }
}
