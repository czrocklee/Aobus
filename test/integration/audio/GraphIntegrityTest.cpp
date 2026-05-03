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
  class ImmediateDispatcher : public ao::IMainThreadDispatcher
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

  auto dispatcher = std::make_shared<ImmediateDispatcher>();
  auto backend = std::make_unique<NullBackend>();
  Device device{.id = DeviceId{"null"},
                .displayName = "Null",
                .description = "Null",
                .isDefault = false,
                .backendId = kBackendNone};

  Engine engine(std::move(backend), device, dispatcher);

  TrackPlaybackDescriptor descriptor;
  descriptor.filePath = testFile.string();
  descriptor.title = "Test Title";
  descriptor.artist = "Test Artist";

  engine.play(descriptor);

  // Wait for the engine to open the track and populate the graph
  // We need to wait because it happens on a background thread
  bool decoderFound = false;
  bool engineFound = false;

  for (int i = 0; i < 50; ++i)
  {
    auto snap = engine.status();
    decoderFound = false;
    engineFound = false;

    for (auto const& node : snap.flow.nodes)
    {
      if (node.id == "rs-decoder") decoderFound = true;
      if (node.id == "rs-engine") engineFound = true;
    }

    if (decoderFound && engineFound) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  SECTION("rs-decoder is present in the graph")
  {
    CHECK(decoderFound);
  }

  SECTION("rs-engine is present in the graph")
  {
    CHECK(engineFound);
  }

  SECTION("rs-decoder has valid format")
  {
    auto snap = engine.status();
    auto it = std::ranges::find(snap.flow.nodes, "rs-decoder", &ao::audio::flow::Node::id);
    if (it != snap.flow.nodes.end())
    {
      REQUIRE(it->optFormat);
      CHECK(it->optFormat->sampleRate == 44100);
      CHECK(it->optFormat->channels == 2);
      CHECK(it->optFormat->bitDepth == 16);
    }
  }

  engine.stop();
}
