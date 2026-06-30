// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/AudioRouteFormatState.h>
#include <ao/audio/Backend.h>
#include <ao/audio/Engine.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/PlaybackInput.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace ao::audio::test
{
  namespace
  {
    class RouteStateObserver final
    {
    public:
      void observe(AudioRouteFormatState const& state)
      {
        auto lock = std::scoped_lock{_mutex};
        _optLastState = state;

        if (state.sourceFormat.sampleRate != 0)
        {
          _optReadyState = state;
          _cv.notify_all();
        }
      }

      std::optional<AudioRouteFormatState> waitForReady(std::chrono::milliseconds timeout)
      {
        auto lock = std::unique_lock{_mutex};
        _cv.wait_for(lock, timeout, [this] { return _optReadyState != std::nullopt; });
        return _optReadyState;
      }

      std::string describeLastState() const
      {
        auto lock = std::scoped_lock{_mutex};

        if (!_optLastState)
        {
          return "Route state was not observed";
        }

        return "Last route state: source sample rate=" + std::to_string(_optLastState->sourceFormat.sampleRate) +
               ", source channels=" + std::to_string(_optLastState->sourceFormat.channels) +
               ", engine sample rate=" + std::to_string(_optLastState->engineOutputFormat.sampleRate) +
               ", engine channels=" + std::to_string(_optLastState->engineOutputFormat.channels);
      }

    private:
      mutable std::mutex _mutex;
      std::condition_variable _cv;
      std::optional<AudioRouteFormatState> _optLastState;
      std::optional<AudioRouteFormatState> _optReadyState;
    };
  } // namespace

  TEST_CASE("Engine - Graph Integrity", "[playback][integration][graph]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.flac";

    if (!std::filesystem::exists(testFile))
    {
      WARN("Test file not found, skipping Graph Integrity test");
      return;
    }

    auto backendPtr = std::make_unique<NullBackend>();
    auto const device = Device{.id = DeviceId{"null"},
                               .displayName = "Null",
                               .description = "Null",
                               .isDefault = false,
                               .backendId = kBackendNone};

    auto routeState = RouteStateObserver{};
    auto engine = Engine{std::move(backendPtr), device};

    auto const descriptor = PlaybackInput{.filePath = testFile.string()};
    engine.setOnRouteChanged([&routeState](Engine::RouteStatus const& route) { routeState.observe(route.state); });

    engine.play(descriptor);
    routeState.observe(engine.status().routeState);

    auto const optRouteState = routeState.waitForReady(std::chrono::seconds{1});
    INFO("Expected route state with decoder source format after 1s; " << routeState.describeLastState());
    REQUIRE(optRouteState);

    SECTION("AudioRouteFormatState has decoder source format")
    {
      CHECK(optRouteState->sourceFormat.sampleRate == 44100);
      CHECK(optRouteState->sourceFormat.channels == 2);
      CHECK(optRouteState->sourceFormat.bitDepth == 16);
    }

    SECTION("AudioRouteFormatState has engine output format")
    {
      CHECK(optRouteState->engineOutputFormat.sampleRate == 44100);
      CHECK(optRouteState->engineOutputFormat.channels == 2);
    }

    engine.stop();
  }
} // namespace ao::audio::test
