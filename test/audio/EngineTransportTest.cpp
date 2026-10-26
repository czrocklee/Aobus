// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EngineTestSupport.h"
#include "FakeCapturingBackend.h"
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
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "test.flac"}));
    REQUIRE(engine.status().transport == Transport::Playing);
    backend->clearEvents();
    engine.resume();
    CHECK(engine.status().transport == Transport::Playing);
    CHECK(backend->events().empty());

    engine.pause();
    engine.resume();
    auto const events = backend->events();
    REQUIRE(events.size() == 2);
    CHECK(events[0].name == "pause");
    CHECK(events[1].name == "resume");
    CHECK(engine.status().transport == Transport::Playing);
  }

  TEST_CASE("Engine - pause on idle engine does nothing", "[audio][unit][engine][transport]")
  {
    auto const device = makeEngineTestDevice();
    auto backendPtr = std::make_unique<FakeCapturingBackend>();
    auto* const backend = backendPtr.get();
    auto engine = Engine{std::move(backendPtr), device, makeScriptedEngineDecoderFactory()};

    engine.stop();
    backend->clearEvents();
    engine.pause();
    CHECK(engine.status().transport == Transport::Idle);
    CHECK(backend->events().empty());

    engine.play(makePlaybackItem(PlaybackInput{.filePath = "test.flac"}));
    REQUIRE(engine.status().transport == Transport::Playing);
    backend->clearEvents();
    engine.pause();
    auto const events = backend->events();
    REQUIRE(events.size() == 1);
    CHECK(events.front().name == "pause");
    CHECK(engine.status().transport == Transport::Paused);
  }
} // namespace ao::audio::test
