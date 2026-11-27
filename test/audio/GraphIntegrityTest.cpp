// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/audio/NullBackend.h"
#include "test/audio/AudioFixtureDirectory.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/Engine.h>
#include <ao/audio/PlaybackInput.h>
#include <ao/audio/Transport.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

namespace ao::audio::test
{
  TEST_CASE("Engine - null output publishes decoded formats without a native route anchor")
  {
    auto const testFile = audioFixtureDirectory() / "basic_metadata.flac";

    INFO("Required audio fixture: " << testFile);
    REQUIRE(std::filesystem::is_regular_file(testFile));

    auto engine = Engine{std::make_unique<NullBackend>(), {.id = DeviceId{"null"}, .backendId = kBackendNone}};
    engine.play({.id = Engine::PlaybackItemId{1}, .input = PlaybackInput{.filePath = testFile.string()}});
    REQUIRE(engine.transport() == Transport::Playing);

    auto const route = engine.routeStatus();
    CHECK(route.generation == engine.playbackGeneration());
    CHECK_FALSE(route.optAnchor);
    CHECK(route.state.sourceFormat.sampleRate == 44100);
    CHECK(route.state.sourceFormat.channels == 2);
    CHECK(route.state.sourceFormat.precisionBits == 16);
    CHECK(route.state.engineOutputFormat.sampleRate == 44100);
    CHECK(route.state.engineOutputFormat.channels == 2);
    CHECK(route.state.engineOutputFormat == route.state.decoderOutputFormat);
  }
} // namespace ao::audio::test
