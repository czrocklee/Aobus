// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Types.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>

namespace ao::audio::test
{
  TEST_CASE("Engine - Graph Integrity", "[playback][integration][graph]")
  {
    auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";

    if (!std::filesystem::exists(testFile))
    {
      WARN("Test file not found, skipping Graph Integrity test");
      return;
    }

    auto backend = std::make_unique<NullBackend>();
    auto const device = Device{.id = DeviceId{"null"},
                               .displayName = "Null",
                               .description = "Null",
                               .isDefault = false,
                               .backendId = kBackendNone};

    auto engine = Engine{std::move(backend), device};

    auto const descriptor =
      TrackPlaybackDescriptor{.filePath = testFile.string(), .title = "Test Title", .artist = "Test Artist"};

    engine.play(descriptor);

    // Wait for the engine to open the track and populate route state
    bool routeReady = false;

    for (int i = 0; i < 50; ++i)
    {
      auto const snap = engine.status();

      if (snap.routeState.sourceFormat.sampleRate != 0)
      {
        routeReady = true;
        break;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    SECTION("RouteState has decoder source format")
    {
      REQUIRE(routeReady);
      auto const snap = engine.status();
      CHECK(snap.routeState.sourceFormat.sampleRate == 44100);
      CHECK(snap.routeState.sourceFormat.channels == 2);
      CHECK(snap.routeState.sourceFormat.bitDepth == 16);
    }

    SECTION("RouteState has engine output format")
    {
      REQUIRE(routeReady);
      auto const snap = engine.status();
      CHECK(snap.routeState.engineOutputFormat.sampleRate == 44100);
      CHECK(snap.routeState.engineOutputFormat.channels == 2);
    }

    engine.stop();
  }
} // namespace ao::audio::test
