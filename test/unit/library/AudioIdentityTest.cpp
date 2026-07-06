// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/library/AudioIdentity.h>
#include <ao/utility/Fnv1a.h>

#include <catch2/catch_test_macros.hpp>

#include <stop_token>
#include <vector>

namespace ao::library::test
{
  TEST_CASE("readAudioIdentity returns payload identity for supported audio", "[library][unit][audio-identity]")
  {
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto progressEvents = std::vector<double>{};

    auto result = readAudioIdentity(sourceFile, [&](double fraction) { progressEvents.push_back(fraction); });

    REQUIRE(result);
    REQUIRE(result->has_value());
    CHECK((*result)->payloadLength > 0);
    CHECK((*result)->signature != utility::Hash128{});
    REQUIRE(progressEvents.size() >= 2);
    CHECK(progressEvents.front() == 0.0);
    CHECK(progressEvents.back() == 1.0);
  }

  TEST_CASE("readAudioIdentity returns empty optional when cancelled", "[library][unit][audio-identity]")
  {
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto stopSource = std::stop_source{};

    auto result = readAudioIdentity(
      sourceFile,
      [&stopSource](double fraction)
      {
        if (fraction == 0.0)
        {
          stopSource.request_stop();
        }
      },
      stopSource.get_token());

    REQUIRE(result);
    CHECK_FALSE(result->has_value());
  }
} // namespace ao::library::test
