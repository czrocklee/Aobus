// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace ao::utility::test
{
  TEST_CASE("TransparentStringHash - equivalent string representations hash equally")
  {
    auto const hash = TransparentStringHash{};
    auto const owned = std::string{"alpha"};
    auto const backing = std::string{"alpha-suffix"};
    auto const slice = std::string_view{backing.data(), owned.size()};

    CHECK(hash(owned) == hash(slice));
    CHECK(hash("alpha") == hash(slice));
  }

  TEST_CASE("TransparentStringHash - heterogeneous map lookup preserves bounded keys")
  {
    auto values = boost::unordered_flat_map<std::string, std::string_view, TransparentStringHash, std::equal_to<>>{};
    values.emplace(std::string{"a\0b", 3}, "full key");
    values.emplace("a", "prefix");

    auto const embeddedIt = values.find(std::string_view{"a\0b", 3});
    REQUIRE(embeddedIt != values.end());
    CHECK(embeddedIt->second == "full key");

    auto const literalIt = values.find("a");
    REQUIRE(literalIt != values.end());
    CHECK(literalIt->second == "prefix");
  }
} // namespace ao::utility::test
