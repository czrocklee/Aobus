// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/LibrarySuccessorProtocol.h>

#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/utility/Path.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::desktop::test
{
  TEST_CASE("LibrarySuccessorProtocol - ordinary arguments remain unclaimed in process order",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const arguments = std::array<std::string_view, 3>{"--display=:7", "music.aobus", "--name=Aobus"};

    auto result = parseLibrarySuccessorProtocol(arguments);

    REQUIRE(result);
    CHECK_FALSE(result->optRequest);
    CHECK(result->remainingArguments == std::vector<std::string>{"--display=:7", "music.aobus", "--name=Aobus"});
  }

  TEST_CASE("LibrarySuccessorProtocol - valid paired request round-trips normalized root and scan intent",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const root = (fixture.path() / "music" / ".." / "library").lexically_normal();
    auto const rootText = utility::pathToUtf8(root);
    auto const arguments = std::array<std::string_view, 6>{
      "--display=:7", kLibrarySuccessorOption, kLibraryRootOption, rootText, kScanAfterOpenOption, "--name=Aobus"};

    auto parsedRes = parseLibrarySuccessorProtocol(arguments);

    REQUIRE(parsedRes);
    REQUIRE(parsedRes->optRequest);
    CHECK(parsedRes->optRequest->libraryRoot == root);
    CHECK(parsedRes->optRequest->scanAfterOpen);
    CHECK(parsedRes->remainingArguments == std::vector<std::string>{"--display=:7", "--name=Aobus"});

    auto encodedRes = librarySuccessorArguments(*parsedRes->optRequest);

    REQUIRE(encodedRes);
    auto roundTripViews = std::vector<std::string_view>{};

    for (auto const& argument : *encodedRes)
    {
      roundTripViews.emplace_back(argument);
    }

    auto roundTripRes = parseLibrarySuccessorProtocol(roundTripViews);
    REQUIRE(roundTripRes);
    CHECK(roundTripRes->optRequest == parsedRes->optRequest);
    CHECK(roundTripRes->remainingArguments.empty());
  }

  TEST_CASE("LibrarySuccessorProtocol - malformed private arguments fail before frontend startup",
            "[runtime][unit][desktop-lifecycle]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const rootText = utility::pathToUtf8(fixture.path());

    SECTION("root without marker")
    {
      auto const arguments = std::array<std::string_view, 2>{kLibraryRootOption, rootText};
      auto result = parseLibrarySuccessorProtocol(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("together"));
    }

    SECTION("marker without root")
    {
      auto const arguments = std::array<std::string_view, 1>{kLibrarySuccessorOption};
      auto result = parseLibrarySuccessorProtocol(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("together"));
    }

    SECTION("duplicate root")
    {
      auto const arguments = std::array<std::string_view, 5>{
        kLibrarySuccessorOption, kLibraryRootOption, rootText, kLibraryRootOption, rootText};
      auto result = parseLibrarySuccessorProtocol(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("once"));
    }

    SECTION("relative root")
    {
      auto const arguments =
        std::array<std::string_view, 3>{kLibrarySuccessorOption, kLibraryRootOption, "relative/music"};
      auto result = parseLibrarySuccessorProtocol(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("absolute"));
    }

    SECTION("scan without successor")
    {
      auto const arguments = std::array<std::string_view, 1>{kScanAfterOpenOption};
      auto result = parseLibrarySuccessorProtocol(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("requires a successor"));
    }
  }
} // namespace ao::desktop::test
