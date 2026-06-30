// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CapturingBackend.h"
#include "EngineTestSupport.h"
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

namespace ao::audio::test
{
  TEST_CASE("Engine - resume on already playing engine does nothing", "[audio][unit][engine][transport]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(PlaybackInput{.filePath = "test.flac"});
    CHECK(engine.status().transport == Transport::Playing);
    engine.resume();
    CHECK(engine.status().transport == Transport::Playing);
  }

  TEST_CASE("Engine - pause on idle engine does nothing", "[audio][unit][engine][transport]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<CapturingBackend>();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.stop();
    engine.pause();
    CHECK(engine.status().transport == Transport::Idle);
  }
} // namespace ao::audio::test
