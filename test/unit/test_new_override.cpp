// SPDX-License-Identifier: MIT
// Copyright (c) 2024 Putup authors

#include "catch_amalgamated.hpp"

#ifdef _WIN32

#include <cstdint>
#include <new>

extern "C" {
extern unsigned pup_ov_new;
extern unsigned pup_ov_new_nothrow;
extern unsigned pup_ov_new_aligned;
extern unsigned pup_ov_delete;
extern unsigned pup_ov_delete_aligned;
}

namespace {

struct alignas(64) OverAligned {
    unsigned char data[64];
};

// Escaping pointers through a volatile sink prevents the compiler from
// eliding the new/delete pairs (replaceable allocation calls may legally
// be optimized away even though the override has side effects).
void* volatile sink;

} // namespace

TEST_CASE("global operator new override is total on the MSVC CRT", "[new_override]")
{
    SECTION("plain and array new/delete route through the override")
    {
        auto const new_before = pup_ov_new;
        auto const delete_before = pup_ov_delete;
        auto* p = new int { 7 };
        sink = p;
        REQUIRE(*p == 7);
        delete p;
        auto* arr = new int[16] {};
        sink = arr;
        arr[15] = 3;
        REQUIRE(arr[15] == 3);
        delete[] arr;
        REQUIRE(pup_ov_new >= new_before + 2);
        REQUIRE(pup_ov_delete >= delete_before + 2);
    }

    SECTION("over-aligned new/delete route through the override, not the CRT")
    {
        auto const new_before = pup_ov_new_aligned;
        auto const delete_before = pup_ov_delete_aligned;
        auto* p = new OverAligned {};
        sink = p;
        REQUIRE(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
        delete p;
        auto* arr = new OverAligned[3] {};
        sink = arr;
        REQUIRE(reinterpret_cast<std::uintptr_t>(arr) % 64 == 0);
        delete[] arr;
        REQUIRE(pup_ov_new_aligned == new_before + 2);
        REQUIRE(pup_ov_delete_aligned == delete_before + 2);
    }

    SECTION("nothrow new routes through the override")
    {
        auto const before = pup_ov_new_nothrow;
        auto* p = new (std::nothrow) int { 5 };
        sink = p;
        REQUIRE(p != nullptr);
        delete p;
        REQUIRE(pup_ov_new_nothrow >= before + 1);
    }
}

#endif // _WIN32
