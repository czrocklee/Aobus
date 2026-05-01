// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <platform/linux/playback/PipeWireBackend.h>
#include <rs/audio/IBackend.h>
#include <rs/audio/Engine.h>
#include <rs/utility/Log.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include <chrono>
#include <thread>

namespace rs::audio
{
  TEST_CASE("Graph Analysis Verification", "[playback][graph][debug]")
  {
    Log::init();
    APP_LOG_INFO("Starting Graph Analysis Verification Test");

    // Use PipeWire backend
    auto backend = std::make_unique<app::rs::audio::PipeWireBackend>();
    auto engine = std::make_unique<Engine>(std::move(backend));

    // Prepare a track (using a known test file if available, otherwise just use a dummy path to trigger analysis)
    TrackPlaybackDescriptor descriptor;
    descriptor.title = "Verification Track";
    descriptor.artist = "Debug Artist";
    descriptor.filePath = "/home/rocklee/Music/song_1.flac"; // Adjusted to likely existing path
    descriptor.sampleRateHint = 44100;
    descriptor.channelsHint = 2;
    descriptor.bitDepthHint = 16;

    if (std::filesystem::exists(descriptor.filePath))
    {
      APP_LOG_INFO("Triggering playback for graph analysis...");
      engine->play(descriptor);

      // Wait a few seconds for PipeWire to renegotiate and the engine to analyze the graph multiple times
      for (int i = 0; i < 5; ++i)
      {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto snap = engine->snapshot();
        APP_LOG_INFO("Current Quality Conclusion: {} ({})", static_cast<int>(snap.quality), snap.qualityTooltip);
      }

      engine->stop();
    }
    else
    {
      WARN("Test file not found: " << descriptor.filePath << ". Skipping real analysis.");
    }

    Log::shutdown();
  }
} // namespace rs::audio
