// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/AudioIdentity.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <stop_token>
#include <vector>

namespace ao::library::test
{
  namespace
  {
    constexpr std::size_t kHashChunkSize = 4ULL * 1024ULL * 1024ULL;

    std::vector<std::byte> makePayload(std::size_t size)
    {
      auto payload = std::vector<std::byte>(size);

      for (std::size_t index = 0; index < payload.size(); ++index)
      {
        payload[index] = static_cast<std::byte>(index % 251U);
      }

      return payload;
    }
  } // namespace

  TEST_CASE("readAudioIdentity - returns payload identity and progress", "[library][unit][audio-identity]")
  {
    auto const payload = makePayload(32);
    auto progressEvents = std::vector<double>{};

    auto optResult = readAudioIdentity(payload, [&](double fraction) { progressEvents.push_back(fraction); });

    REQUIRE(optResult);
    CHECK(optResult->payloadLength == payload.size());
    CHECK(optResult->signature != utility::Hash128{});
    REQUIRE(progressEvents.size() == 2);
    CHECK(progressEvents.front() == 0.0);
    CHECK(progressEvents.back() == 1.0);
  }

  TEST_CASE("readAudioIdentity - streaming matches one-shot XXH3 across chunk boundaries",
            "[library][unit][audio-identity]")
  {
    auto const payload = makePayload(kHashChunkSize + 17U);

    auto optResult = readAudioIdentity(payload);

    REQUIRE(optResult);
    CHECK(optResult->signature == utility::xxh3Hash128(payload));
    CHECK(optResult->payloadLength == payload.size());
  }

  TEST_CASE("readAudioIdentity - cancellation after one chunk returns no identity", "[library][unit][audio-identity]")
  {
    auto const payload = makePayload(kHashChunkSize + 17U);
    auto stopSource = std::stop_source{};
    auto progressEvents = std::vector<double>{};

    auto optResult = readAudioIdentity(
      payload,
      [&stopSource, &progressEvents](double fraction)
      {
        progressEvents.push_back(fraction);

        if (fraction > 0.0)
        {
          stopSource.request_stop();
        }
      },
      stopSource.get_token());

    CHECK_FALSE(optResult);
    REQUIRE(progressEvents.size() == 2);
    CHECK(progressEvents.front() == 0.0);
    CHECK(progressEvents.back() > 0.0);
    CHECK(progressEvents.back() < 1.0);
  }
} // namespace ao::library::test
