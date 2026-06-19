// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/StringArena.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ao::utility::test
{
  TEST_CASE("StringArena interns and deduplicates by content", "[utility][unit][arena]")
  {
    auto arena = StringArena{};

    SECTION("empty input is not stored")
    {
      auto const view = arena.intern("");
      CHECK(view.empty());
      CHECK(arena.empty());
      CHECK(arena.empty());
    }

    SECTION("distinct strings each get stored")
    {
      auto const a = arena.intern("alpha");
      auto const b = arena.intern("beta");

      CHECK(a == "alpha");
      CHECK(b == "beta");
      CHECK(arena.size() == 2);
      CHECK(a.data() != b.data());
    }

    SECTION("equal content returns the same view")
    {
      auto const first = arena.intern("gamma");

      // Feed a distinct buffer with equal content to prove dedup is by value, not pointer.
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

  TEST_CASE("StringArena views stay valid across many insertions", "[utility][unit][arena]")
  {
    auto arena = StringArena{};

    // Retain early views, then insert enough unique strings to force the flat index to
    // rehash and the monotonic resource to grow into new blocks. Both must leave the early
    // views pointing at unchanged bytes.
    auto views = std::vector<std::string_view>{};
    views.reserve(1000);

    for (std::int32_t idx = 0; idx < 1000; ++idx)
    {
      views.push_back(arena.intern(std::format("entry-{:04d}", idx)));
    }

    CHECK(arena.size() == 1000);

    for (std::int32_t idx = 0; idx < 1000; ++idx)
    {
      CHECK(views[static_cast<std::size_t>(idx)] == std::format("entry-{:04d}", idx));
    }
  }

  TEST_CASE("StringArena clear resets storage", "[utility][unit][arena]")
  {
    auto arena = StringArena{};

    arena.intern("one");
    arena.intern("two");
    REQUIRE(arena.size() == 2);

    arena.clear();
    CHECK(arena.empty());
    CHECK(arena.empty());

    // Re-interning after clear works and counts fresh.
    auto const view = arena.intern("three");
    CHECK(view == "three");
    CHECK(arena.size() == 1);
  }
} // namespace ao::utility::test
