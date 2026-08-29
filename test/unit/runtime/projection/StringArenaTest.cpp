// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/projection/StringArena.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::detail::test
{
  TEST_CASE("StringArena - interns and deduplicates projection text", "[runtime][unit][projection][arena]")
  {
    auto arena = StringArena{};

    SECTION("empty input is not stored")
    {
      auto const view = arena.intern("");
      CHECK(view.empty());
      CHECK(arena.empty());
      CHECK(arena.allocatedBytes() == 0);
    }

    SECTION("distinct strings each get stored")
    {
      auto const a = arena.intern("alpha");
      auto const b = arena.intern("beta");

      CHECK(a == "alpha");
      CHECK(b == "beta");
      CHECK(arena.size() == 2);
      CHECK(arena.allocatedBytes() > 0);
      CHECK(a.data() != b.data());
    }

    SECTION("equal content returns the same view")
    {
      auto const first = arena.intern("gamma");

      // Use a distinct buffer with equal content to prove deduplication is by value, not pointer.
      auto const other = std::string{"gamma"};
      auto const second = arena.intern(other);

      CHECK(first.data() == second.data());
      CHECK(first.size() == second.size());
      CHECK(arena.size() == 1);
    }

    SECTION("interned copy is independent of the source buffer")
    {
      auto source = std::string{"mutable"};
      auto const view = arena.intern(source);

      source[0] = 'X';
      source.clear();

      CHECK(view == "mutable");
    }
  }

  TEST_CASE("StringArena - views stay valid across arena and index growth", "[runtime][unit][projection][arena]")
  {
    auto arena = StringArena{};

    // Retain early views, then force both index rehashing and monotonic-resource growth.
    // Neither operation may move the bytes those views reference.
    auto views = std::vector<std::string_view>{};
    views.reserve(1000);

    for (std::int32_t index = 0; index < 1000; ++index)
    {
      views.push_back(arena.intern(std::format("entry-{:04d}", index)));
    }

    CHECK(arena.size() == 1000);

    for (std::int32_t index = 0; index < 1000; ++index)
    {
      CHECK(views[static_cast<std::size_t>(index)] == std::format("entry-{:04d}", index));
    }
  }

  TEST_CASE("StringArena - clear resets projection storage", "[runtime][unit][projection][arena]")
  {
    auto arena = StringArena{};

    arena.intern("one");
    arena.intern("two");
    CHECK(arena.size() == 2);
    CHECK(arena.allocatedBytes() > 0);

    arena.clear();
    CHECK(arena.empty());
    CHECK(arena.allocatedBytes() == 0);

    // Clearing resets ownership without making the arena unusable.
    auto const view = arena.intern("three");
    CHECK(view == "three");
    CHECK(arena.size() == 1);
  }
} // namespace ao::rt::detail::test
