// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/StrongType.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace ao::utility::test
{
  using namespace ao::utility;

  namespace
  {
    using StringId = StrongType<std::string, struct StringIdTag>;
    using IntId = StrongType<std::int32_t, struct IntIdTag>;
  }

  TEST_CASE("StrongType - Typedef wrappers", "[utility][unit][strong_type]")
  {
    SECTION("String-backed strong type")
    {
      auto id1 = StringId{"test"};
      auto id2 = StringId{"test"};
      auto id3 = StringId{"other"};
      auto idEmpty = StringId{};

      CHECK(id1 == id2);
      CHECK(id1 != id3);
      CHECK(idEmpty.empty());
      CHECK(!id1.empty());

      id1.clear();
      CHECK(id1.empty());

      auto const sv = std::string_view{id2};
      CHECK(sv == "test");

      CHECK(id2 == "test");
      CHECK(id2 != "other");
      CHECK((id2 <=> "other") > 0);

      // hash
      auto set = std::unordered_set<StringId>{};
      set.insert(id2);
      CHECK(set.contains(id2));

      // formatter
      auto const formatted = std::format("{}", id2);
      CHECK(formatted == "test");
    }

    SECTION("Integer-backed strong type")
    {
      auto id1 = IntId{42};
      auto id2 = IntId{42};
      auto id3 = IntId{10};

      CHECK(id1 == id2);
      CHECK(id1 != id3);

      auto const val = static_cast<std::int32_t>(id1);
      CHECK(val == 42);

      CHECK(id1 == 42);
      CHECK(id1 != 10);
      CHECK((id1 <=> 10) > 0);

      // Increment / Decrement
      auto inc = id3;
      CHECK(inc++ == 10);
      CHECK(inc == 11);
      CHECK(++inc == 12);

      auto dec = id3;
      CHECK(dec-- == 10);
      CHECK(dec == 9);
      CHECK(--dec == 8);

      // Output stream
      auto oss = std::ostringstream{};
      oss << id1;
      CHECK(oss.str() == "42");

      // hash
      auto set = std::unordered_set<IntId>{};
      set.insert(id1);
      CHECK(set.contains(id1));

      // formatter
      auto const formatted = std::format("{}", id1);
      CHECK(formatted == "42");
    }
  }
} // namespace ao::utility::test