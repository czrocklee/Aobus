// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "lib/audio/NullBackend.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/audio/Engine.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace ao::audio::test
{
  TEST_CASE("Engine - different PCM identities require a drained transition", "[audio][integration][engine][gapless]")
  {
    auto const first = ao::test::TempFile{
      ao::test::wav::makeWav({.channels = 2, .sampleRate = 48000, .audioData = {0x11, 0x12, 0x23, 0x24}}), ".wav"};
    auto nextSpec = ao::test::wav::Spec{.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                        .channels = 2,
                                        .sampleRate = 48000,
                                        .channelMask = 0x30,
                                        .audioData = {0x35, 0x36, 0x47, 0x48}};

    SECTION("known alternate roles")
    {
    }

    SECTION("different rate")
    {
      nextSpec.sampleRate = 44100;
      nextSpec.channelMask = 0x3;
    }

    SECTION("different count")
    {
      nextSpec.channels = 1;
      nextSpec.channelMask = 0x4;
      nextSpec.audioData = {0x35, 0x36};
    }

    auto const next = ao::test::TempFile{ao::test::wav::makeWav(nextSpec), ".wav"};
    auto engine = Engine{std::make_unique<NullBackend>(), makeEngineTestDevice()};
    engine.play(makePlaybackItem(first.path));
    REQUIRE(engine.status().transport == Transport::Playing);
    auto const preparedRes = engine.setNext(makePlaybackItem(next.path));
    REQUIRE(preparedRes);
    CHECK(preparedRes->transition == Engine::PreparedTransitionMode::DrainFallback);
  }
} // namespace ao::audio::test
