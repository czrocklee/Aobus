// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/audio/Engine.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/backend/PipeWireBackend.h>
#include <ao/utility/IMainThreadDispatcher.h>
#include <ao/utility/Log.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>

namespace ao::audio
{
  namespace
  {
    class ImmediateDispatcher : public ao::IMainThreadDispatcher
    {
    public:
      void dispatch(std::function<void()> callback) override { callback(); }
    };
  }

  TEST_CASE("Graph Analysis Verification", "[playback][graph][debug][audio]")
  {
    ao::log::Log::init(ao::log::LogLevel::Info);

    // Use PipeWire backend
    ao::audio::Device dummyDevice;
    ao::audio::ProfileId dummyProfile = kProfileShared;
    auto backend = std::make_unique<backend::PipeWireBackend>(dummyDevice, dummyProfile);

    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto engine = std::make_unique<Engine>(std::move(backend), dummyDevice, dispatcher);

    // Prepare a track
    TrackPlaybackDescriptor descriptor;
    descriptor.title = "Verification Track";
    descriptor.artist = "Debug Artist";
    descriptor.filePath = "/home/rocklee/Music/song_1.flac"; // Adjusted to likely existing path
    descriptor.sampleRateHint = 44100;
    descriptor.channelsHint = 2;
    descriptor.bitDepthHint = 16;

    if (std::filesystem::exists(descriptor.filePath))
    {
      AUDIO_LOG_INFO("Triggering playback for graph analysis...");
      engine->play(descriptor);

      // Wait a few seconds for PipeWire to renegotiate and the engine to analyze the graph multiple times
      for (int i = 0; i < 5; ++i)
      {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto const snap = engine->status();
        AUDIO_LOG_INFO("Graph Snapshot: {} nodes", snap.flow.nodes.size());
      }

      engine->stop();
    }
    else
    {
      WARN("Test file not found: " << descriptor.filePath << ". Skipping real analysis.");
    }
  }
} // namespace ao::audio
