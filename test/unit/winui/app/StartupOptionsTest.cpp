// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/app/StartupOptions.h>

#include <ao/utility/Path.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

namespace ao::winui::test
{
  TEST_CASE("StartupOptions - no private arguments selects persisted startup", "[winui][unit][app]")
  {
    auto const arguments = std::array<std::string_view, 0>{};
    auto result = parseStartupOptions(arguments);

    REQUIRE(result);
    CHECK_FALSE(result->optLibraryRoot);
  }

  TEST_CASE("StartupOptions - preserves one Unicode and space-containing library root", "[winui][unit][app]")
  {
    auto const expected = std::string{"C:/My Music/\xE6\xB5\xB7\xE5\xA4\x96"};
    auto const arguments = std::array<std::string_view, 2>{kLibraryRootOption, expected};

    auto result = parseStartupOptions(arguments);

    REQUIRE(result);
    REQUIRE(result->optLibraryRoot);
    CHECK(*result->optLibraryRoot == utility::pathFromUtf8(expected));
  }

  TEST_CASE("StartupOptions - rejects malformed private arguments", "[winui][unit][app]")
  {
    SECTION("missing value")
    {
      auto const arguments = std::array<std::string_view, 1>{kLibraryRootOption};
      auto result = parseStartupOptions(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("requires a path"));
    }

    SECTION("duplicate option")
    {
      auto const arguments =
        std::array<std::string_view, 4>{kLibraryRootOption, "C:/Music", kLibraryRootOption, "D:/Music"};
      auto result = parseStartupOptions(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("once"));
    }

    SECTION("unknown option")
    {
      auto const arguments = std::array<std::string_view, 1>{"--other"};
      auto result = parseStartupOptions(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("Unknown"));
    }
  }
} // namespace ao::winui::test
