// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/library/AudioIdentity.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Xxh3.h>

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

  TEST_CASE("readAudioIdentity matches one-shot XXH3-128 of the audio payload", "[library][unit][audio-identity]")
  {
    auto const sourceFile = audio::test::requireAudioFixture("basic_metadata.flac");
    auto tagFileResult = tag::TagFile::open(sourceFile);
    REQUIRE(tagFileResult);
    auto const payloadResult = (*tagFileResult)->audioPayload();
    REQUIRE(payloadResult);

    auto result = readAudioIdentity(**tagFileResult);

    REQUIRE(result);
    REQUIRE(result->has_value());
    // The production loop hashes in bounded chunks; XXH3 streaming is
    // chunk-boundary invariant, so it must equal the one-shot signature.
    CHECK((*result)->signature == utility::xxh3Hash128(payloadResult->bytes));
    CHECK((*result)->payloadLength == payloadResult->bytes.size());
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
