// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/compat/Enumerate.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <ranges>
#include <string>
#include <vector>

namespace ao::compat::test
{
  namespace
  {
    // Targets the portable adaptor directly: on Linux and Windows
    // ao::compat::views::enumerate resolves to std::views::enumerate, so
    // testing only the alias would leave this code covered on macOS alone.
    constexpr auto kPortableEnumerate = detail::EnumerateAdaptor{};
  } // namespace

  TEST_CASE("Enumerate - pairs each element with its index", "[core][unit][enumerate]")
  {
    auto const values = std::vector<std::string>{"a", "b", "c"};

    auto seenIndices = std::vector<std::int64_t>{};
    auto seenValues = std::vector<std::string>{};

    for (auto const& [index, value] : kPortableEnumerate(values))
    {
      seenIndices.push_back(static_cast<std::int64_t>(index));
      seenValues.push_back(value);
    }

    CHECK(seenIndices == std::vector<std::int64_t>{0, 1, 2});
    CHECK(seenValues == values);
  }

  TEST_CASE("Enumerate - supports pipe syntax", "[core][unit][enumerate]")
  {
    auto const values = std::vector<std::int32_t>{10, 20};

    std::int64_t total = 0;

    for (auto const& [index, value] : values | kPortableEnumerate)
    {
      total += static_cast<std::int64_t>(index) * value;
    }

    CHECK(total == 20);
  }

  TEST_CASE("Enumerate - yields references into the source range", "[core][unit][enumerate]")
  {
    auto values = std::vector<std::int32_t>{1, 2, 3};

    for (auto const& [index, value] : kPortableEnumerate(values))
    {
      value += static_cast<std::int32_t>(index);
    }

    CHECK(values == std::vector<std::int32_t>{1, 3, 5});
  }

  TEST_CASE("Enumerate - an empty range yields nothing", "[core][unit][enumerate]")
  {
    auto const values = std::vector<std::int32_t>{};

    std::int32_t count = 0;

    for ([[maybe_unused]] auto const& entry : kPortableEnumerate(values))
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("Enumerate - indexes the filtered sequence, not the source", "[core][unit][enumerate]")
  {
    auto const values = std::vector<std::int32_t>{1, 2, 3, 4, 5, 6};
    auto even = values | std::views::filter([](std::int32_t value) { return value % 2 == 0; });

    auto seen = std::vector<std::int64_t>{};

    for (auto const& [index, value] : kPortableEnumerate(even))
    {
      seen.push_back((static_cast<std::int64_t>(index) * 100) + value);
    }

    // Indices stay 0,1,2 across a non-common range rather than tracking source positions.
    CHECK(seen == std::vector<std::int64_t>{2, 104, 206});
  }

  TEST_CASE("Enumerate - a const view is still a range", "[core][unit][enumerate]")
  {
    auto const values = std::vector<std::int32_t>{4, 5, 6};

    // Holding the view in a const object is what forces begin() const. The
    // range-for cases above never do, because the adaptor yields a prvalue.
    auto const view = kPortableEnumerate(values);
    STATIC_REQUIRE(std::ranges::range<decltype(view)>);

    auto seen = std::vector<std::int64_t>{};

    for (auto const& [index, value] : view)
    {
      seen.push_back((static_cast<std::int64_t>(index) * 10) + value);
    }

    CHECK(seen == std::vector<std::int64_t>{4, 15, 26});
  }

  TEST_CASE("Enumerate - reports the size of a sized base range", "[core][unit][enumerate]")
  {
    auto values = std::vector<std::int32_t>{1, 2, 3, 4};

    auto view = kPortableEnumerate(values);
    STATIC_REQUIRE(std::ranges::sized_range<decltype(view)>);
    CHECK(std::ranges::size(view) == 4U);
    CHECK_FALSE(view.empty());

    auto const constView = kPortableEnumerate(values);
    CHECK(std::ranges::size(constView) == 4U);
  }

  TEST_CASE("Enumerate - an unsized base range leaves the view unsized", "[core][unit][enumerate]")
  {
    auto const values = std::vector<std::int32_t>{1, 2, 3, 4};
    // Not const: filter_view has no begin() const, so a const one is not a
    // range at all and could not be enumerated by either implementation.
    auto odd = values | std::views::filter([](auto value) { return value % 2 != 0; });

    // filter_view has no size, so neither does the enumeration over it. The
    // point is that the adaptor forwards the property rather than inventing it.
    STATIC_REQUIRE_FALSE(std::ranges::sized_range<decltype(kPortableEnumerate(odd))>);
  }

  TEST_CASE("Enumerate - the project alias behaves the same", "[core][unit][enumerate]")
  {
    auto const values = std::vector<std::int32_t>{7, 8};

    auto seen = std::vector<std::int64_t>{};

    for (auto const& [index, value] : views::enumerate(values))
    {
      seen.push_back((static_cast<std::int64_t>(index) * 10) + value);
    }

    CHECK(seen == std::vector<std::int64_t>{7, 18});
  }
} // namespace ao::compat::test
