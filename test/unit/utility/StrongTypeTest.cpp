// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/utility/StrongType.h>

#include <ao/utility/StrongTypeFormatter.h>

#include <catch2/catch_test_macros.hpp>

#include <compare>
#include <concepts>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ao::utility::test
{
  namespace
  {
    using StringId = StrongType<std::string, struct StringIdTag>;
    using IntId = StrongType<std::int32_t, struct IntIdTag>;

    template<typename T>
    concept Clearable = requires(T& value) { value.clear(); };

    template<typename T>
    concept PreIncrementable = requires(T& value) { ++value; };

    template<typename T>
    concept PostIncrementable = requires(T& value) { value++; };

    template<typename T>
    concept PreDecrementable = requires(T& value) { --value; };

    template<typename T>
    concept PostDecrementable = requires(T& value) { value--; };

    static_assert(std::same_as<decltype(std::declval<StringId&>().raw()), std::string const&>);
    static_assert(!Clearable<StringId>);
    static_assert(!PreIncrementable<IntId>);
    static_assert(!PostIncrementable<IntId>);
    static_assert(!PreDecrementable<IntId>);
    static_assert(!PostDecrementable<IntId>);
  } // namespace

  TEST_CASE("StrongType - string-backed wrappers expose value semantics", "[utility][unit][strong-type]")
  {
    auto id1 = StringId{"test"};
    auto id2 = StringId{"test"};
    auto id3 = StringId{"other"};
    auto idEmpty = StringId{};

    CHECK(id1 == id2);
    CHECK(id1 != id3);
    CHECK(idEmpty.empty());
    CHECK(!id1.empty());

    auto const sv = std::string_view{id2};
    CHECK(sv == "test");

    CHECK(id2 == "test");
    CHECK(id2 != "other");
    CHECK((id2 <=> "other") == std::strong_ordering::greater);

    // hash
    auto set = std::unordered_set<StringId>{};
    set.insert(id2);
    CHECK(set.contains(id2));

    // formatter
    auto const formatted = std::format("{}", id2);
    CHECK(formatted == "test");
  }

  TEST_CASE("StrongType - integer-backed wrappers expose numeric semantics", "[utility][unit][strong-type]")
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
    CHECK((id1 <=> 10) == std::strong_ordering::greater);

    // hash
    auto set = std::unordered_set<IntId>{};
    set.insert(id1);
    CHECK(set.contains(id1));

    // formatter
    auto const formatted = std::format("{}", id1);
    CHECK(formatted == "42");
  }
} // namespace ao::utility::test
