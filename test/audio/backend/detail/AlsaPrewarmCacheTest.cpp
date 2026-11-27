// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/backend/detail/AlsaPrewarmCache.h"

#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::audio::backend::detail::test
{
  namespace
  {
    // The cache is a pure in-memory advisory record: these tests construct it
    // directly, so no ALSA device is opened and no native positions are
    // observed. A miss only withholds an optimistic client format; it never
    // asserts anything about a physical endpoint.
    //
    // Entries key on the complete signal. Both layouts below share the sample
    // rate, channel count, and precision, and differ only in their WAVE-style
    // speaker masks: front pair plus back pair, vs front pair, front center,
    // and back center.
    constexpr std::uint32_t kQuadBackPairMask = 0x33;
    constexpr std::uint32_t kQuadCenterBackMask = 0x107;

    constexpr SignalFormat quadSource(std::uint32_t speakerMask)
    {
      return SignalFormat{
        .sampleRate = 48000,
        .channels = 4,
        .precisionBits = 24,
        .sampleKind = SampleKind::Integer,
        .speakerMask = speakerMask,
      };
    }
  } // namespace

  TEST_CASE("AlsaPrewarmCache - speaker-mask-distinct signals keep their client formats separately")
  {
    auto cache = AlsaPrewarmCache{};

    auto const backPairClient = PcmFormat{
      .sampleRate = 48000,
      .channels = 4,
      .encoding = SampleEncoding::Signed24In32Le,
      .speakerMask = kQuadBackPairMask,
    };
    auto const centerBackClient = PcmFormat{
      .sampleRate = 48000,
      .channels = 4,
      .encoding = SampleEncoding::Signed24PackedLe,
      .speakerMask = kQuadCenterBackMask,
    };

    cache.store(quadSource(kQuadBackPairMask), backPairClient);
    cache.store(quadSource(kQuadCenterBackMask), centerBackClient);

    auto const optBackPair = cache.find(quadSource(kQuadBackPairMask));
    REQUIRE(optBackPair);
    CHECK(*optBackPair == backPairClient);

    auto const optCenterBack = cache.find(quadSource(kQuadCenterBackMask));
    REQUIRE(optCenterBack);
    CHECK(*optCenterBack == centerBackClient);
  }

  TEST_CASE("AlsaPrewarmCache - an unrecorded zero mask does not match identified layouts")
  {
    auto cache = AlsaPrewarmCache{};
    CHECK_FALSE(cache.find(quadSource(0)).has_value());

    cache.store(quadSource(kQuadBackPairMask),
                PcmFormat{
                  .sampleRate = 48000,
                  .channels = 4,
                  .encoding = SampleEncoding::Signed24In32Le,
                  .speakerMask = kQuadBackPairMask,
                });
    cache.store(quadSource(kQuadCenterBackMask),
                PcmFormat{
                  .sampleRate = 48000,
                  .channels = 4,
                  .encoding = SampleEncoding::Signed24In32Le,
                  .speakerMask = kQuadCenterBackMask,
                });

    // The same rate, count, and precision do not match a different key: zero
    // is unassigned at this model boundary, never a count-based inference.
    CHECK_FALSE(cache.find(quadSource(0)).has_value());

    auto const unassignedClient =
      PcmFormat{.sampleRate = 48000, .channels = 4, .encoding = SampleEncoding::Signed24In32Le};
    cache.store(quadSource(0), unassignedClient);
    auto const optUnassigned = cache.find(quadSource(0));
    REQUIRE(optUnassigned);
    CHECK(*optUnassigned == unassignedClient);
    CHECK(optUnassigned->speakerMask == 0);
    // An explicitly stored count-only hint remains usable; caching it does not
    // invent speaker identities from either neighboring entry.
    auto const optBackPair = cache.find(quadSource(kQuadBackPairMask));
    REQUIRE(optBackPair);
    CHECK(optBackPair->speakerMask == kQuadBackPairMask);
  }

  TEST_CASE("AlsaPrewarmCache - overwriting one signal leaves the other entry intact")
  {
    auto cache = AlsaPrewarmCache{};

    auto const backPairClient = PcmFormat{
      .sampleRate = 48000,
      .channels = 4,
      .encoding = SampleEncoding::Signed24In32Le,
      .speakerMask = kQuadBackPairMask,
    };
    auto const centerBackClient = PcmFormat{
      .sampleRate = 48000,
      .channels = 4,
      .encoding = SampleEncoding::Signed24PackedLe,
      .speakerMask = kQuadCenterBackMask,
    };
    cache.store(quadSource(kQuadBackPairMask), backPairClient);
    cache.store(quadSource(kQuadCenterBackMask), centerBackClient);

    auto const replacement = PcmFormat{
      .sampleRate = 48000,
      .channels = 4,
      .encoding = SampleEncoding::Signed32Le,
      .speakerMask = kQuadBackPairMask,
    };
    cache.store(quadSource(kQuadBackPairMask), replacement);

    auto const optBackPair = cache.find(quadSource(kQuadBackPairMask));
    REQUIRE(optBackPair);
    CHECK(*optBackPair == replacement);

    auto const optCenterBack = cache.find(quadSource(kQuadCenterBackMask));
    REQUIRE(optCenterBack);
    CHECK(*optCenterBack == centerBackClient);
  }

  TEST_CASE("AlsaPrewarmCache - a full cache evicts the oldest stored signal")
  {
    constexpr std::uint32_t kFirstRate = 48000;
    auto cache = AlsaPrewarmCache{};
    auto const sourceAt = [](std::uint32_t rate)
    { return SignalFormat{.sampleRate = rate, .channels = 2, .precisionBits = 16}; };
    auto const clientAt = [](std::uint32_t rate)
    { return PcmFormat{.sampleRate = rate, .channels = 2, .encoding = SampleEncoding::Signed16Le}; };
    auto const newestRate = kFirstRate + static_cast<std::uint32_t>(AlsaPrewarmCache::kCapacity);

    for (auto rate = kFirstRate; rate < newestRate; ++rate)
    {
      cache.store(sourceAt(rate), clientAt(rate));
    }

    REQUIRE(cache.find(sourceAt(kFirstRate)) == clientAt(kFirstRate));
    cache.store(sourceAt(newestRate), clientAt(newestRate));

    CHECK_FALSE(cache.find(sourceAt(kFirstRate)));

    for (auto rate = kFirstRate + 1U; rate <= newestRate; ++rate)
    {
      CHECK(cache.find(sourceAt(rate)) == clientAt(rate));
    }
  }
} // namespace ao::audio::backend::detail::test
