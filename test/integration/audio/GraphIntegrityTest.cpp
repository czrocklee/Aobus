// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/audio/Engine.h>
#include <ao/audio/NullBackend.h>
#include <ao/utility/IMainThreadDispatcher.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace ao::audio;

namespace
{
  class ImmediateDispatcher final : public ao::IMainThreadDispatcher
  {
  public:
    void dispatch(std::function<void()> callback) override { callback(); }
  };
}

TEST_CASE("Engine - Graph Integrity", "[playback][integration][graph]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";

  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found, skipping Graph Integrity test");
    return;
  }

  auto const dispatcher = std::make_shared<ImmediateDispatcher>();
  auto backend = std::make_unique<NullBackend>();
  auto const device = Device{.id = DeviceId{"null"},
                             .displayName = "Null",
                             .description = "Null",
                             .isDefault = false,
                             .backendId = kBackendNone};

  auto engine = Engine{std::move(backend), device, dispatcher};

  auto const descriptor =
    TrackPlaybackDescriptor{.filePath = testFile.string(), .title = "Test Title", .artist = "Test Artist"};

  engine.play(descriptor);

  // Wait for the engine to open the track and populate route state
  auto routeReady = false;

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
